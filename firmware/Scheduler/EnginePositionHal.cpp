#include "EnginePositionHal.h"
#include <algorithm>
#include "TriggerConfigCheck.h"
#include "SegmentTimer.h"
#include "GenericTriggerBuilder.h"
#include "EngineSyncSampler.h"
#include "Log.h"
#include "../Platform/platform_hal.h"      // platform_irq_save / restore
#include "../../generated/ecu_config.h"   // g_config — the resolver writes even-fire TDCs back into config
#include "../Comms/CommandState.h"        // set_command_state — OK edge tells the studio to re-read engine.cyl
#include <cstring>

volatile uint32_t g_firing_bind_generation = 0;

// VirtualTrigger reconfigure hook — defined in main.cpp during bring-up; weak so host
// tests (and any build without the angle clock) link with it resolving to null.
extern void vtrig_reconfigure(AngleDeg10 cycle_angle, uint16_t teeth) noexcept
    __attribute__((weak));

// Knock worker wake — defined in main.cpp (pends the relay vector). Weak so host tests link null.
extern void knock_worker_pend() noexcept __attribute__((weak));

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
EnginePositionHal::EnginePositionHal(const EcuHardwareAssignment& assignment,
                                      const TriggerConfig&         trig,
                                      const EngineConfig&          eng,
                                      const OutputsConfig&         outs) noexcept
    : assignment_(assignment)
    , trig_src_(trig)            // live g_config refs, handed to the modules that shadow them
    , eng_src_(eng)
    , outs_src_(outs)
    , scheduler_()
    , shadow_{}
    , vvt_results_{}
    , last_level_(SyncLevel::NONE)
    , promotion_pending_(false)
    , grid_vt_step_(0)
    , grid_tpv_(0)
    , running_(false)
{
    // Bind channels from the (possibly still-empty) assignment. With dynamic
    // configuration the real binding happens later, when start()/reconfigure()
    // re-runs this against a resolved assignment_.
    wire_from_assignment();

    // ---- Initialise shadow and active defaults ------------------------------
    for (uint8_t i = 0; i < MAX_CYLINDERS; ++i) {
        shadow_[i].spark_btdc          = AngleDeg10(-30 * 10);  // 30° BTDC
        shadow_[i].dwell_us            = 3500u;                   // 3.5 ms
        shadow_[i].trailing_spark_btdc = AngleDeg10(-15 * 10);
        shadow_[i].trailing_dwell_us   = 2500u;
        shadow_[i].inj_open_btdc       = AngleDeg10(-355 * 10); // 355° BTDC
        shadow_[i].inj_pw_us           = 5000u;                  // 5 ms
        for (uint8_t s = 0; s < MAX_STAGED_STAGES; ++s) {
            shadow_[i].inj_secondary_pw_us[s]     = 3000u;
            shadow_[i].staged_enabled[s]          = false;
            shadow_[i].inj_secondary_open_btdc[s] = AngleDeg10(-355 * 10); // mirrors primary
        }
    }

    for (int i = 0; i < MAX_CAM_CHANNELS; ++i) {
        VvtResult tmp{ static_cast<uint8_t>(i), 0, false };
        std::memcpy(const_cast<VvtResult*>(&vvt_results_[i]), &tmp, sizeof(VvtResult));
    }
}

// ---------------------------------------------------------------------------
// Shadow register setters — write only; all are FreeRTOS-task-safe.
// Changes take effect on the next gap-tooth cycle boundary via service().
// ---------------------------------------------------------------------------
void EnginePositionHal::set_spark_btdc(uint8_t cyl, AngleDeg10 spark_btdc) noexcept
{
    if (cyl >= scheduler_.cylinder_count()) return;
    shadow_[cyl].spark_btdc = spark_btdc;
}

void EnginePositionHal::set_dwell_us(uint8_t cyl, uint32_t dwell_us) noexcept
{
    if (cyl >= scheduler_.cylinder_count()) return;
    shadow_[cyl].dwell_us = dwell_us;
}

void EnginePositionHal::set_trailing_spark_btdc(uint8_t face, AngleDeg10 spark_btdc) noexcept
{
    if (face >= scheduler_.cylinder_count()) return;
    if (!scheduler_.is_rotary()) return;
    shadow_[face].trailing_spark_btdc = spark_btdc;
}

void EnginePositionHal::set_trailing_dwell_us(uint8_t face, uint32_t dwell_us) noexcept
{
    if (face >= scheduler_.cylinder_count()) return;
    if (!scheduler_.is_rotary()) return;
    shadow_[face].trailing_dwell_us = dwell_us;
}

void EnginePositionHal::set_inj_open_angle(uint8_t cyl, AngleDeg10 open_btdc) noexcept
{
    if (cyl >= scheduler_.cylinder_count()) return;
    shadow_[cyl].inj_open_btdc = open_btdc;
}

void EnginePositionHal::set_inj_pw_us(uint8_t cyl, uint32_t pw_us) noexcept
{
    if (cyl >= scheduler_.cylinder_count()) return;
    shadow_[cyl].inj_pw_us = pw_us;
}

void EnginePositionHal::prime_injectors(uint32_t pw_us) noexcept
{
    // Hardware-agnostic prime: hand the request to the scheduler, which fires every active injector
    // simultaneously on its next grid tooth (ISR). Firmware never touches a pin.
    scheduler_.request_prime(pw_us);
}

void EnginePositionHal::request_async_injection(uint32_t pw_us, uint8_t num_pulses) noexcept
{
    // Hardware-agnostic async transient delivery: the scheduler spreads num_pulses extra all-injector
    // squirts across the cycle (ISR-fired between the sequential events). No pin handling here.
    scheduler_.request_async_injection(pw_us, num_pulses);
}

void EnginePositionHal::set_staged_pw_us(uint8_t cyl, uint8_t stage, uint32_t pw_us) noexcept
{
    if (cyl >= scheduler_.cylinder_count() || stage >= MAX_STAGED_STAGES) return;
    shadow_[cyl].inj_secondary_pw_us[stage] = pw_us;
}

void EnginePositionHal::set_staged_enabled(uint8_t cyl, uint8_t stage, bool enabled) noexcept
{
    if (cyl >= scheduler_.cylinder_count() || stage >= MAX_STAGED_STAGES) return;
    shadow_[cyl].staged_enabled[stage] = enabled;
}

