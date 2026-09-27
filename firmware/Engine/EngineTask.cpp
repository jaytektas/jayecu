#include "EngineTask.h"
#include "well_known_signals.h"   // wk:: roles — rename-safe signal bindings
#include "../Diagnostics/DtcManager.h"        // injected dtc_ methods + DtcSource / FF channels
#include "../Led/LedStatus.h"
#include "../Storage/SdArbitrator.h"
#include "../Platform/platform_hal.h"
#include "../Diagnostics/CpuStats.h"
#include "../Diagnostics/ReconfigCost.h"
#include "../Scheduler/Log.h"                  // EFI_LOG_WARN — flag a dropped participant loudly
#include "../../generated/ecu_config.h"        // g_config — live firing order for the fault INDICATOR

// SD card ownership arbitrator (defined in main.cpp). Driven here from the key state.
extern SdArbitrator g_sd_arb;


// Key thresholds on battery voltage, with hysteresis (kills chatter, rides cranking dips).
// Key-on thresholds moved to Sensors (Sensors::KEY_ON_V/KEY_OFF_V) with the decision itself.

// Control-frame cadence (ms). 1 kHz target freshness; the crank ISR applies at exact angle.
static constexpr uint32_t ENGINE_FRAME_PERIOD_MS = 1;

EngineTask::EngineTask(EnginePositionHal& epos_hal, CanBroker& can_broker)
    : epos_hal_(epos_hal), can_broker_(can_broker) {}

void EngineTask::add_participant(EngineModule* m, const char* name, Phase phase,
                                 Cadence cadence, uint16_t cadence_hz) {
    const uint8_t c = static_cast<uint8_t>(cadence);
    const uint8_t p = static_cast<uint8_t>(phase);
    if (!m || c >= CADENCE_COUNT || p >= PHASE_COUNT) return;
    if (m->sched_linked_) {
        // A module belongs to exactly one (cadence, phase) list — it carries a single intrusive link.
        // Adding it twice would splice one list into another; refuse and shout (a compose-time bug).
        EFI_LOG_WARN("compose", "participant '%s' added more than once — ignoring the duplicate", name ? name : "?");
        return;
    }
    m->sched_next_   = nullptr;
    m->sched_linked_ = true;
    m->sched_name_   = name ? name : "?";

    // Cadence -> decimation + publish ttl. PER_CYCLE participants aren't frame-gated (the cycle gates them):
    // period 1, and a per-cycle ttl. KHZ_1 participants run every ⌈1000/hz⌉ frames, phase-staggered so
    // same-rate modules don't bunch onto the same frame, with a ttl matched to the effective rate.
    if (cadence == Cadence::PER_CYCLE) {
        m->sched_period_ = 1;
        m->sched_phase_  = 0;
        m->sched_ttl_    = TTL_CYCLE_MS;
    } else {
        const uint16_t hz = cadence_hz ? cadence_hz : 1000;
        uint32_t period = (1000u + hz / 2u) / hz;                 // round(1000/hz) frames
        if (period < 1u) period = 1u;
        m->sched_period_ = static_cast<uint16_t>(period);
        m->sched_phase_  = static_cast<uint16_t>(add_seq_ % period);   // stagger consecutive adds
        m->sched_ttl_    = ttl_for(static_cast<uint16_t>(1000u / period));   // ttl of the ACTUAL frame rate
    }
    ++add_seq_;

    if (!phase_head_[c][p]) phase_head_[c][p] = m;                 // first in this list
    else                    phase_tail_[c][p]->sched_next_ = m;    // append — preserves registration order
    phase_tail_[c][p] = m;
}

void EngineTask::run_phase(Cadence cadence, Phase phase, const EnginePosition& pos, EngineFrame& frame) {
    const uint8_t c = static_cast<uint8_t>(cadence);
    const uint8_t p = static_cast<uint8_t>(phase);
    for (EngineModule* m = phase_head_[c][p]; m; m = m->sched_next_) {
        // Cadence gate: run a KHZ_1 module only on its staggered frames (period 1 / phase 0 = every frame).
        // PER_CYCLE modules have period 1 → this is a no-op, they run every cycle.
        if (m->sched_period_ > 1 && (frame_count_ % m->sched_period_) != m->sched_phase_) continue;
        const uint32_t t0 = platform_cyccnt();
        m->update(pos, signal_bus_, frame);
        const uint32_t used = platform_cyccnt() - t0;
        m->sched_cyc_ = m->sched_cyc_ ? (m->sched_cyc_ - m->sched_cyc_ / 8u + used / 8u) : used;
    }
}

void EngineTask::dispatch_lifecycle(Cadence cadence, bool started) {
    const uint8_t c = static_cast<uint8_t>(cadence);
    for (uint8_t p = 0; p < PHASE_COUNT; p++)
        for (EngineModule* m = phase_head_[c][p]; m; m = m->sched_next_) {
            if (started) m->on_engine_start();
            else         m->on_engine_stop();
        }
}

void EngineTask::start(uint8_t num_cylinders) {
    num_cylinders_ = num_cylinders;

    // Per-cycle task FIRST so its handle exists before the 1 kHz task can notify it.
    // Prio 2: BELOW the 1 kHz frame (3) — the heavy base runs in the inter-tick slack.
    cycle_task_handle_ = xTaskCreateStatic(
        cycle_task_fn, "EngCycle",
        sizeof(cycle_stack_) / sizeof(cycle_stack_[0]),
        this, 2,
        cycle_stack_, &cycle_tcb_);

    task_handle_ = xTaskCreateStatic(
        task_fn, "Engine",
        sizeof(task_stack_) / sizeof(task_stack_[0]),
        this, 3,
        task_stack_, &task_tcb_);
}

void EngineTask::task_fn(void* pv) {
    static_cast<EngineTask*>(pv)->run();
}

void EngineTask::cycle_task_fn(void* pv) {
    static_cast<EngineTask*>(pv)->run_cycle();
}

void EngineTask::run() {
    epos_hal_.start();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(ENGINE_FRAME_PERIOD_MS));
        // MEASURED HERE AND NOWHERE ELSE: everything the frame does is inside run_frame(), and the
        // vTaskDelay above is the part that is deliberately NOT ours. Two DWT reads per frame — the
        // counter is free-running whatever we do, so this costs two loads and a subtract.
        // WHICH FRAME ABSORBS A TUNE WRITE: the first one to run after the generation moved, because
        // that is the one every rebuilder notices it in. Comparing the generation across run_frame()
        // does not find it — the bump happens on the comms task, between frames — so the comparison is
        // against what the PREVIOUS frame saw.
        extern volatile uint32_t g_config_generation;
        static uint32_t gen_seen = 0;
        const uint32_t gen_now = g_config_generation;
        const bool reconfig_frame = (gen_now != gen_seen);
        gen_seen = gen_now;
        if (reconfig_frame) reconfig::begin();          // last time's figures are not this time's
        const uint32_t t0 = platform_cyccnt();
        run_frame();
        const uint32_t used = platform_cyccnt() - t0;   // unsigned: wrap-safe like every other user
        if (reconfig_frame) reconfig::note(reconfig::FRAME, used);
        // THE FRAME THAT ABSORBED A TUNE WRITE, whole. The per-rebuilder figures say what each one
        // spent; this says what the frame actually took, so the difference between them is the part
        // nobody has claimed yet.
        // 1/8 EMA for the gauge (a single long frame should move it, not own it), and the WORST frame
        // kept raw — an average of 30 % with a 120 % spike is a missed frame, and the average hides it.
        frame_cyc_ema_ = frame_cyc_ema_ ? (frame_cyc_ema_ - frame_cyc_ema_ / 8u + used / 8u) : used;
        // THE FIRST FRAMES ARE NOT THE STEADY STATE. Everything one-time lands in the first one —
        // measured on the bench at over 2.55 ms, which saturated the peak and left it reading 255 %
        // for ever after, which is a number nobody can act on. A tenth of a second in, the frame is
        // doing what it will do from then on. ('cpu reset' re-arms it from wherever you are now.)
        if (frame_wakes_ > 100u && used > frame_cyc_max_) frame_cyc_max_ = used;
    }
}

// Cycles -> percent of the frame PERIOD. Not of the CPU: 100 % here means the frame body filled its
// millisecond, which is the point at which the next one is late.
static uint8_t cyc_to_frame_pct(uint32_t cyc) {
    const uint32_t hz = platform_cpu_hz();
    if (!hz) return 0;                                       // host build: no cycle counter
    const uint32_t per_frame = (hz / 1000u) * ENGINE_FRAME_PERIOD_MS;
    if (!per_frame) return 0;
    const uint32_t pct = (cyc * 100u + per_frame / 2u) / per_frame;
    return pct > 255u ? 255u : static_cast<uint8_t>(pct);    // over 100 IS the report, not an error
}
uint8_t EngineTask::frame_load_pct()     const noexcept { return cyc_to_frame_pct(frame_cyc_ema_); }
uint8_t EngineTask::frame_load_max_pct() const noexcept { return cyc_to_frame_pct(frame_cyc_max_); }
// Every participant, heaviest first, in microseconds — `frame` on the CLI. Walked rather than sorted
// into storage: there is no array of participants by design (each module carries its own link), and a
// selection sort over a list nobody is timing is cheaper than keeping one.
uint8_t EngineTask::frame_costs(FrameCost* out, uint8_t max) const {
    const uint32_t hz = platform_cpu_hz();
    if (!out || !max || !hz) return 0;
    const uint32_t per_us = hz / 1000000u;
    uint8_t n = 0;
    for (uint8_t c = 0; c < CADENCE_COUNT; c++)
        for (uint8_t p = 0; p < PHASE_COUNT; p++)
            for (const EngineModule* m = phase_head_[c][p]; m; m = m->sched_next_) {
                const uint32_t us = per_us ? m->sched_cyc_ / per_us : 0u;
                const bool pc = (c == static_cast<uint8_t>(Cadence::PER_CYCLE));
                if (n < max) { out[n++] = { m->sched_name_, us, m->sched_period_, pc }; }
                else {
                    uint8_t worst = 0;
                    for (uint8_t k = 1; k < n; k++) if (out[k].us < out[worst].us) worst = k;
                    if (us > out[worst].us) out[worst] = { m->sched_name_, us, m->sched_period_, pc };
                }
            }
    for (uint8_t a = 1; a < n; a++) {                      // heaviest first
        const FrameCost key = out[a];
        int8_t b = static_cast<int8_t>(a) - 1;
        for (; b >= 0 && out[b].us < key.us; b--) out[b + 1] = out[b];
        out[b + 1] = key;
    }
    return n;
}