void EnginePositionHal::set_staged_open_angle(uint8_t cyl, uint8_t stage, AngleDeg10 btdc_x10) noexcept
{
    if (cyl >= scheduler_.cylinder_count() || stage >= MAX_STAGED_STAGES) return;
    shadow_[cyl].inj_secondary_open_btdc[stage] = btdc_x10;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void EnginePositionHal::wire_from_assignment(bool boot) noexcept
{
    // No pool-wide "force every pin LOW" here: under the PinArbiter each pin's state
    // is the responsibility of its owner. The scheduler de-energizes only ITS claimed
    // pins (emergency_off over ign_channels_); unclaimed pins are Hi-Z; another module
    // (e.g. boost) owns its pin and must not be stepped on.

    // ---- Wire scheduler channels -------------------------------------------
    // Set the cylinder config FIRST: assign_*_channel() forces each pin to its
    // polarity-correct idle level (ign/inj_idle_level()), which reads cyl_cfg_. If
    // the config were set after, an active-low output would be driven LOW (energized)
    // during binding before the polarity is known.
    scheduler_.set_config(eng_src_, output_map_from(outs_src_.output, OUTPUTS_OUTPUT_COUNT), boot);
    if (assignment_.timebase) {
        scheduler_.assign_timebase(*assignment_.timebase);
    }
    // Claim ONLY the cylinders' pins from the arbiter (spares stay Hi-Z). Falls back
    // to nothing if no arbiter is wired (firing disabled until one is).
    if (assignment_.pin_arbiter) {
        scheduler_.assign_pin_arbiter(*assignment_.pin_arbiter);
        scheduler_.claim_outputs();
    }
    // The firing pins just changed hands. A generic output that lost its row to a coil released its pin
    // already; one that GAINED a row the scheduler has only now let go of has to be rebuilt to claim it.
    ++g_firing_bind_generation;
    if (assignment_.angle_alarm) scheduler_.assign_angle_alarm(*assignment_.angle_alarm);
    if (assignment_.time_alarm)  scheduler_.assign_time_alarm(*assignment_.time_alarm);

    // ---- Wire the generic trigger decoder ----------------------------------
    // Build GenericTrigger from the live stream config and bind each stream's capture channel
    // BY RATE to the resolved crank/cam channels (pin + edge come from the crank/cam
    // resolution in AssignmentResolver; per-stream capture_index resolution lands with the
    // hardware-schema rework). One trampoline per stream → on_gen_edge → fuse → PLL feed.
    // Teeth per REVOLUTION of the crank wheel, straight from the tune: base teeth minus the ones the
    // gap removes. Configuration, so it survives anything the decoder does at runtime.
    teeth_per_rev_ = 0;
    for (uint8_t i = 0; i < MAX_STREAMS; ++i) {
        const StreamsConfig& c = trig_src_.streams[i];
        if (!c.enabled || !role_is_crank_slot(i)) continue;
        // cell_len is the number of GAP POSITIONS on the wheel; zero means an EVEN wheel with no
        // gaps at all. Treating zero as one (a stray `cell_len ? cell_len : 1`) invented a missing
        // tooth on every even wheel and the capture came back one tooth short per revolution —
        // measured on a Nissan CAS as 358 crank edges where the fine track delivers 360.
        const uint16_t missing = c.cell_len
            ? static_cast<uint16_t>((c.gap_ratio > 1 ? (c.gap_ratio - 1) : 0) * c.cell_len)
            : 0u;
        if (c.slots > missing) teeth_per_rev_ = static_cast<uint16_t>(c.slots - missing);
        // slots/missing describe ONE PERIOD of the pattern, which is a revolution only when the
        // pattern happens to recur once per revolution. Carry the repeat count so the cycle length
        // can be worked out from the two: a Renix wheel is 4 teeth per 180 deg, and calling that
        // "teeth per revolution" and doubling it gives 8 where the cycle holds 16.
        crank_repeats_ = c.repeats ? c.repeats
                       : static_cast<uint8_t>(scheduler_.cycle_angle() / ANGLE_360);
        if (!crank_repeats_) crank_repeats_ = 1;
        break;
    }
    build_generic(gtrig_, trig_src_.streams, MAX_STREAMS, scheduler_.cycle_angle());
    // Bind each stream to a capture channel BY RATE, in lockstep with AssignmentResolver:
    // crank-rate streams → crank_primary then crank_secondary; cam-rate → cam[0..3] (and
    // remember the cam slot for VVT). Streams flagged unused (capture_index 255) are
    // skipped WITHOUT advancing the counters, so the rate ordering matches the resolver's.
    for (uint8_t s = 0; s < MAX_STREAMS; ++s) {
        ICaptureChannel* ch = nullptr;
        gen_cam_index_[s] = 0xFF;
        if (!trig_src_.streams[s].enabled || trig_src_.streams[s].capture_index == 255) {
            gen_ch_[s] = nullptr; gen_ctx_[s] = { this, s };
            continue;
        }
        // Bind BY SLOT to the same crank/cam slots the AssignmentResolver filled, so the channel
        // (pin + edge) matches. The slot IS the role, so nothing maps between the two and a stream
        // cannot be bound as something other than what it is.
        const int crank_slot = crank_slot_of(s);
        const int cam_slot   = cam_slot_of(s);
        if (crank_slot == 0)      ch = assignment_.crank_primary;
        else if (crank_slot == 1) ch = assignment_.crank_secondary;
        else if (cam_slot >= 0 && cam_slot < MAX_CAM_CHANNELS) {
            gen_cam_index_[s] = static_cast<uint8_t>(cam_slot);
            ch = assignment_.cam[cam_slot];
        }
        gen_ch_[s] = ch; gen_ctx_[s] = { this, s };
        if (ch) { ch->register_callback(gen_edge_trampoline, &gen_ctx_[s]);
                  ch->set_capture_enabled(true); }
    }
    scheduler_.register_compute_hook(compute_trampoline, this);   // cyl_cfg_ already set above
}

void EnginePositionHal::start() noexcept
{
    if (running_) return;
    // Each module refreshes its OWN config shadow at this safe boundary:
    // wire_from_assignment() calls scheduler_.set_config() (copies the cylinder
    // config), and decoder_.start() (below) re-reads the wheel config. Engine-
    // stopped, task context — a 'w' to a reconfig field only lands here.
    // The FIRST start after the flashed tune is loaded does a FULL shadow pull (incl.
    // reboot-flagged fields); later starts (reconfigure) refresh engine_stop fields
    // only, so reboot fields apply on the next boot, not on a runtime reconfigure.
    const bool boot = !primed_;
    primed_ = true;
    // Refresh the even-fire cylinder angles INTO config before the scheduler fetches them, so the shadow
    // pull below (and resolve_binding) reads the fresh values. On a stopped reconfigure (not boot) bump the
    // generation so the studio re-reads engine.cyl — a direct g_config write, not the comms 'w' path, so it
    // does not re-arm the reconfigure gate (that watches 'w' shadow bits, not the generation).
    update_cylinder_angles(g_config.engine);
    // Validity gate: an invalid firing order (a duplicate or a missing cylinder) must NEVER schedule — it
    // would double-fire one cylinder and silently drop another. update_firing_enable() ANDs this in, so
    // firing can never arm while it is false. Recomputed here on every reconfigure and at boot. Qualified
    // ::firing_order_valid to reach the free function past the same-named getter.
    firing_order_valid_ = ::firing_order_valid(g_config.engine);
    // CAN THIS TUNE EVER KNOW WHERE THE ENGINE IS? A wheel with no unique feature and nothing else to
    // anchor it never can, and that is knowable now rather than by an engine that will not run right.
    // Folded into the firing enable below beside the firing-order check, which is the same kind of
    // fault: a configuration that must not be allowed to fire, decided at configure time.
    {
        // The engine, from the tune itself: the scheduler's copy is refreshed only by the wire-up
        // below, and a reconfigure that just changed Ignition Mode must be judged on the new one.
        TriggerEngineShape shape;
        shape.known     = true;
        shape.ign_mode  = g_config.engine.ign_mode;
        shape.ncyl      = g_config.engine.cylinder_count;
        shape.even_fire = (g_config.engine.odd_fire == 0);
        shape.rotary    = (g_config.engine.cycle_type == static_cast<uint8_t>(EngineCycleType::ROTARY));
        for (uint8_t k = 0; k < g_config.engine.num_inj_stages && k < MAX_INJ_STAGES; ++k) {
            const auto m = static_cast<InjectionMode>(g_config.engine.inj_stage[k].mode);
            if (m == InjectionMode::SEQUENTIAL || m == InjectionMode::SEMI_SEQUENTIAL ||
                m == InjectionMode::BANK)
                shape.cyl_timed_injection = true;
        }
        const TriggerConfigCheck chk =
            check_trigger_config(trig_src_.streams, MAX_STREAMS, scheduler_.cycle_angle(), shape);
        // Before wire_from_assignment() rebuilds the decoder, so the streams it adds take it up.
        gtrig_.set_sync_always(chk.sync_always);
        config_ceiling_ = static_cast<SyncLevel>(static_cast<uint8_t>(chk.ceiling));
        config_fault_   = static_cast<uint8_t>(chk.fault);
    }
#if defined(JAYECU_FIRMWARE)   // g_config_generation lives in CommsManager (target); host tests have no studio
    if (!boot && g_config.engine.odd_fire == 0) {
        extern volatile uint32_t g_config_generation;
        ++g_config_generation;
        // If an engine structural write armed a reconfigure (ENGINE_RECONFIG RUNNING, set in the config-write
        // handler), flip it to OK now that cyl[].tdc_angle is recomputed — the studio re-reads engine.cyl on
        // that edge (Cache::rereadForOp). Same RUNNING->OK handshake the ETB routines use; the pending-arm and
        // the completion are naturally separated in time, so consecutive reconfigures each raise an edge.
        if (g_command_state == cmdstate::pack(cmdstate::ENGINE_RECONFIG, cmdstate::RUNNING))
            set_command_state(cmdstate::ENGINE_RECONFIG, cmdstate::OK);
    }
#endif
    wire_from_assignment(boot);  // (re)bind from the current resolved assignment

    // Segment timing follows the wheel and the cylinder layout, both of which can change here — so it
    // is (re)declared on every start rather than once at boot. AVAILABILITY is decided inside, from
    // teeth per segment: a wheel too coarse to time a segment reports nothing at all rather than
    // handing a classifier numbers dominated by where its teeth happen to fall.
    if (seg_timer_) {
        AngleDeg10 tdcs[SegmentTimer::MAX_CYL] = {};
        const uint8_t n = scheduler_.cylinder_count() < SegmentTimer::MAX_CYL
                        ? scheduler_.cylinder_count() : SegmentTimer::MAX_CYL;
        for (uint8_t c = 0; c < n; ++c) tdcs[c] = scheduler_.tdc_angle(c);
        seg_timer_->configure(tdcs, n, scheduler_.cycle_angle());
    }

    // Re-init the angle clock to a clean CRANK grid on EVERY (re)start, so a LIVE wheel
    // reconfigure (engine-stopped, no MCU reset) starts boot-equivalent: no stale PLL lock,
    // velocity EMA, or grid basis carried over from the previous wheel — those corrupt RPM
    // and block CRANK→PHASE re-acquisition. vtrig_reconfigure() resets the VirtualTrigger
    // (clears locked_/ticks_per_vtooth_, disarms the DCO) and narrows the grid to 360°/36.
    grid_vt_step_ = 0;
    grid_tpv_     = 0;
    if (vtrig_reconfigure) vtrig_reconfigure(ANGLE_360, 36);
    // The deadline's bounds and its callback come from the assignment, which a live reconfigure can
    // replace — so they are refreshed HERE, on every (re)start. The old watchdog's comment claimed
    // it was "re-applied on (re)configure" while being called exactly once, from the composer.
    configure_stall_watchdog();
    last_fine_tick_valid_ = false;
    disarm_tooth_deadline();

    // Build the persistent skeleton ONCE from the tune. Firing stays disabled
    // (build_skeleton clears it) until service() sees CRANK sync. Default grid
    // pitch is 10° (100 dd); on_grid_tooth confirms the live pitch.
    scheduler_.build_skeleton(grid_vt_step_ > 0 ? grid_vt_step_ : 100);
    last_level_        = SyncLevel::NONE;
    promotion_pending_ = false;

    running_ = true;
    // gtrig_ was built and its stream channels armed in wire_from_assignment().
    (void)boot;
}

void EnginePositionHal::reconfigure() noexcept
{
    // Live re-apply with no power cycle: stop() forces outputs safe and disarms
    // captures; start() re-wires from the updated assignment_ and re-reads the
    // wheel geometry, then re-arms. start() also refreshes the even-fire cylinder
    // angles into config before the scheduler reads them (see update_cylinder_angles).
    stop();
    start();
}

void EnginePositionHal::stop() noexcept
{
    if (!running_) return;
    running_ = false;
    for (uint8_t s = 0; s < MAX_STREAMS; ++s)         // disarm the generic stream captures
        if (gen_ch_[s]) gen_ch_[s]->set_capture_enabled(false);
    disarm_tooth_deadline();     // no captures means no teeth: a deadline here would be self-fulfilling
    // quiesce_volatiles() → emergency_off() de-energizes only the scheduler's OWN
    // claimed pins (its ign_channels_/inj_channels_) at the polarity-correct idle —
    // it does not touch pins owned by other modules (boost, etc.). No pool-wide
    // force here (that would step on another owner).
    scheduler_.quiesce_volatiles();   // firing off + unlink volatiles + scheduler outputs de-energized
    last_level_        = SyncLevel::NONE;
    promotion_pending_ = false;
}

// ---------------------------------------------------------------------------
// service() — engine task frame (1 kHz). Steady-state cycles do NO work here:
// the self-correcting chain runs entirely in the grid ISR. This only (1) rebuilds
// the persistent skeleton when a config change asked for it, and (2) acts on
// sync-LEVEL transitions (NONE/CRANK/PHASE), which the decoder reports by level,
// not by the SyncState callback.
// ---------------------------------------------------------------------------
void EnginePositionHal::service() noexcept
{
    // Pick up a timing-light adjustment without stopping the engine (see the member's comment).
    trig_offset_btdc_ = static_cast<AngleDeg10>(trig_src_.trigger_offset_btdc);

    // NOTHING IS DRIVEN WITH THE KEY OFF. Generic outputs already let go of their pins (OutputManager);
    // the coils and injectors did not — the scheduler claimed them at wiring and held them push-pull at
    // their idle level whatever the key said, so with the ECU on USB or the key off an injector or coil
    // line was actively driven low. Key off: firing stops and every firing pin goes back to Hi-Z. Key on:
    // they are claimed again at their idle level, and generic outputs rebuild around them.
    {
        extern bool g_system_active;
        if (g_system_active != key_was_on_) {
            key_was_on_ = g_system_active;
            if (!key_was_on_) {
                scheduler_.set_firing_enabled(false);
                scheduler_.release_outputs();
            } else if (assignment_.pin_arbiter) {
                scheduler_.claim_outputs();
            }
            ++g_firing_bind_generation;
        }
    }

    // Phase-sync rpm band (min/max_full_sync_rpm_x10). The decoder trusts the cam stream to acquire
    // full (PHASE) sync only while rpm is inside the band; outside it the cam is ignored for acquisition
    // and correction, and an already-locked phase is retained on crank counting. min==max==0 disables it
    // (cam trusted whenever it locks — the historical behaviour). Read live so a band edit applies at once.
    {
        const uint32_t min_x10 = trig_src_.min_full_sync_rpm_x10;
        const uint32_t max_x10 = trig_src_.max_full_sync_rpm_x10;
        bool cam_in_band = true;
        if (min_x10 != 0 || max_x10 != 0) {
            const uint32_t rpm_x10 = get_rpm_x10();
            cam_in_band = (rpm_x10 >= min_x10) && (max_x10 == 0 || rpm_x10 <= max_x10);
        }
        gtrig_.set_phase_stream_valid(cam_in_band);
    }

    // Maximum dwell: twice the longest commanded, never under 3 ms nor over 20 ms. Twice, because the
    // dwell is placed by angle and a cranking engine's speed swings; the cutoff is for a coil that has
    // lost its spark, not for trimming a real one.
    {
        uint32_t longest = 0;
        const uint8_t n = scheduler_.cylinder_count();
        for (uint8_t c = 0; c < n && c < MAX_CYLINDERS; ++c) {
            longest = std::max<uint32_t>(longest, static_cast<uint32_t>(shadow_[c].dwell_us));
            if (scheduler_.is_rotary()) longest = std::max<uint32_t>(longest, static_cast<uint32_t>(shadow_[c].trailing_dwell_us));
        }
        scheduler_.set_max_dwell_us(std::min<uint32_t>(20000u, std::max<uint32_t>(3000u, 2u * longest)));
    }

    // The match window follows engine speed — widest while cranking, tightest at rpm. Pushed here,
    // once per frame, so the matchers stay rpm-agnostic and the policy lives in one place.
    gtrig_.set_rpm(get_rpm_x10() / 10u);


    const SyncLevel lvl = get_sync_level();
    if (lvl != last_level_) {
        handle_sync_level_change(last_level_, lvl);
        last_level_ = lvl;
    }
}

// ---------------------------------------------------------------------------
// handle_sync_level_change — drive the scheduler's phase gate / firing enable,
// coil-safe on downgrade, and widen/narrow the grid across the CRANK⇄PHASE edge.
// Model A: CRANK fires the wasted pair (phase unknown); PHASE gates each event to
// its home revolution half. Engine task context.
// ---------------------------------------------------------------------------
void EnginePositionHal::handle_sync_level_change(SyncLevel from, SyncLevel to) noexcept
{
    // ANY sync transition opens a new firing epoch. This disarms firing (armed_epoch_ no longer
    // matches) until the per-cycle task computes + commits a schedule for the new epoch — so a
    // schedule computed against the OLD position reference can never fire after a re-sync.
    sync_epoch_++;

    switch (to) {
    case SyncLevel::NONE:
        // Lost position — coil-safe and stop dispatch; re-narrow the grid so a
        // fresh acquisition starts in CRANK (360°) space.
        scheduler_.quiesce_volatiles();
        scheduler_.set_phase_known(false);
        if (vtrig_reconfigure) vtrig_reconfigure(ANGLE_360, 36);
        // THE VELOCITY DIES WITH THE POSITION, on this path too. on_trigger_lost() already clears
        // it, but a matcher-driven loss reaches NONE through here instead and used to leave the grid
        // velocity behind. get_rpm_x10() gates on the level so nothing reads it — which is exactly
        // what was true of the stale velocity that reported 1348 rpm into a dead trigger for forty
        // minutes. A value that outlives the thing it was measured from is not worth keeping.
        grid_tpv_     = 0;
        grid_vt_step_ = 0;
        // There is no position left to lose, but there IS still a question worth answering: was
        // that a tooth that did not arrive, or a trigger that is gone? Only time separates them, so
        // keep one deadline running at the floor bound purely to upgrade the diagnosis. Disarming
        // here unconditionally is what stopped SIGNAL_ABSENT ever being reached — the ISR armed the
        // follow-up and this ran a frame later and threw it away.
        if (last_fine_tick_valid_ && !trigger_absent_ && stall_floor_ticks_ && assignment_.tooth_alarm)
            assignment_.tooth_alarm->arm(last_fine_tick_ + stall_floor_ticks_);
        else
            disarm_tooth_deadline();
        promotion_pending_ = false;
        break;

    case SyncLevel::CRANK:
        // Wasted-spark / semi-sequential. On demotion from PHASE, force ignition
        // safe first (a coil mid-dwell must not strand), then revert to wasted.
        if (from == SyncLevel::PHASE) {
            scheduler_.quiesce_volatiles();
            if (vtrig_reconfigure) vtrig_reconfigure(ANGLE_360, 36);
        }
        scheduler_.set_phase_known(false);
        // ARM ACROSS THE TRANSITION. vtrig_reconfigure() above has just reset the DCO, and the old
        // watchdog lived inside it — so this is precisely the window in which the ECU had no
        // liveness detection at all. If the teeth stop here, the deadline is what notices.
        arm_tooth_deadline(last_fine_tick_valid_ ? last_fine_tick_ : timebase_now());
        promotion_pending_ = false;
        break;

    case SyncLevel::PHASE:
        // Promotion: widen the grid to the ENGINE CYCLE so it emits a full-cycle index and the
        // scheduler can tell the revolutions apart. Unlink the stale CRANK-armed volatiles so PHASE
        // re-arms at PHASE angles. The generic decoder re-anchors the cycle angle continuously from
        // the cam (no one-cycle suppression needed).
        //
        // A TWO-STROKE has nothing to widen to: its cycle IS 360 deg, every cylinder fires every
        // revolution, and crank sync is already full sync. Cam phase still tells the decoder where
        // it is (and drives VVT), but the firing grid stays 3600/36 — widening it would spread one
        // cycle's events across two revolutions of a cycle the engine does not have. (Wasted spark
        // still applies on a two-stroke: the companion sits 180 deg away, at BDC.)
        scheduler_.quiesce_volatiles();
        if (vtrig_reconfigure) {
            // One virtual tooth per 10 deg, whatever the cycle: 36 (two-stroke), 72 (four-stroke),
            // 108 (rotary). MAX_VTEETH is sized for the longest of them.
            const AngleDeg10 cyc = scheduler_.cycle_angle();
            vtrig_reconfigure(cyc, static_cast<uint16_t>(cyc / 100));
        }
        scheduler_.set_phase_known(true);
        // Same reasoning as the CRANK case: the promotion has just reset the DCO. This is the exact
        // transition that left a bench ECU reporting 1348 rpm with no trigger wire attached.
        arm_tooth_deadline(last_fine_tick_valid_ ? last_fine_tick_ : timebase_now());
        promotion_pending_ = false;
        break;
    }
    update_firing_enable();   // new epoch -> disarmed until the next commit; honours the key gate + sync
}

// The single firing-enable decision: key-on gate AND a schedule armed for the CURRENT sync epoch AND
// actually synced. Re-evaluated on every input to it (key gate, sync transition, arm). Reads are all
// single-word atomic; on any race the mismatch/NONE checks fail CLOSED, so it never fires spuriously.
void EnginePositionHal::update_firing_enable() noexcept {
    scheduler_.set_firing_enabled(firing_gate_
                                  && config_ceiling_ != SyncLevel::NONE
                                  && firing_order_valid_
                                  && armed_epoch_ == sync_epoch_
                                  && get_sync_level() != SyncLevel::NONE);
}

// The per-cycle task calls this after committing a schedule to the shadow, passing the epoch it
// snapshotted BEFORE computing. If a sync transition advanced the epoch meanwhile, the schedule is
// stale and this no-ops the arm (armed_epoch_ won't match) — fail-closed.
void EnginePositionHal::arm_firing(uint32_t epoch) noexcept {
    armed_epoch_ = epoch;
    update_firing_enable();
}

// Master firing gate (system-active / key-on). Re-evaluated every 1 kHz frame from EngineTask.
// Closed -> firing off NOW (armed events check firing_enabled_ and won't fire). Opened -> restore
// firing if we're synced AND armed (no fresh sync edge would otherwise re-enable it).
void EnginePositionHal::set_firing_gate(bool open) noexcept {
    // CLOSING IT DROPS SYNC. The edges stop being consumed the moment the key goes (see on_gen_edge), so
    // a sync reached while it was on would otherwise sit there unrefreshed — a stale angle, and an rpm
    // decaying out of a decoder nothing is feeding. Dropping puts it back to ACQUIRING, which is the
    // state a power-on reset would have left it in, and is the same path the lost-trigger watchdog takes.
    //
    // MASKED: this is task context, and the capture ISR (priority 1) runs the same decoder in on_edge.
    // An edge landing half-way through drop_sync would judge itself against half-reset matcher state.
    if (firing_gate_ && !open) {
        const uint32_t pm = platform_irq_save();
        gtrig_.drop_sync();
        on_trigger_lost();
        platform_irq_restore(pm);
    }
    firing_gate_ = open;
    update_firing_enable();
}

// ---------------------------------------------------------------------------
// Compute hook — called from the grid ISR when a STATIC schedule node comes due.
// Recompute the cylinder's live fire angles from the shadow registers + RPM and
// re-arm its VOLATILE nodes. This is the self-correcting chain: no pool rebuild.
// ---------------------------------------------------------------------------
// `index` is a CYLINDER for the ignition/knock actions and an injection EVENT for INJ_SCHEDULE —
// see EventScheduler::ComputeHook. The two spaces coincided only while every injection event
// belonged to one cylinder.
void EnginePositionHal::compute_trampoline(void* ud, uint8_t index, EventAction kind) noexcept
{
    auto* self = static_cast<EnginePositionHal*>(ud);
    switch (kind) {
    case EventAction::IGN_SCHEDULE:    self->recompute_ignition(index);  break; // arm spark
    case EventAction::IGN_DWELL_START: self->recompute_dwell(index);     break; // arm next dwell
    case EventAction::ADC_TRIGGER:     self->on_knock_window(index);     break; // knock window fired
    default:                           self->recompute_injection(index); break; // INJ_SCHEDULE
    }
}

// ADC_TRIGGER dispatch (ISR): the firing cylinder's knock window just came due. Deposit it into the
// SPSC mailbox for the knock worker (Stage C arms the ADC burst + runs the DSP). Drops on overflow —
// single-flight back-pressure, so a slow worker skips windows rather than piling up.
void EnginePositionHal::on_knock_window(uint8_t cyl) noexcept {
    knock_window_fires_++;                    // bench diagnostic (every window, independent of the mailbox)
    const uint8_t h   = knock_head_;
    const uint8_t nxt = static_cast<uint8_t>((h + 1u) & (KNOCK_RING - 1u));
    if (nxt == knock_tail_) return;          // full → drop
    knock_ring_[h] = cyl;
    knock_head_    = nxt;
    if (knock_worker_pend) knock_worker_pend();   // wake the burst worker (relay does the FromISR notify)
}

bool EnginePositionHal::pop_knock_cyl(uint8_t& cyl) noexcept {
    const uint8_t t = knock_tail_;
    if (t == knock_head_) return false;      // empty
    cyl = knock_ring_[t];
    knock_tail_ = static_cast<uint8_t>((t + 1u) & (KNOCK_RING - 1u));
    return true;
}

AngleDeg10 EnginePositionHal::pw_angle_dd(uint32_t rpm_x10, uint32_t pw_us) const noexcept
{
    // Same rate as the dwell angle: decideg = rpm_x10 · µs / 166660. Clamp to the cycle span so an
    // absurd pw at very low RPM cannot wrap the open angle past a whole cycle.
    uint32_t dd = static_cast<uint32_t>(((uint64_t)rpm_x10 * pw_us) / 166660ULL);
    const uint32_t cyc = static_cast<uint32_t>(scheduler_.cycle_angle());
    if (cyc > 0 && dd >= cyc) dd = cyc - 1;
    return static_cast<AngleDeg10>(dd);
}

// The window a dwell may span, and the clamp to it. Pure, so it is testable on its own.
// A SHARED COIL IS ONLY FREE BETWEEN ITS OWN SPARKS. A distributor's one coil fires every cycle/ncyl; a
// wasted-spark pair's every 360°. The dwell for the next spark cannot start before the current one has
// fired and burned, so it is clamped to that spacing, not to the whole cycle: a V8 distributor at 6000 rpm
// has 2.5 ms between sparks, and a 3 ms dwell started the next charge before the spark it followed — both
// fired weak, and the engine misfired across the top end.
uint32_t EnginePositionHal::clamp_dwell_dd(uint32_t dd, IgnitionCoilMode mode, AngleDeg10 cycle, uint8_t ncyl,
                                           bool phase_known, bool rotary) noexcept
{
    uint32_t window = phase_known ? static_cast<uint32_t>(cycle) : static_cast<uint32_t>(ANGLE_360);
    uint32_t margin = 200u;                                        // 20° of burn/recovery, at least
    if (!rotary) {
        if (mode == IgnitionCoilMode::SINGLE_COIL_DISTRIBUTOR && ncyl > 1) {
            window = static_cast<uint32_t>(cycle) / ncyl;
            margin = std::max<uint32_t>(margin, window / 4u);        // ~75 % max duty on the one coil
        } else if (mode == IgnitionCoilMode::WASTED_SPARK) {
            window = std::min<uint32_t>(window, static_cast<uint32_t>(ANGLE_360));
        }
    }
    const uint32_t max_dd = (window > margin) ? window - margin : window / 2u;
    return (dd > max_dd) ? max_dd : dd;
}

uint32_t EnginePositionHal::dwell_angle_dd(uint32_t rpm_x10, uint32_t dwell_us) const noexcept
{
    // dwellAngle(decideg) = rpm · dwell_us / 16666  (rpm_x10 · dwell_us / 166660).
    uint32_t dd = static_cast<uint32_t>(((uint64_t)rpm_x10 * dwell_us) / 166660ULL);
    // The dwell is armed a full cycle ahead (at discharge), so it may span nearly
    // the whole firing window: a revolution at CRANK (wasted fires every 360°), a
    // full cycle at PHASE. Clamp to the available window (truncated, never stuck).
    // On a two-stroke the cycle IS 360, so phase sync does not buy a longer window.
    return clamp_dwell_dd(dd, scheduler_.ign_mode(), scheduler_.cycle_angle(), scheduler_.cylinder_count(),
                          scheduler_.phase_known(), scheduler_.is_rotary());
}

// IGN_SCHEDULE: arm the SPARK(s) with fresh advance (~90° ahead). The dwell is NOT
// armed here — it is rescheduled when the previous spark discharges.
void EnginePositionHal::recompute_ignition(uint8_t cyl) noexcept
{
    if (cyl >= scheduler_.cylinder_count()) return;
    const AngleDeg10 tdc = scheduler_.tdc_angle(cyl);
    const AngleDeg10 cyc = scheduler_.cycle_angle();

    const uint32_t   rpm = get_rpm_x10();
    const AngleDeg10 spark = angle_btdc(tdc, shadow_[cyl].spark_btdc, cyc);
    scheduler_.arm_spark(cyl, spark);
    // …and make the dwell precede THIS spark, on this rpm (EventScheduler::fit_dwell).
    scheduler_.fit_dwell(cyl, angle_wrap(static_cast<AngleDeg10>(
        spark - static_cast<AngleDeg10>(dwell_angle_dd(rpm, shadow_[cyl].dwell_us))), cyc), false);

    if (scheduler_.is_rotary()) {
        const AngleDeg10 tspark = angle_btdc(tdc, shadow_[cyl].trailing_spark_btdc, cyc);
        scheduler_.arm_spark_trailing(cyl, tspark);
        scheduler_.fit_dwell(cyl, angle_wrap(static_cast<AngleDeg10>(
            tspark - static_cast<AngleDeg10>(dwell_angle_dd(rpm, shadow_[cyl].trailing_dwell_us))), cyc), true);
    }

    // Knock sampling window: arm this cylinder's ADC burst once per cycle, only when knock windowing is
    // enabled — at Window Start, or earlier by the pre-ignition look-ahead before THIS spark.
    //
    // ARMED FROM THE IGNITION SCHEDULE, not the injection one. The window is a per-cylinder
    // observation of a per-cylinder combustion event, and injection is no longer per cylinder: under
    // multi-point an eight-cylinder engine holds two injection events, so hanging the windows off
    // INJ_SCHEDULE would have armed two of the eight and silently stopped listening to the rest.
    if (knock_window_en_) {
        const AngleDeg10 open = knock_open_btdc(knock_window_start_, knock_lookahead_, shadow_[cyl].spark_btdc);
        knock_open_btdc_[cyl] = open;
        scheduler_.arm_knock_window(cyl, angle_btdc(tdc, open, cyc));
    }
}

AngleDeg10 EnginePositionHal::knock_open_btdc(AngleDeg10 start_btdc, AngleDeg10 lookahead,
                                              AngleDeg10 spark_btdc) noexcept
{
    AngleDeg10 open = start_btdc;
    if (lookahead > 0) {
        const AngleDeg10 pre = static_cast<AngleDeg10>(spark_btdc + lookahead);
        if (pre > open) open = pre;
    }
    // ARMED FROM IGN_SCHEDULE, which runs SCHED_COMPUTE_LEAD (90 deg) before TDC: a window asked to open
    // earlier than that is already behind the crank when it is armed, and would fire a whole cycle late.
    // 5 deg of margin for the compute itself.
    constexpr AngleDeg10 kEarliest = static_cast<AngleDeg10>(SCHED_COMPUTE_LEAD - 50);
    return open > kEarliest ? kEarliest : open;
}

// Spark discharge → reschedule the NEXT dwell from live RPM (full cycle of lead,
// so dwell length is unconstrained). Guarded inside the scheduler against the
// still-pending case, so either spark (leading/trailing) may trigger it.
void EnginePositionHal::recompute_dwell(uint8_t cyl) noexcept
{
    if (cyl >= scheduler_.cylinder_count()) return;
    const uint32_t   rpm = get_rpm_x10();
    const AngleDeg10 tdc = scheduler_.tdc_angle(cyl);
    const AngleDeg10 cyc = scheduler_.cycle_angle();

    const AngleDeg10 spark = angle_btdc(tdc, shadow_[cyl].spark_btdc, cyc);
    const uint32_t   dwd   = dwell_angle_dd(rpm, shadow_[cyl].dwell_us);
    scheduler_.arm_dwell(
        cyl, angle_wrap(static_cast<AngleDeg10>(spark - static_cast<AngleDeg10>(dwd)), cyc));

    if (scheduler_.is_rotary()) {
        const AngleDeg10 tspark = angle_btdc(tdc, shadow_[cyl].trailing_spark_btdc, cyc);
        const uint32_t   tdwd   = dwell_angle_dd(rpm, shadow_[cyl].trailing_dwell_us);
        scheduler_.arm_dwell_trailing(
            cyl, angle_wrap(static_cast<AngleDeg10>(tspark - static_cast<AngleDeg10>(tdwd)), cyc));
    }
}

// INJ_SCHEDULE: arm one injection EVENT. The event knows which channels it drives and which TDC its
// firing angle is measured back from; the reference cylinder supplies the fuel. In full-sequential
// that reference is the event's own cylinder, and this is the old per-cylinder behaviour exactly; in
// batch or multi-point one event delivers the same commanded pulse to a whole group.
void EnginePositionHal::recompute_injection(uint8_t event) noexcept
{
    if (event >= scheduler_.inj_event_count()) return;
    const uint8_t ref = scheduler_.inj_event_ref_cyl(event);
    if (ref >= MAX_CYLINDERS) return;
    const uint8_t stage = scheduler_.inj_event_stage(event);   // 0 = primary; 1..N = staged stage 2..4

    const AngleDeg10 cyc  = scheduler_.cycle_angle();
    const AngleDeg10 base = scheduler_.inj_event_base_angle(event);
    const uint32_t   rpm  = get_rpm_x10();

    // PW + open angle come from THIS event's stage — each event is one stage now. A staged stage that
    // the fuel calc's duty progression did not engage this cycle (staged_enabled) delivers nothing: leave
    // its open node unscheduled rather than arm a zero pulse.
    uint32_t   pw;
    AngleDeg10 open_btdc;
    if (stage == 0) {
        pw        = shadow_[ref].inj_pw_us;
        open_btdc = shadow_[ref].inj_open_btdc;
    } else {
        const uint8_t si = static_cast<uint8_t>(stage - 1);
        if (si >= MAX_STAGED_STAGES || !shadow_[ref].staged_enabled[si]) return;
        pw        = shadow_[ref].inj_secondary_pw_us[si];
        open_btdc = shadow_[ref].inj_secondary_open_btdc[si];
    }

    // Injector Timing Method. The firing-angle table is END OF INJECTION by default, so the injector must
    // OPEN a pulse-width of angle earlier for the close to land on the set angle (extra BTDC = pw as angle
    // at live RPM). Start-of-injection opens AT the angle (offset 0).
    const AngleDeg10 off = scheduler_.inj_end_of_injection() ? pw_angle_dd(rpm, pw) : 0;
    scheduler_.arm_injection(
        event, angle_btdc(base, static_cast<AngleDeg10>(open_btdc + off), cyc), pw);
}

// ---------------------------------------------------------------------------
// State accessors
// ---------------------------------------------------------------------------
DecoderTelemetry EnginePositionHal::get_decoder_telemetry() const noexcept
{
    // Generic decoder: sync_level + live RPM + sync-health counters. The gap matcher reports every
    // TOLERATED gap-miss (it rides through without losing sync), so teeth/errors_last_cycle are real —
    // EngineTask derives trigger_error_pct and EngineProtection records the DTC. No longer reads 0.
    DecoderTelemetry t{};
    t.current_angle      = gtrig_.angle();
    t.current_rpm_x10    = get_rpm_x10();
    t.teeth_last_cycle   = gtrig_.teeth_last_cycle();
    t.errors_last_cycle  = gtrig_.errors_last_cycle();
    t.trigger_error_total= gtrig_.error_total();
    t.last_error_tooth   = gtrig_.last_error_tooth();
    t.last_error_kind    = gtrig_.last_error_kind();
    t.edges_total        = gtrig_.edges_total();
    t.noise_total        = gtrig_.noise_total();
    t.missed_total       = gtrig_.missed_total();
    t.phase_lost_total   = gtrig_.phase_lost_total();
    t.fault_stream       = gtrig_.last_error_stream();
    t.trigger_absent     = trigger_absent_;
    return t;
}

VvtResult EnginePositionHal::get_vvt_result(uint8_t cam_index) const noexcept
{
    if (cam_index >= MAX_CAM_CHANNELS) return { cam_index, 0, false };
    VvtResult result;
    std::memcpy(&result,
                const_cast<const VvtResult*>(&vvt_results_[cam_index]),
                sizeof(VvtResult));
    // A MEASUREMENT IS ONLY AS GOOD AS THE LOCK IT CAME FROM. The stored result is written when the cam
    // stream locks and is never withdrawn afterwards, so a cam that went quiet, a lost sync or a stopped
    // engine all went on reporting the last angle as a live one — and the VVT loop, which holds on base
    // duty when it has NO angle, was instead closing on a frozen one. Valid only while this cam's stream
    // is locked and absolute right now.
    bool live = false;
    if (get_sync_level() != SyncLevel::NONE)
        for (uint8_t s = 0; s < MAX_STREAMS; ++s)
            if (gen_cam_index_[s] == cam_index && gtrig_.stream_locked_absolute(s)) { live = true; break; }
    result.valid = result.valid && live;
    return result;
}


SyncLevel EnginePositionHal::get_sync_level() const noexcept
{
    return static_cast<SyncLevel>(static_cast<uint8_t>(gtrig_.level()));
}

uint32_t EnginePositionHal::get_rpm_x10() const noexcept
{
    if (!assignment_.timebase) return 0;
    // No position lock → no meaningful RPM (and don't report a stale period).
    if (get_sync_level() == SyncLevel::NONE) return 0;

    // SINGLE SOURCE OF TRUTH: RPM is derived from the PLL's live ticks-per-virtual-
    // tooth — the SAME value the fire-tick interpolation uses — not a second EMA.
    // (The PLL's own velocity EMA already smooths it; a scheduler-side re-filter was
    // redundant, added lag, and made dwell_angle ride a different speed estimate than
    // the spark it pairs with.)
    const uint32_t tpv = grid_tpv_;          // PLL ticks per virtual tooth (live)
    if (tpv == 0) return 0;

    const uint32_t tps   = assignment_.timebase->get_ticks_per_second();
    // Virtual teeth per crank rev = 3600 / grid pitch (e.g. 36 at a 10° grid).
    const uint16_t teeth = (grid_vt_step_ > 0)
        ? static_cast<uint16_t>(ANGLE_360 / grid_vt_step_)
        : 36u;   // pre-first-grid-tooth fallback (grid_vt_step_ is set on the first vtooth)
    if (teeth == 0) return 0;

    //   rev_period = teeth · tpv / tps  (seconds)
    //   rpm_x10    = 10 · 60 · tps / (teeth · tpv)
    // 32-bit hardware UDIV (variable divisor → would be a software __udivdi3 in 64-bit).
    // At the 1 MHz timebase both num (tps·600 = 6e8) and den (teeth·tpv ≤ ~43M) fit uint32.
    const uint32_t den = static_cast<uint32_t>(teeth) * tpv;
    if (den == 0) return 0;
    return (tps * 600u) / den;
}

// ---------------------------------------------------------------------------
// Pool rebuild (engine task context)
// Uses the locked active_[] snapshot. Only IGN_SPARK and INJ_OPEN events are
// added to the pool — their time-domain companions (dwell start, injector close)
// are generated implicitly by EventScheduler's chained ISR.
// ---------------------------------------------------------------------------
// BTDC — "how many degrees BEFORE TDC". The angle axis increases with rotation (the decoder adds a
// tooth pitch per tooth, and dwell is armed at spark-minus-dwell), so earlier in the cycle means a
// SMALLER angle: subtract. An ignition table holds advance, all positive, so 34.0 deg advance must
// fire 34 deg before its cylinder's TDC, and a negative value retards past TDC.
AngleDeg10 EnginePositionHal::angle_btdc(AngleDeg10 tdc, AngleDeg10 btdc,
                                         AngleDeg10 cycle) noexcept
{
    return angle_wrap(static_cast<AngleDeg10>(tdc - btdc), cycle);
}

// ATDC — "how many degrees AFTER TDC". The knock sampling window is specified this way (it opens
// after the spark), so it adds.
AngleDeg10 EnginePositionHal::angle_atdc(AngleDeg10 tdc, AngleDeg10 atdc,
                                         AngleDeg10 cycle) noexcept
{
    return angle_wrap(static_cast<AngleDeg10>(tdc + atdc), cycle);
}

// Decoder (wheel-referenced) angle -> engine (TDC#1-referenced) angle. See the header: the reference
// sits `offset` degrees BEFORE TDC #1, so TDC #1 is reached `offset` later and the engine angle is
// the decoder angle minus the offset. Wrapped into the caller's live cycle span.
AngleDeg10 EnginePositionHal::engine_angle(AngleDeg10 decoder_angle, AngleDeg10 offset_btdc,
                                           AngleDeg10 cycle) noexcept
{
    if (cycle <= 0) return decoder_angle;
    int32_t a = static_cast<int32_t>(decoder_angle) - static_cast<int32_t>(offset_btdc);
    a %= cycle;                       // offset may exceed the span (a 720 offset on a 360 grid)
    if (a < 0) a += cycle;
    return static_cast<AngleDeg10>(a);
}


// ---------------------------------------------------------------------------
// ISR trampolines
// ---------------------------------------------------------------------------

// Bench-validation hook (weak): the VirtualTrigger angle clock taps the same
// per-tooth stream the scheduler sees. Defined in main.cpp during bring-up;
// absent (resolves null) in production builds, so this couples nothing.
extern void vtrig_feed(AngleDeg10 angle, uint32_t ticks, uint32_t T0,
                       AngleDeg10 tooth_angle, bool last_was_gap) noexcept __attribute__((weak));

// ---- Generic-trigger capture path -----------------------------------------
// One capture edge → GenericTrigger.on_edge → fusion. When the fine (velocity) source
// advanced, feed the VirtualTrigger exactly like the legacy tooth callback does, so the
// downstream (grid → scheduler → RPM) is identical regardless of decoder.
void EnginePositionHal::gen_edge_trampoline(uint32_t tick, void* ud) noexcept
{
    auto* c = static_cast<GenCtx*>(ud);
    c->self->on_gen_edge(c->idx, tick);
}

void EnginePositionHal::on_gen_edge(uint8_t s, uint32_t tick) noexcept
{
    const bool rising = gen_ch_[s]
        ? (gen_ch_[s]->get_edge_polarity() == CaptureEdge::RISING) : true;

    // RAW TRIGGER LOG, before the decoder is handed the edge. Deliberately first: what lands here is
    // what the pin did, not what survived the matcher, and a logger downstream of decode cannot show
    // the noise that broke the decode.
    if (trig_log_) trig_log_->record(s, rising, tick);

    // KEY OFF -> THE EDGE STOPS HERE. On USB-only power the ECU must behave as though it had just come
    // out of reset: no trigger consumed, no sync, no rpm. Without this the capture ISR fed the decoder
    // whatever the rig was spinning, so an ECU with no 12 V would acquire sync and publish an rpm off a
    // bench stim — and everything keyed off "the engine is turning" would believe it.
    //
    // Placed AFTER the raw log deliberately: that log is an operator-armed diagnostic and is exactly
    // what a bench wants to look at with no engine and no key. Everything below it is the ECU forming
    // a belief about a running engine, which is the part that must not happen. A bench that wants the
    // decoder as well forces the key ('key on'), which is what that override is for.
    extern bool g_system_active;                  // key-on (battery over threshold), owned by Sensors
    if (!g_system_active) return;

    // Raw liveness, before any decision about the decoder: the crank is turning if teeth are
    // arriving, whatever anything downstream makes of them.
    last_edge_tick_ = tick;
    last_edge_tick_valid_ = true;

    // THE LOG DOES NOT TOUCH THE DECODER. It sits above this line, taking a copy of the edge, and
    // that is the whole of its involvement: it never withholds an edge, never disables decoding and
    // never clears sync. It used to do all three, on the reasoning that an unknown wheel produces
    // garbage sync states — but the decoder already refuses a wheel it cannot match, and paying for
    // that with a mode that stops the engine decoding meant a capture could not be taken on a
    // running engine, could not be taken WHILE starting one, and left the ECU dead if the host that
    // armed it went away. Recording what a pin did costs a struct write in an ISR; it should cost
    // nothing else.

    const SyncLvl lvl_before = gtrig_.level();
    gtrig_.on_edge(s, tick, rising);

    // THE SCHEDULER HALTS HERE, not a frame later. The matcher has just judged this edge, and if it
    // fell outside the window the lock is gone and the decoder has reset to acquiring — but firing
    // is only recomputed by service() on the 1 kHz task. At 6000 rpm that gap is up to 36 degrees of
    // crank spent firing on a position the decoder has already stopped trusting, which is precisely
    // the state everything else here exists to prevent.
    //
    // Clearing the enable is the cheap half and the one that matters: armed events test it at
    // dispatch, so nothing more comes out. The rest of the teardown — volatiles unlinked, outputs
    // de-energised, the grid re-narrowed — is heavier than a capture ISR should carry and follows on
    // the next service() through handle_sync_level_change, which is soon enough for a coil that is
    // mid-dwell and far too late for a spark that is not.
    if (lvl_before != SyncLvl::NONE && gtrig_.level() == SyncLvl::NONE)
        scheduler_.set_firing_enabled(false);

    // VVT: a cam-rate stream that has locked an absolute angle gives the cam's measured
    // position in the 720° cycle. Cam advance = measured − nominal, normalised to
    // (−360°, +360°]. Published per cam slot for telemetry; valid once the stream locks.
    // Engine-cycle capture: the trigger lane. Recorded HERE, after on_edge has advanced the
    // PLL, so the tooth carries the angle the decoder actually resolved it to — the same angle
    // everything downstream schedules against. This is the reference the output lanes are read
    // against; without it a spark at 685 deg is a number rather than a position on a wheel.
    // Written from the capture ISR (prio 1), which is why the recorder keeps the trigger edges
    // in their own single-writer ring.
    // Counted-angle wrap, evaluated in THIS ISR from the decoder's own position, so no flag crosses
    // an interrupt priority. Held across calls because only the capture ISR (prio 1) runs this line
    // and it cannot preempt itself.
    // THE CYCLE BOUNDARY for the trigger lane: COUNT THE TEETH.
    //
    // Four earlier designs derived this from state read at edge time — the decoder's whole-cycle
    // angle, then the fine angle plus rev_, then a count of revolution wraps, then a flag set by the
    // grid ISR. Every one of them broke, and the measurements say why. gtrig_.angle() carries a
    // revolution component re-derived from the cam (rev_ = A / 360, so a cam pulse in the first
    // revolution reports 0 for ever) and revs_per_cycle() came back as one revolution after a
    // restart: a 116-tooth cycle was served as 37 + 79, then as 58. The grid flag fixed that — every
    // restart landed on 116 — but it is set at priority 2 and read at priority 1, so a tooth
    // arriving either side of the set fell in either cycle and the old +/-1 jitter returned (5 of
    // 117 and 5 of 115).
    //
    // A wheel produces a FIXED number of teeth per cycle, and that number is configuration, not
    // decode state — it cannot be wrong after a restart because nothing re-derives it. So the count
    // lives here, in the capture ISR, incremented and tested by the same interrupt that records the
    // edge. Nothing is read across a priority boundary, so there is no window in which a tooth can
    // land on the wrong side of anything, and every window holds exactly N teeth by construction.
    bool cycle_wrapped = false;
    const bool crank_edge = (gen_cam_index_[s] >= MAX_CAM_CHANNELS);
    if (crank_edge && teeth_per_rev_ > 0) {
        // At PHASE the cycle holds every repeat of the pattern; without a cam the fold span is ONE
        // period, which is what the angles are folded into. Counted in REPEATS rather than
        // revolutions, because those are the same number only for an ordinary crank wheel.
        const uint16_t reps = (gtrig_.level() == SyncLvl::PHASE) ? crank_repeats_ : 1u;
        const uint16_t n = static_cast<uint16_t>(teeth_per_rev_ * (reps ? reps : 1u));
        if (++teeth_since_mark_ >= n) { teeth_since_mark_ = 0; cycle_wrapped = true; }
    }

    if (recorder_ && recorder_->recording()) {
        // Identify the lane by its ROLE SLOT, not the stream index. They differ as soon as there is
        // more than one stream: on a crank+cam config the cam is stream 1 but cam slot 0, so passing
        // the stream index labelled it "Cam 2" when it is the FIRST cam. The rest of the system
        // (VVT, the resolver, the assignment) names cams by slot, and a lane that disagrees with the
        // dialogs is worse than an unnamed one.
        const uint8_t cam = gen_cam_index_[s];
        const bool    is_cam = (cam < MAX_CAM_CHANNELS);
        const int     crank_slot = crank_slot_of(s);
        recorder_->record_trigger(
            is_cam ? CycleSignal::Cam : CycleSignal::Crank,
            static_cast<uint8_t>(is_cam ? cam : (crank_slot >= 0 ? crank_slot : 0)),
            // The angle the FIRING CLOCK believed when this tooth physically arrived — NOT the
            // decoder's counted position. The counted position is what the grid is built from, so
            // stamping teeth with it makes this lane and the Virtual lane identical by construction
            // and their difference permanently zero. Measured against the grid, a tooth that lands
            // early or late is visible, which is the entire reason both lanes exist.
            scheduler_.angle_at(tick), rising,
            // The cycle boundary, and the ONLY thing that decides which cycle these teeth belong to.
            // An engine cycle is the physical trigger pattern occurring the stroke's number of times,
            // and gtrig_.angle() is exactly that: the decoder's COUNTED position across the whole
            // cycle. Counted, so its wrap is a fact about the wheel. The stamp above is the firing
            // clock's belief and near the boundary reads 719.9 deg as readily as 0.1 — cutting on it
            // put one tooth in the wrong cycle in 10 captures out of 3753.
            cycle_wrapped);
    }

    const uint8_t cam = gen_cam_index_[s];
    if (cam < MAX_CAM_CHANNELS && gtrig_.stream_locked_absolute(s)) {
        // PREFER THE MEASURED RESIDUAL. The decoder now compares the CRANK's position against this
        // cam's parked angle at every cam sync, and that difference is the displacement. The old form
        // below subtracts nominal from the stream's own angle, which works for a SEQUENCE cam (its
        // angle advances per event) but not for a WIDTH one: WidthMatcher::angle() returns the
        // configured target, so it is one constant minus another and reports a cam that never moves
        // however far its phaser travels. A VVT loop closing on that has no feedback at all.
        //
        // AND IT IS PUBLISHED AS ADVANCE. The residual is crank-at-the-edge minus nominal, so a cam that
        // fires EARLIER — an advanced cam — reads NEGATIVE. The channel is "cam advance" and the VVT
        // loop's targets are degrees of advance (target - measured), so publishing the residual as it
        // stood gave the loop positive feedback: advancing the cam grew the error it was correcting,
        // and it ran to a stop whichever Direction was set. Negated here, once, at the boundary.
        AngleDeg10 disp = static_cast<AngleDeg10>(-(gtrig_.phase_resid_valid(s)
            ? gtrig_.phase_residual(s)
            : static_cast<AngleDeg10>(gtrig_.stream_angle(s)
                                                  - assignment_.cam_nominal_angle[cam])));
        if (disp >   ANGLE_360) disp = static_cast<AngleDeg10>(disp - ANGLE_720);
        if (disp <= -ANGLE_360) disp = static_cast<AngleDeg10>(disp + ANGLE_720);
        const VvtResult r{ cam, disp, true };
        std::memcpy(const_cast<VvtResult*>(&vvt_results_[cam]), &r, sizeof(VvtResult));
    }

    // THE boundary. Everything downstream of this line — the virtual-trigger grid, the scheduler,
    // the knock window, the engine-sync sampler — works in ENGINE angle, so a per-cylinder TDC is a
    // plain engine angle (cyl 1 = 0) and nothing downstream needs to know the wheel exists. Applying
    // it further in (at TDC consumption) would fix firing and leave telemetry reading wheel angle,
    // which is two coordinate systems and the bug that follows from having them.
    // THE POSITION TOOTH ARRIVED. Stamp it and re-arm the decoder's deadline from the schedule this
    // tooth just advanced — the prediction is only valid once the matcher has consumed the edge.
    // Kept separate from the PLL feed below because a build without an angle clock (host tests) has
    // no vtrig_feed, and liveness must not depend on the firing clock existing. That coupling is the
    // bug this whole change exists to remove.
    if (gtrig_.fine_advanced()) {
        last_fine_tick_       = tick;
        last_fine_tick_valid_ = true;
        tooth_overdue_reported_ = false;
        trigger_absent_       = false;   // a tooth arrived: whatever was wrong, it is not absence
        arm_tooth_deadline(tick);
    }
    if (gtrig_.fine_advanced() && vtrig_feed) {
        const AngleDeg10 cycle = (gtrig_.level() == SyncLvl::PHASE) ? scheduler_.cycle_angle()
                                                                    : ANGLE_360;
        const AngleDeg10 eang = engine_angle(gtrig_.pll_angle(), trig_offset_btdc_, cycle);
        vtrig_feed(eang, tick, gtrig_.velocity(), gtrig_.fine_pitch(), false);
        // SEGMENT TIMING taps the same tooth, at the same instant, BEFORE the PLL sees it. Only at
        // PHASE sync: a segment is a named cylinder's expansion stroke, and at CRANK level the two
        // revolutions are indistinguishable, so there is no cylinder to name.
        if (seg_timer_ && gtrig_.level() == SyncLvl::PHASE)
            seg_timer_->on_tooth(eang, tick, gtrig_.fine_pitch());
    }
}

// Driven by the VirtualTrigger per virtual tooth. The scheduler dispatches the
// events bucketed under this tooth, interpolating each fire-tick from the grid's
// (tick, ticks_per_vtooth). index is the virtual-tooth number this cycle.
void EnginePositionHal::on_grid_tooth(uint16_t index, AngleDeg10 angle, uint32_t tick,
                                      uint32_t velocity_ticks_per_vtooth,
                                      AngleDeg10 vt_step) noexcept
{
    grid_vt_step_ = vt_step;
    grid_tpv_     = velocity_ticks_per_vtooth;   // single velocity source for RPM/dwell
    g_engine_sync.feed(vt_step);                 // angle-window ADC averaging (engine-sync sensors)
    // The engine-cycle capture's boundary, taken BEFORE the scheduler dispatches this tooth so
    // an event sitting exactly on tooth 0 lands inside the capture it belongs to rather than
    // being clipped off the front. rpm and sync level are latched from the position authority
    // at the instant the capture starts.
    if (recorder_) {
        recorder_->on_cycle_boundary(index, scheduler_.cycle_angle(), get_rpm_x10(),
                                     static_cast<uint8_t>(get_sync_level()));
        // The grid's OWN angle, not a recomputed index*step: this is what the firing clock believed
        // at this instant, and comparing it against the real teeth is the only way PLL error becomes
        // visible. Recorded AFTER the boundary call so tooth 0 lands inside the capture it opens.
        recorder_->record_virtual(angle, index == 0);
    }
    scheduler_.on_virtual_tooth(index, tick, velocity_ticks_per_vtooth);
}

// ---------------------------------------------------------------------------
// Lost-trigger watchdog
// ---------------------------------------------------------------------------
// Fixed low-RPM floor for the stall timeout. Not a user knob: it is the "the engine physically cannot
// be turning slower than this and still be running" bound. It only sets
// the CEILING on how long the watchdog waits — the actual timeout scales down with the wheel's tooth
// count in the VirtualTrigger. One revolution at this floor is the longest silence ever tolerated.
static constexpr uint16_t kStallRpmFloor = 30;
// How far past its PREDICTED arrival a tooth is overdue: the SAME window the matcher applies to an
// edge that does arrive, which is the stream's own window_pct. It was a fixed 1.5x, which made the
// two disagree — with the default 25% window a tooth arriving at 1.3x was rejected the moment it
// landed, while one that never came was tolerated until 1.5x. One rule for both, and it is the same
// per-stream knob a tuner already sets, rather than a constant that cannot follow a wheel.

void EnginePositionHal::configure_stall_watchdog() noexcept
{
    stall_floor_ticks_ = 0; stall_min_ticks_ = 0;
    if (!assignment_.timebase) return;      // no clock -> no liveness detection (host tests, no tune)
    const uint32_t tps = assignment_.timebase->get_ticks_per_second();
    // One crank revolution at the floor RPM (60/floor s) — the longest silence ever tolerated,
    // whatever the wheel. tps*60 fits uint32 at 1 MHz (6e7).
    stall_floor_ticks_ = (tps / kStallRpmFloor) * 60u;
    // And a 1 ms absolute minimum on the OVERDUE deadline. This guards against a pathological
    // prediction, not against noise: noise shortens intervals and cannot cause an absence, and a
    // bogus short prediction would need a bogus short measured interval, which the minimum-interval
    // reject now refuses before the matcher ever sees it. The old bound here was 0.1 s, inherited
    // from a design where this same number was the SYNC-LOSS threshold and had to be conservative.
    // Left at 0.1 s it swamped the prediction entirely — at 600 rpm a tooth is 2.8 ms, so every
    // missed tooth was reported 100 ms late or not at all. A floor can only make the report later
    // than the wheel says, never earlier, which is the safe direction for a floor to err in.
    stall_min_ticks_   = tps / 1000u;
    if (assignment_.tooth_alarm)
        assignment_.tooth_alarm->register_callback(tooth_deadline_trampoline, this);
}

void EnginePositionHal::tooth_deadline_trampoline(void* ud) noexcept {
    static_cast<EnginePositionHal*>(ud)->on_tooth_deadline();
}

// Arm for the next tooth the decoder predicts, measured from `from_tick`. One CCR write; called per
// fine tooth, which at 8000 rpm on a 60-2 is ~7.7k times a second and still under 0.1% of the core.
void EnginePositionHal::arm_tooth_deadline(uint32_t from_tick) noexcept
{
    if (!assignment_.tooth_alarm || stall_floor_ticks_ == 0) return;
    if (gtrig_.level() == SyncLvl::NONE) { disarm_tooth_deadline(); return; }
    // The decoder's own prediction, stretched by the overdue margin. 0 means it has no prediction
    // yet (locked but no interval measured) — fall back to the floor, which is conservative and
    // still bounded, rather than leaving the deadline unarmed.
    const uint32_t expected = gtrig_.expected_fine_ticks();
    // The band's upper edge: expected plus half a tooth pitch. A tooth is invalid precisely when
    // nothing lands between half and one and a half pitches of where it was due, so the deadline for
    // an edge that never comes sits exactly where the matcher stops accepting one that does.
    uint32_t to = (expected == 0) ? stall_floor_ticks_ : (expected + gtrig_.fine_tol_ticks());
    if (to < stall_min_ticks_)   to = stall_min_ticks_;
    if (to > stall_floor_ticks_) to = stall_floor_ticks_;
    assignment_.tooth_alarm->arm(from_tick + to);
}

void EnginePositionHal::disarm_tooth_deadline() noexcept
{
    if (assignment_.tooth_alarm) assignment_.tooth_alarm->disarm();
    tooth_overdue_reported_ = false;
}

// ---------------------------------------------------------------------------
// The deadline fired: a tooth the decoder predicted did not arrive.
//
// This is the whole point of the decoder owning a clock. Everything else in the decode chain is
// edge-driven — matchers, fusion, the sync level and every trigger DTC change state only when an
// edge arrives — so absence, the one failure that matters most, was the one thing none of them
// could see. It used to be answered inside the DCO's match ISR, which meant it was disarmed by
// every sync transition (each resets the DCO) and an ECU could hold PHASE sync and a plausible RPM
// for ever with no wheel connected at all.
// ---------------------------------------------------------------------------
void EnginePositionHal::on_tooth_deadline() noexcept
{
    const uint32_t gap    = timebase_now() - last_fine_tick_;   // unsigned: wraps correctly
    // Past one crank revolution at the RPM floor nothing is turning at all, whatever the wheel.
    const bool     absent = (!last_fine_tick_valid_ || gap >= stall_floor_ticks_);

    if (get_sync_level() != SyncLevel::NONE) {
        // THE SAME RULE THE MATCHER APPLIES TO AN EDGE THAT ARRIVES, applied to one that does not.
        // A tooth outside the window the schedule predicted means the decoder cannot prove where the
        // engine is, and that is exactly as true when the tooth never comes as when it comes late.
        // So this cuts; it does not report and continue. There is no count of missing teeth at which
        // an engine of unknown position becomes safe to go on firing.
        //
        // …BUT AN ENGINE WINDING DOWN IS NOT A FAULT. Every stop ends like this: the teeth slow, one
        // comes later than the window allows, and the deadline fires. Recording that as a missed tooth
        // raised P0336 at the next start, and 2 s later every stopped engine reported its trigger wire
        // missing (P0338, Level 3) for as long as the key stayed on. So the speed at the moment of loss
        // decides: at or above the Cranking Threshold the engine was running and a vanished tooth is a
        // fault; below it the engine was stopping (or stalling through cranking speed), and the sync is
        // dropped exactly the same — firing stops — with nothing recorded. Read BEFORE on_trigger_lost(),
        // which zeroes the rpm.
        const uint32_t rpm_x10  = get_rpm_x10();
        const uint32_t floor_x10 = static_cast<uint32_t>(g_config.engine.cranking_rpm) * 10u;
        lost_at_speed_ = (rpm_x10 >= floor_x10);
        if (lost_at_speed_) {
            if (absent) trigger_absent_ = true;
            gtrig_.note_kind(absent ? TriggerErrorKind::SIGNAL_ABSENT : TriggerErrorKind::MISSED_TOOTH);
        }
        on_trigger_lost();                       // drops sync, and disarms this deadline
        if (!absent) {
            // The engine is already cut. Keep asking anyway, for the DIAGNOSIS: a tooth that did not
            // arrive and a trigger wire that is gone need the same cut but send an operator to two
            // completely different places, and at the moment of the cut they are indistinguishable.
            // Only time tells them apart.
            assignment_.tooth_alarm->arm(last_fine_tick_ + stall_floor_ticks_);
        }
        return;
    }

    // Sync is already gone: this is that follow-up, and it changes nothing but the fault text. Only for
    // teeth that vanished at speed — a stopped engine has no signal by definition (see above).
    if (absent && !trigger_absent_ && lost_at_speed_) {
        trigger_absent_ = true;
        gtrig_.note_kind(TriggerErrorKind::SIGNAL_ABSENT);
    }
}

void EnginePositionHal::on_trigger_lost() noexcept
{
    // ISR context (VirtualTrigger stall callback). Drop the decoder to NONE — but KEEP the stream config
    // so it re-acquires when the trigger returns. drop_sync() (NOT reset_all(), which zeroes n_ and would
    // leave the decoder dead until a reconfigure) resets the matchers + sync state only. get_sync_level()
    // then reads NONE, so get_rpm_x10() returns 0 and the next service() poll runs the full sync-loss path
    // (grid re-narrow, firing disable) while EngineProtection latches its fuel+ign cut. The DCO is already
    // disarmed by the watchdog. When teeth resume, the decoder locks again from scratch — no reconfigure.
    decoder_reset_tick_ = timebase_now();
    // FIRING STOPS NOW. The edge path (on_gen_edge) already did this; the tooth deadline and the gate
    // did not, and firing went on until the next 1 kHz service() — up to a frame of sparks on a
    // position the decoder had just said it could not prove.
    scheduler_.set_firing_enabled(false);
    gtrig_.drop_sync();
    // THE VELOCITY DIES WITH THE SYNC. get_rpm_x10() returns 0 at NONE, so this is belt and braces —
    // but a stale grid velocity surviving a reset is exactly what made a frozen 1348 rpm look like a
    // measurement for forty minutes. Nothing that outlives the thing it was measured from is safe.
    grid_tpv_     = 0;
    grid_vt_step_ = 0;
    // last_fine_tick_ is deliberately KEPT: the follow-up that decides "late tooth" from "dead
    // wire" measures the silence from the last real tooth, and clearing the stamp would make every
    // sync loss read as an absent signal the moment it happened.
    disarm_tooth_deadline();          // the sync-loss path re-arms it for the diagnosis
    // Write the discontinuity into the capture too. Without it the recorder kept serving the last
    // complete cycle — with its own rpm in the header — for as long as the engine stayed stopped,
    // and the next restart could produce a "cycle" spliced from teeth either side of the gap.
    if (recorder_) recorder_->on_trigger_lost();
}