uint32_t EngineTask::frame_max_us() const noexcept {
    const uint32_t hz = platform_cpu_hz();
    return hz ? frame_cyc_max_ / (hz / 1000000u) : 0u;
}

// The per-cycle compute cadence. Blocks on the 1 kHz task's notify (50 ms timeout fallback so it
// keeps ticking while stopped/cranking). Runs the PER_CYCLE participant list (the heavy fuel base —
// FuelCalculator) against a fresh position snapshot, then commits the INJECTION shadow. Reads its
// cross-cadence inputs (fuel-cut, corrections) from the bus, not the 1 kHz frame.
void EngineTask::run_cycle() {
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        per_cycle_wakes_++;

        // Deliver any engine-state edge the 1 kHz task flagged, to the PER_CYCLE participants, here on
        // THIS task — so on_engine_start/on_engine_stop never race their update(). Stop is applied
        // before start (chronological order if both advanced since the last wake). The 50 ms timeout
        // guarantees the stop edge is seen even after cycle boundaries cease.
        if (stop_gen_  != seen_stop_gen_ ) { seen_stop_gen_  = stop_gen_;  dispatch_lifecycle(Cadence::PER_CYCLE, /*started=*/false); }
        if (start_gen_ != seen_start_gen_) { seen_start_gen_ = start_gen_; dispatch_lifecycle(Cadence::PER_CYCLE, /*started=*/true);  }

        // Snapshot the firing epoch BEFORE computing. arm_firing() below re-enables firing only if
        // this epoch is still live at commit time — so a sync transition during the compute (the
        // 20k-RPM fast-resync case) invalidates the arm and the stale schedule can't fire.
        const uint32_t fire_epoch = epos_hal_.firing_epoch();

        const EnginePosition pos = read_position_raw();
        EngineFrame frame{};
        run_phase(Cadence::PER_CYCLE, Phase::INPUT,  pos, frame);
        run_phase(Cadence::PER_CYCLE, Phase::MODULE, pos, frame);
        run_phase(Cadence::PER_CYCLE, Phase::OUTPUT, pos, frame);
        extern bool g_system_active;   // key-on gate: no fuel/spark commit or prime while on USB/bench
        if (g_system_active &&
            phase_head_[static_cast<uint8_t>(Cadence::PER_CYCLE)][static_cast<uint8_t>(Phase::MODULE)] != nullptr) {
            commit_ignition(frame);    // spark/dwell (Ignition, PER_CYCLE)
            commit_injection(frame);   // open-angle/PW (FuelCalculator, PER_CYCLE)
            epos_hal_.arm_firing(fire_epoch);   // a real schedule is now in the shadow for this epoch
            // One-shot cold-start prime (FuelCalculator latches it to once per power-up) — fired via
            // the HAL, which squirts all injectors simultaneously on the next grid tooth.
            if (frame.prime_pw_us > 0) epos_hal_.prime_injectors(frame.prime_pw_us);
        }

        signal_bus_.set_u32(wk::per_cycle_count, per_cycle_wakes_, true, platform_get_tick_ms());   // counter -> integer cell (see frame_count)
    }
}

void EngineTask::run_frame() {
    frame_wakes_++;                                  // diagnostic: actual 1 kHz loop rate
    ++frame_count_;                                  // cadence gate base (run_phase decimates KHZ_1 modules)
    EngineFrame frame{};
    const EnginePosition pos = read_position();

    // The per-cycle task is no longer woken from here. It's clocked by the crank: the decoder pends
    // a soft IRQ at each cycle boundary (see the wake relay in main.cpp), which does the FromISR
    // notify. This frame stays out of the engine-event business — so decimating it later can't drag
    // the per-cycle cadence with it. While stopped (no crank) the per-cycle task's own 50 ms timeout
    // keeps it ticking (prime/state staging). cycle_count is still tracked below for telemetry.

    // --- Engine run-state machine — the single authoritative STOPPED/CRANKING/RUNNING decision,
    //     published to the bus BEFORE any module runs so both cadences read a coherent state. On a
    //     transition, dispatch the lifecycle edge to KHZ_1 participants inline (same task), and bump
    //     the generation counters the per-cycle task observes to deliver the same edge to PER_CYCLE
    //     participants on its own task (race-free). g_engine_running subsumes the old rpm>50 flag:
    //     "turning" (cranking OR running) — which is what main.cpp's reconfigure/storage gate means. ---
    bool started_edge = false, stopped_edge = false;
    const EngineRunState st = state_machine_.update(pos.rpm, started_edge, stopped_edge);
    signal_bus_.set(wk::engine_state, static_cast<float>(st), true, platform_get_tick_ms());
    extern bool g_engine_running;
    g_engine_running = (st != EngineRunState::STOPPED);
    if (stopped_edge) { dispatch_lifecycle(Cadence::KHZ_1, /*started=*/false); stop_gen_++;  }
    if (started_edge) { dispatch_lifecycle(Cadence::KHZ_1, /*started=*/true);  start_gen_++; }

    // --- INPUT phase: producers fill the bus (sensors / inputs) ---
    run_phase(Cadence::KHZ_1, Phase::INPUT, pos, frame);

    extern bool g_system_active;    // owned by Sensors (INPUT phase); read here, never written
    // SD card ownership follows the key. The key is DECIDED IN THE INPUT PHASE now, by Sensors, right
    // after the battery it is derived from publishes -- see Sensors::key_on(). It used to be decided
    // here, one phase later, which left a single frame on every key-on and every boot where the sensors
    // had already been skipped as key-off but the modules ran anyway and found their inputs missing.
    // Reading it here instead of recomputing it keeps one owner: a second hysteresis on the same
    // battery could disagree with the first about exactly when the key turned.
    key_on_ = g_system_active;
    g_sd_arb.service(key_on_, platform_get_tick_ms());

    // system-active = key-on: the master ACTUATION gate, set by Sensors in the INPUT phase above.
    // Closing the firing gate stops spark/injection immediately (a bench trigger on USB can't drive
    // the coils/injectors); the per-cycle task also skips its fuel/spark commit while inactive.
    epos_hal_.set_firing_gate(key_on_);
    // Cut requesters (rev limiter, launch, flat shift, pit limiter, overboost, EngineProtection)
    // publish wk::ign_cut / wk::fuel_cut; validity IS the OR. Push them to the output stage, which is
    // where a cut belongs — see EnginePositionHal::set_output_cuts.
    // …and the two OPERATOR gateways, which join the same OR rather than getting a path of their own:
    // engine.ign_enable / engine.inj_enable, off = that half of the engine does not fire. Crank with no
    // fuel to look for spark, or with no spark to check fuel delivery and clear a flood. They persist
    // through a reset, which is the point and also the hazard. They need no channel of their own to be
    // visible: ign_exec_mask / inj_exec_mask already go to zero here, and the firmware's own note calls
    // those "the only thing in a datalog that says why nothing came out". Two more channels saying the
    // same thing cost telemetry bytes and pushed a Diagnostics column past its card.
    const bool ign_on = g_config.engine.ign_enable != 0;
    const bool inj_on = g_config.engine.inj_enable != 0;
    const bool cut_ign = signal_bus_.valid(wk::ign_cut) || !ign_on;
    const bool cut_inj = signal_bus_.valid(wk::fuel_cut) || !inj_on;
    epos_hal_.set_output_cuts(cut_ign, cut_inj);

    // Per-cylinder cuts, composed with the global cuts inside the scheduler rather than overwriting
    // them, so a rev limiter and a cylinder cut can both hold. FUEL and SPARK separately:
    //   pre-ignition (Knock) — fuel and/or spark, as its two settings say;
    //   misfire — fuel only ("stop injecting into a dead cylinder"). This mask used to be published
    //     and read by nothing, so Cut Fuel to a Dead Cylinder did nothing at all.
    {
        const uint16_t pre = static_cast<uint16_t>(signal_bus_.get(SIG_PREIGN_CUT_MASK, 0.0f));
        const uint16_t mis = static_cast<uint16_t>(signal_bus_.get(SIG_MISFIRE_CUT_MASK, 0.0f));
        const uint16_t fuel  = static_cast<uint16_t>((g_config.knock.preign_cut_fuel  ? pre : 0u) | mis);
        const uint16_t spark = static_cast<uint16_t>(g_config.knock.preign_cut_spark ? pre : 0u);
        epos_hal_.set_cylinder_cuts(fuel, spark);
    }
    // Publish the resulting mask. The commanded advance and pulse width no longer go to zero during a
    // cut -- they report what the tables asked for -- so this is the only thing in a datalog that
    // says why nothing came out.
    signal_bus_.set(SIG_IGN_EXEC_MASK, cut_ign ? 0.0f : float((1u << MAX_IGN_CHANNELS) - 1u),
                    true, platform_get_tick_ms());
    signal_bus_.set(SIG_INJ_EXEC_MASK, cut_inj ? 0.0f : float((1u << MAX_INJ_CHANNELS) - 1u),
                    true, platform_get_tick_ms());
    if (dtc_) dtc_->set_active(key_on_);   // USB/bench: runtime DTCs off, config/validity still flag
    // …and age out anything nobody is still asserting, the way the bus expires a value its producer
    // stopped writing. The table is the only place that can catch a judge that has gone quiet, whatever
    // the reason it went quiet.
    //
    // AT 10 Hz, NOT AT 1 kHz. It was written into the frame body, which swept sixty-four records a
    // thousand times a second to enforce deadlines that are never shorter than one second. A hundred
    // milliseconds of slack on a one-second ttl is not a property anybody can observe, and the sweep
    // is then 1 % of what it was. (The same mistake this module has been finding in everything else
    // all evening, made while fixing them.)
    if (dtc_ && (frame_count_ % 100u) == 0u) dtc_->age(platform_get_tick_ms());

    // OBD Mode 04 (clear DTC) requested via CAN, handled before the modules run. The strobe
    // tells EngineProtection (MODULE phase) to forget its edges so still-true faults re-raise.
    if (can_broker_.fault_clear_pending()) {
        if (dtc_) dtc_->clear_all();
        signal_bus_.set_bool(wk::obd_clear_cmd, true, platform_get_tick_ms());
        can_broker_.consume_fault_clear();
    }

    // Config-time pin conflict -> P1650 (display/indicator only, severity 1). Three OR'd sources:
    // the firing layer (epos), the INPUT side (Sensors publishes wk::pin_conflict_fault each INPUT
    // phase), and the OUTPUT side (OutputManager's conflict probe — its OUTPUT-phase write can't use
    // the bus bool without being stomped by Sensors, so it feeds a direct flag). Healed only when
    // ALL three clear.
    constexpr uint16_t PCODE_PIN_CONFLICT = 0x1650;
    const bool pin_conflict = epos_hal_.pin_conflict()
                           || signal_bus_.get_bool(wk::pin_conflict_fault)
                           || (output_conflict_ && *output_conflict_);
    if (dtc_) {
        if (pin_conflict) dtc_->raise(PCODE_PIN_CONFLICT, DtcSource::PIN_ARBITER, 1, platform_get_tick_ms(), DTC_TTL_DEFAULT);
        else              dtc_->heal(PCODE_PIN_CONFLICT);
    }

    // --- MODULE phase: bus->bus transforms ---
    run_phase(Cadence::KHZ_1, Phase::MODULE, pos, frame);

    // --- OUTPUT phase: bus consumers (outputs, diagnostics pack) ---
    run_phase(Cadence::KHZ_1, Phase::OUTPUT, pos, frame);

    // Async transient injection (TransientThrottle, 1 kHz MODULE): a prompt extra-injection burst on a
    // tip-in, which the scheduler spreads between the sequential events. Gated by key-on like the prime
    // (no actuation on USB/bench). frame is local + fresh each call, so no clear needed.
    extern bool g_system_active;
    if (g_system_active && frame.async_inj_pulses > 0)
        epos_hal_.request_async_injection(frame.async_inj_pw_us, frame.async_inj_pulses);

    capture_freeze_frames(pos, signal_bus_);        // snapshot conditions into codes that just went active
    // No shadow commit here — both ignition AND injection are computed + committed on the per-cycle
    // task now (E-2c). The 1 kHz frame is sensors / lambda / protection / telemetry only.
    epos_hal_.service();
    signal_bus_.expire_stale(platform_get_tick_ms());
    update_telemetry(pos, signal_bus_, frame);
}

// Pure read of the decoder state — NO member mutation, so it is safe to call from either the
// 1 kHz frame task or the per-cycle task. cycle_count carries the current count (last bumped by
// the 1 kHz read_position); callers that need cycle detection use read_position(), not this.
EnginePosition EngineTask::read_position_raw() const {
    EnginePosition pos{};
    const DecoderTelemetry dec = epos_hal_.get_decoder_telemetry();
    pos.rpm             = dec.current_rpm_x10 / 10.0f;
    pos.crank_angle_deg = dec.current_angle / 10.0f;
    pos.sync_level      = epos_hal_.get_sync_level();
    pos.is_synchronized = (pos.sync_level >= SyncLevel::CRANK);
    if (dec.teeth_last_cycle > 0) {
        uint32_t pct = (static_cast<uint32_t>(dec.errors_last_cycle) * 100u) / dec.teeth_last_cycle;
        pos.trigger_error_pct = (pct > 100u) ? 100u : static_cast<uint8_t>(pct);
    }
    pos.errors_last_cycle = dec.errors_last_cycle;
    // The free-running half. Unlike the three fields above, these keep meaning when the teeth stop —
    // which is the only time the trigger fault that matters most can be reported.
    pos.noise_edges_total  = dec.noise_total;
    pos.missed_teeth_total = dec.missed_total;
    pos.phase_lost_total   = dec.phase_lost_total;
    pos.trigger_absent     = dec.trigger_absent;
    pos.last_error_tooth = dec.last_error_tooth;
    pos.last_error_kind  = dec.last_error_kind;
    pos.cycle_count      = cycle_count_;
    return pos;
}

EnginePosition EngineTask::read_position() {
    EnginePosition pos = read_position_raw();
    // Per-cycle counter: the crank angle runs 0..720 under PHASE (wraps once per engine cycle) or
    // 0..360 under CRANK (wraps once per rev). Either way a large BACKWARD jump is a wrap; the
    // angle is PLL-monotonic when synced, so a drop > 180° unambiguously marks a boundary (and a
    // 360° CRANK wrap clears 180 where the old >360 test never fired). Under CRANK the cadence is
    // therefore per-rev — the finest cycle info available without cam phase. (Unsynced -> leave the
    // count alone so the compute self-decimates to "always run".) Owned by the 1 kHz task only.
    if (pos.is_synchronized && (last_cycle_angle_ - pos.crank_angle_deg) > 180.0f) cycle_count_++;
    last_cycle_angle_ = pos.crank_angle_deg;
    pos.cycle_count   = cycle_count_;
    return pos;
}

void EngineTask::commit_ignition(const EngineFrame& frame) {
    for (uint8_t c = 0; c < num_cylinders_; c++) {
        epos_hal_.set_spark_btdc(c, frame.cyl[c].spark_btdc_x10);
        epos_hal_.set_dwell_us(c,   frame.cyl[c].dwell_us);
        // The trailing plug (rotary). Pushed unconditionally — the scheduler only arms it when the
        // engine cycle is ROTARY, and leaving it stale would fire the trailing coil at
        // whatever angle the last rotary tune left behind. Same dwell: same coil, same charge.
        epos_hal_.set_trailing_spark_btdc(c, frame.cyl[c].trail_btdc_x10);
        epos_hal_.set_trailing_dwell_us(c,   frame.cyl[c].dwell_us);
    }
}

void EngineTask::commit_injection(const EngineFrame& frame) {
    for (uint8_t c = 0; c < num_cylinders_; c++) {
        epos_hal_.set_inj_open_angle(c, frame.cyl[c].inj_btdc_x10);
        epos_hal_.set_inj_pw_us(c,      frame.cyl[c].inj_pw_us);
        for (uint8_t s = 0; s < MAX_STAGED_STAGES; ++s) {
            epos_hal_.set_staged_pw_us(c, s,      frame.cyl[c].staged_pw_us[s]);
            epos_hal_.set_staged_open_angle(c, s, frame.cyl[c].staged_btdc_x10[s]);
            epos_hal_.set_staged_enabled(c, s,    frame.cyl[c].staged_enabled[s]);
        }
    }
}

void EngineTask::update_telemetry(const EnginePosition& pos, SignalBus& bus, const EngineFrame& /*frame*/) {
    // Publish the decoder + system channels to the bus (single source of truth; comms packs
    // the wire frame FROM it). Every other channel is published by its owning participant.
    //
    // EVERY WRITE CARRIES THE TICK. These went out untimed, which does not mean "no opinion about the
    // time" — SignalBus reads the missing timestamp as 0 and ages the value already in the slot
    // against it, so a live override on any of these channels was judged ~50 days old and discarded.
    // The tick is right here; there was never a reason not to pass it.
    const uint32_t now_up = platform_get_tick_ms();
    bus.set(wk::rpm,             pos.rpm,                                   true, now_up);
    bus.set_bool(wk::synchronized, pos.is_synchronized,                           now_up);
    bus.set(wk::sync_level,      static_cast<float>(pos.sync_level),        true, now_up);
    bus.set(wk::crank_angle,     pos.crank_angle_deg,                       true, now_up);
    bus.set(wk::trigger_error_pct, static_cast<float>(pos.trigger_error_pct), true, now_up);
    // HOW LONG WE HAVE BEEN AWAKE, and WHETHER THE WHEEL IS TURNING — the two facts an output
    // condition needs and nothing published. Uptime every frame; the tooth count only WHEN IT
    // CHANGES, which is what makes age(trigger_teeth) mean "milliseconds since the last tooth".
    // Written from here rather than the capture ISR: the count is free-running in the decoder and
    // reading it once per frame costs nothing on the edge path.
    bus.set(wk::uptime_s, static_cast<float>(now_up) / 1000.0f, true, now_up);
    // HOW MUCH ROOM IS LEFT, in the two senses that matter and are not the same number: the chip's
    // (idle's share of the last window, sampled elsewhere at 1 Hz) and the ENGINE FRAME's (how much of
    // the millisecond the frame body used — its smoothed value, and the worst one since boot, because
    // an average of 30 % with a 120 % spike is a frame that was late and an average that hides it).
    bus.set(wk::cpu_load_pct,        static_cast<float>(cpustats::load_pct()),   true, now_up);
    bus.set(wk::frame_load_pct,      static_cast<float>(frame_load_pct()),       true, now_up);
    bus.set(wk::frame_load_max_pct,  static_cast<float>(frame_load_max_pct()),   true, now_up);
    const uint32_t edges = epos_hal_.get_decoder_telemetry().edges_total;
    if (edges != last_edges_) {
        last_edges_ = edges;
        bus.set_u32(wk::trigger_teeth, edges, true, now_up);
    }
    // Trigger health that keeps meaning when the teeth stop. The error RATE above cannot: it is
    // derived from a per-cycle snapshot counted in teeth, so it freezes with them. An operator
    // staring at a rig needs to see the difference between a noisy trigger and an absent one.
    {
        const DecoderTelemetry d = epos_hal_.get_decoder_telemetry();
        bus.set(wk::trigger_absent, d.trigger_absent ? 1.0f : 0.0f, true, now_up);
        bus.set_u32(wk::trigger_noise_edges,  d.noise_total,  true, now_up);
        bus.set_u32(wk::trigger_missed_teeth, d.missed_total, true, now_up);
        bus.set_u32(wk::trigger_phase_lost,   d.phase_lost_total, true, now_up);
        bus.set(wk::trigger_fault_stream, static_cast<float>(d.fault_stream), true, now_up);
        bus.set(wk::trigger_sync_ceiling,
                static_cast<float>(static_cast<uint8_t>(epos_hal_.config_sync_ceiling())), true, now_up);
    }
    bus.set(wk::trigger_last_error_tooth, static_cast<float>(pos.last_error_tooth), true, now_up);
    bus.set(wk::trigger_last_error_kind,  static_cast<float>(pos.last_error_kind), true, now_up);
    // Cam angles carry the measurement's own validity — invalid until a cam stream is locked, and again
    // once it is lost — so VvtControl holds on base duty rather than closing on a stale or absent angle.
    { const VvtResult v = epos_hal_.get_vvt_result(0); bus.set(wk::vvt_angle_1, v.displacement, v.valid, now_up); }
    { const VvtResult v = epos_hal_.get_vvt_result(1); bus.set(wk::vvt_angle_2, v.displacement, v.valid, now_up); }
    { const VvtResult v = epos_hal_.get_vvt_result(2); bus.set(wk::vvt_angle_3, v.displacement, v.valid, now_up); }
    // (Per-cylinder firing TDC is NOT streamed as telemetry — it is ordinary config: the resolver
    //  writes the even-fire angles back into engine.cyl[].tdc_angle on a stopped-engine reconfigure and
    //  the studio reads them like any config field. See EnginePositionHal::reconfigure.)
    { const VvtResult v = epos_hal_.get_vvt_result(3); bus.set(wk::vvt_angle_4, v.displacement, v.valid, now_up); }

    // Firing-order validity. EnginePositionHal refuses to fire while the order is not a clean permutation;
    // surface that so it isn't a silent dead cylinder. Publish the fault bit every frame (ttl 0) for the
    // studio's per-widget error highlight, and edge-raise a CONFIG DTC (validity source -> logs at the
    // bench/key-off too) so it also lands on a scan tool. Level 3; firing itself is held off by the firing gate.
    // A TUNE THAT CAN NEVER KNOW WHERE THE ENGINE IS. Decided from the configuration at every
    // reconfigure: an even wheel with nothing to anchor it, or one whose only absolute source is a
    // phaser, can never fix position however long it spins. EnginePositionHal already refuses to arm
    // firing on it — this is what says WHY, at the bench, instead of leaving a tuner cranking an
    // engine that will never catch. Same shape as the firing-order fault below and for the same
    // reason: a configuration that must not fire, known before it is asked to.
    constexpr uint16_t PCODE_TRIGGER_CONFIG = 0x1652;
    if (dtc_) {
        const bool cfg_bad = (epos_hal_.config_sync_ceiling() == SyncLevel::NONE);
        if (cfg_bad) dtc_->raise(PCODE_TRIGGER_CONFIG, DtcSource::CONFIG, DTC_SEV_LEVEL3,
                                 platform_get_tick_ms(), DTC_TTL_DEFAULT);
        else if (trigger_config_dtc_raised_) { dtc_->heal(PCODE_TRIGGER_CONFIG); }
        trigger_config_dtc_raised_ = cfg_bad;
    }

    constexpr uint16_t PCODE_FIRING_ORDER = 0x1651;   // P1651 — invalid firing order (duplicate/missing cylinder)
    const uint32_t now_fo = platform_get_tick_ms();
    // INDICATOR — the LIVE config, so the studio highlights a bad order the instant it is typed, even
    // while the engine runs (a structural edit is deferred to the next stopped reconfigure, so the
    // applied order below hasn't changed yet). Cheap permutation check; g_config carries the live edit.
    bus.set_bool(SIG_FIRING_ORDER_FAULT, !firing_order_valid(g_config.engine), now_fo, 0);
    // DTC + the firing gate act on the APPLIED order (recomputed at reconfigure). Keep the DTC here, NOT
    // on the live config: it is level 3, and the worst active severity selects that protection level, so a
    // live raise would apply level 3's reaction to a running engine over an order that is typed but not yet
    // applied. The applied order can only turn invalid at an engine-stopped reconfigure.
    const bool fo_bad_applied = !epos_hal_.firing_order_valid();
    if (dtc_) {
        // Raised every frame while the applied order is bad, so last-seen tracks the condition rather
        // than freezing at the boot millisecond it was first noticed (raise() counts only the edge).
        if (fo_bad_applied) {
            dtc_->raise(PCODE_FIRING_ORDER, DtcSource::CONFIG, DTC_SEV_LEVEL3, now_fo, DTC_TTL_DEFAULT);
            firing_order_dtc_raised_ = true;
        } else if (firing_order_dtc_raised_) {
            dtc_->heal(PCODE_FIRING_ORDER);
            firing_order_dtc_raised_ = false;
        }
    }

    // THE OUTPUT ROWS, as APPLIED: a cylinder in the firing order with no coil (P1653) or no stage-1
    // injector (P1654). The studio lays the rows out and the firmware fills nothing in, so a row cleared by
    // hand is a cylinder that gets no spark or no fuel — said here rather than found on a dyno. Level 3, on
    // the APPLIED map for the same reason as the firing order: it only changes at a stopped reconfigure, so a
    // live edit never applies level 3's reaction to a running engine. Not asked of a broken order, which is its own fault.
    constexpr uint16_t PCODE_NO_COIL = 0x1653, PCODE_NO_INJECTOR = 0x1654;
    if (dtc_) {
        const bool order_ok = !fo_bad_applied;
        const bool no_coil  = order_ok && epos_hal_.cylinders_without_coil()     != 0;
        const bool no_inj   = order_ok && epos_hal_.cylinders_without_injector() != 0;
        if (no_coil) dtc_->raise(PCODE_NO_COIL, DtcSource::CONFIG, DTC_SEV_LEVEL3, now_fo, DTC_TTL_DEFAULT);
        else if (no_coil_dtc_raised_) dtc_->heal(PCODE_NO_COIL);
        no_coil_dtc_raised_ = no_coil;
        if (no_inj) dtc_->raise(PCODE_NO_INJECTOR, DtcSource::CONFIG, DTC_SEV_LEVEL3, now_fo, DTC_TTL_DEFAULT);
        else if (no_inj_dtc_raised_) dtc_->heal(PCODE_NO_INJECTOR);
        no_inj_dtc_raised_ = no_inj;
    }
    bus.set(wk::secl, static_cast<float>((xTaskGetTickCount() / 1000) & 0xFF),
            true, platform_get_tick_ms());   // heartbeat
    // A COUNTER, so it lives in a u32 cell: as a float it went coarse past 2^24, which a 1 kHz counter
    // reaches in about 4.7 hours — after which it would have counted in 2s, then 4s.
    bus.set_u32(wk::frame_count, frame_wakes_, true, platform_get_tick_ms());     // 1 kHz loop-rate diagnostic

    // g_engine_running is owned by the run-state machine in run_frame() now (it = "engine turning",
    // i.e. CRANKING or RUNNING) — no longer a bare rpm>50 test here.
    g_led_status.sync_level = pos.sync_level;
    g_led_status.rpm        = pos.rpm;
    g_led_status.turning    = bus.valid(wk::trigger_teeth) &&
                              bus.age_ms(wk::trigger_teeth, platform_get_tick_ms()) < 500u;
    constexpr uint8_t LM = LedStatus::DTC_LED_MAX;
    uint16_t wc[LM] = {}, fc[LM] = {};
    if (dtc_) {
        dtc_->list_active_band(1, 2, wc, LM);
        dtc_->list_active_band(3, 3, fc, LM);
    }
    for (uint8_t i = 0; i < LM; i++) { g_led_status.dtc_warn_codes[i] = wc[i]; g_led_status.dtc_fault_codes[i] = fc[i]; }
}

void EngineTask::capture_freeze_frames(const EnginePosition& pos, SignalBus& bus) {
    if (!dtc_) return;
    uint16_t codes[DtcManager::PENDING_MAX];
    const uint8_t n = dtc_->drain_new_activations(codes, DtcManager::PENDING_MAX);
    if (n == 0) return;
    const float ff[DTC_FF_CHANNELS] = {
        pos.rpm,
        bus.get(wk::map,   0.0f),
        bus.get(wk::clt,     0.0f),
        bus.get(wk::battery, 0.0f),
    };
    for (uint8_t i = 0; i < n; i++)
        dtc_->set_freeze_frame(codes[i], ff, DTC_FF_CHANNELS);
}
