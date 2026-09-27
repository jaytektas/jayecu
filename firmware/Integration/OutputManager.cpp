#include "../Diagnostics/ReconfigCost.h"
#include "OutputManager.h"
#include "../Signal/Expr.h"   // the slot conditions
#include "../../generated/table_registry.h"  // expr_table_desc — the table a slot NAMES
#include "../../generated/ecu_config.h"   // g_config (live tune RAM)
#include "../../generated/table_registry.h"   // expr_table_* — the tables an expression may read
#include "../Platform/platform_hal.h"     // platform_get_tick_ms (P1650 timestamp)

namespace {
constexpr uint16_t PCODE_PIN_CONFLICT = 0x1650;   // P1650 — same code EngineTask merges/heals
}

void OutputManager::init(SoftPwm& pwm, uint32_t tps, PinArbiter& arbiter,
                         Comms::CommsManager& comms, DtcManager& dtc) {
    pwm_      = &pwm;
    tps_      = tps;
    arbiter_  = &arbiter;
    comms_    = &comms;
    dtc_      = &dtc;
    built_    = false;     // the first update() builds, AFTER the scheduler claims firing pins
    conflict_ = false;
    n_        = 0;
}

void OutputManager::update(const EnginePosition& /*pos*/, SignalBus& bus, EngineFrame& /*frame*/) {
    // System-active (key-on) gate: on USB/bench power, park EVERY generic output in its defined safe
    // state (release AUX pins + PWM channels -> Hi-Z) instead of actuating module demands, so a
    // bench tune can't pulse a fuel pump / idle valve / boost solenoid. Edge-triggered: release once
    // on entering inactive (built_=false forces a fresh rebuild when key-on returns).
    extern bool g_system_active;
    if (!g_system_active) {
        if (built_) {
            if (pwm_)     pwm_->release_owner(SoftPwm::OWNER_OUTPUTS);
            if (arbiter_) arbiter_->release_owner(PinOwner::AUX);
            n_ = 0; built_ = false;
        }
        return;
    }

    // Region-scoped rebuild: ONLY when a 'w' write hit the Outputs config offset region (its shadow
    // bit) — or the one-time initial build. Never the global g_config_generation, so tuning an
    // unrelated fuel/ignition cell never tears down a live output pin.
    const uint32_t bit = (1u << JAYECU_SHADOW_OUTPUTS);
    const bool region_write = comms_ && (comms_->shadow_pending_mask() & bit);
    extern volatile uint32_t g_firing_bind_generation;
    const uint32_t bind_gen = g_firing_bind_generation;
    if (!built_ || region_write || bind_gen != bind_gen_) {
        bind_gen_ = bind_gen;
        recompute_live();
        built_ = true;
        if (comms_) comms_->clear_shadow(bit);   // consume only the Outputs bit
    }


    // WHEN, then how much. Each slot's condition is answered first (and latched, timed and locked
    // out here), and the arbitration stage short-circuits on a closed gate.
    const uint32_t now_ms = platform_get_tick_ms();
    update_gates(bus, now_ms);
    update_values(bus, now_ms);          // how much, and how fast — the two numeric questions

    // The output stages are stateless w.r.t. the frame (arbitrate/encode/emit), so no time is fed.
    for (uint8_t i = 0; i < n_; i++) pipelines_[i].run(bus, 0, 0.0f);

    // SAY WHAT EACH SLOT IS DOING. One channel per slot: a PWM slot reports its duty, a digital one
    // the level it drove. Only BUILT slots publish — a slot that is disabled, unpinned or in conflict
    // has no state to report, and an invalid channel says that honestly where a 0 would claim "off".
    extern EcuConfig g_config;
    for (uint8_t i = 0; i < n_; i++) {
        const uint8_t slot = slot_of_[i];
        if (slot >= OUT_SIGNAL_COUNT) continue;
        // A DIGITAL SLOT PUBLISHES THE LEVEL IT DROVE, not the command it was given. last_cmd is what
        // the sink was HANDED — recorded before the sink thresholded it — so a slot fed a candidate
        // reading 60 published 60 while the pin was simply on, and one fed 40 would have published 40
        // while the pin was off. Same shape, same units, opposite physical state, and a datalog with no
        // way to tell them apart. The state is the honest answer, and it is the sink's own decision
        // (digital_on) rather than a second copy of the rule that could drift from it.
        //
        // A PWM slot keeps the number: there it IS the duty, which is exactly what a log wants.
        const OutputConfig& o = g_config.outputs.output[slot];
        const float v = (o.kind == 0) ? pools_[i].last_cmd
                                      : (pipe::digital_on(pools_[i].dig, pools_[i].last_cmd) ? 100.0f
                                                                                             : 0.0f);
        bus.set(OUT_SIGNALS[slot], v, true, now_ms, ttl());
    }
}

// Answer each slot's two conditions and hand them to its gate. The VM decides WHAT IS TRUE, the
// gate decides WHAT THAT MEANS OVER TIME (OutputGate.h), and neither knows about the other.
void OutputManager::update_gates(SignalBus& bus, uint32_t now_ms) {
    extern EcuConfig g_config;
    const OutputsConfig& cfg = g_config.outputs;

    for (uint8_t i = 0; i < n_; i++) {
        const OutputConfig& o = cfg.output[slot_of_[i]];
        OutputGate& g = gates_[i];

        // NO CONDITION = ALWAYS ON. An output driven purely by its candidates is not gated at all,
        // and pays nothing for the feature existing.
        const bool has_on  = !expr::is_empty(o.on_expr,  sizeof(o.on_expr));
        const bool has_off = !expr::is_empty(o.off_expr, sizeof(o.off_expr));
        if (!has_on && !has_off) { g.on = 1; continue; }

        expr::Ctx ectx;
        // THE TUNE'S OWN TABLES, readable from an expression. expr_table_value() reads one at its
        // configured axes (OP_TABLE); expr_table_at() reads it at an x the program pushed (OP_INTERP).
        // The bus rides in the user pointer because a table's axes ARE bus channels — which keeps the
        // VM kernel free of the table engine, the way it was written to be.
        ectx.table       = expr_table_value;
        ectx.curve       = expr_table_at;
        ectx.curve_user  = &bus;
        ectx.curve_count = EXPR_TABLE_COUNT;
        ectx.bus      = &bus;
        ectx.cfg      = reinterpret_cast<const uint8_t*>(&g_config);
        ectx.cfg_size = sizeof(g_config);
        ectx.now_ms   = now_ms;

        // `ran` says the program executed at all; v.ok says its RESULT is trustworthy (a dead channel
        // taints the value it feeds). Both must hold for the answer to count as answered — otherwise
        // the slot's own on_invalid policy decides, which is the whole point of that field.
        bool ask_on = false;
        if (has_on) {
            const expr::Result r = expr::exec(o.on_expr, sizeof(o.on_expr), ectx);
            ask_on = gate_answer(r.ran && r.v.ok, r.v.value, o.on_invalid, g.on != 0);
        }
        // ONE CONDITION MEANS "off is not on". Two mean a deadband between them, which is where
        // hysteresis actually comes from.
        bool ask_off;
        if (has_off) {
            const expr::Result r = expr::exec(o.off_expr, sizeof(o.off_expr), ectx);
            // An unanswerable OFF condition follows the same policy, read the other way round: a
            // fail-off slot turns off, a fail-on slot does not, and a holding slot does nothing.
            ask_off = gate_answer(r.ran && r.v.ok, r.v.value,
                                  o.on_invalid == GATE_INVALID_ON  ? GATE_INVALID_OFF :
                                  o.on_invalid == GATE_INVALID_OFF ? GATE_INVALID_ON  : GATE_INVALID_HOLD,
                                  g.on == 0);
        } else {
            ask_off = !ask_on;
        }

        const OutputGateTimings t{ o.min_on_ms, o.min_off_ms, o.max_on_ms, o.rearm_ms };
        g.step(ask_on, ask_off, t, now_ms);
    }
}

// The slot's two NUMERIC answers, for the frame that is about to run: how much, and how fast.
//
// An output is three questions and only the first is a yes/no. The conditions decide WHETHER; these
// decide HOW MUCH (duty) and AT WHAT CARRIER (frequency), and each may be computed or looked up. They
// are evaluated here, with the conditions, for the same reason those are: the VM needs a Ctx, and a
// pipeline stage has no business owning one — so the manager asks the questions and the stages read
// the answers.
void OutputManager::update_values(SignalBus& bus, uint32_t now_ms) {
    extern EcuConfig g_config;
    const OutputsConfig& cfg = g_config.outputs;

    for (uint8_t i = 0; i < n_; i++) {
        const OutputConfig& o = cfg.output[slot_of_[i]];
        Pool& pool = pools_[i];

        expr::Ctx ectx;
        ectx.bus         = &bus;
        ectx.cfg         = reinterpret_cast<const uint8_t*>(&g_config);
        ectx.cfg_size    = sizeof(g_config);
        ectx.now_ms      = now_ms;
        ectx.table       = expr_table_value;
        ectx.curve       = expr_table_at;
        ectx.curve_user  = &bus;
        ectx.curve_count = EXPR_TABLE_COUNT;

        // DUTY, when the slot says its value is an expression. Read for its NUMBER, not its truth.
        pool.duty_ok = false;
        if (o.value_source == 3 && !expr::is_empty(o.duty_expr, sizeof(o.duty_expr))) {
            const expr::Result r = expr::exec(o.duty_expr, sizeof(o.duty_expr), ectx);
            pool.duty_ok  = r.ran && r.v.ok;
            pool.duty_val = pool.duty_ok ? r.v.value : 0.0f;
        }

        // FREQUENCY. Fixed is the build-time period and needs nothing here; a table or an expression
        // is a new period every frame. Clamped to the range the fixed setting allows, so neither can
        // ask the pulse engine for a carrier it cannot make (and a zero would divide by it).
        pool.pwm.live_period_ticks = 0;
        if (o.kind == 0 && o.freq_source != 0) {
            float hz = 0.0f;
            bool  ok = false;
            if (o.freq_source == 1) { ok = pool.freq_ok; if (ok) hz = tbl::table_eval(pool.freq, bus); }
            else if (!expr::is_empty(o.freq_expr, sizeof(o.freq_expr))) {
                const expr::Result r = expr::exec(o.freq_expr, sizeof(o.freq_expr), ectx);
                ok = r.ran && r.v.ok;
                hz = ok ? r.v.value : 0.0f;
            }
            if (ok && hz >= 1.0f) {
                if (hz > 50000.0f) hz = 50000.0f;
                pool.pwm.live_period_ticks = static_cast<uint32_t>(tps_ / hz);
            }
            // Unanswerable, or out of range: live_period_ticks stays 0 and the sink falls back to the
            // FIXED carrier. A duty with no carrier is not an output at all, so there is always one.
        }
    }
}

void OutputManager::recompute_live() {
    const reconfig::Time _t(reconfig::OUTPUTS);   // what this cost, for `cpu` to report
    // --- SAFE TEARDOWN (before any new allocation) ---------------------------------------------
    // 1) Release every SoftPwm channel WE own back to the shared pool FIRST (set_pin(nullptr) parks
    //    each): a stale waveform would re-drive a pin on the next pulse tick and fight the arbiter's
    //    Hi-Z.
    if (pwm_) pwm_->release_owner(SoftPwm::OWNER_OUTPUTS);
    // 2) Release + force Hi-Z every runtime-output pin (PinOwner::AUX) — covers digital pins too —
    //    so a row changed to None always leaves its pin in a defined safe state.
    if (arbiter_) arbiter_->release_owner(PinOwner::AUX);

    n_       = 0;
    conflict_ = false;
    const OutputsConfig& cfg = g_config.outputs;

    for (uint8_t i = 0; i < N; i++) {
        const OutputConfig& o = cfg.output[i];
        const auto fn = static_cast<OutputFunction>(o.function);
        if (fn != OutputFunction::GENERIC) continue;   // None, or the scheduler's

        // THE ROW'S OWN PIN. nullptr => the scheduler still holds it as a coil or injector (a row changed
        // away from firing waits for the next stopped reconfigure to be released). Trip P1650, flag the
        // conflict, and build NOTHING for this row — it can never drive a contested pin.
        ITimerChannel* sink = arbiter_ ? arbiter_->claim(i, PinOwner::AUX) : nullptr;
        if (!sink) {
            if (arbiter_ && i < arbiter_->size()) {
                conflict_ = true;
                if (dtc_) dtc_->raise(PCODE_PIN_CONFLICT, DtcSource::PIN_ARBITER, /*sev*/ 1,
                                      platform_get_tick_ms());
            }
            continue;
        }

        Pool& pool = pools_[n_];
        pipe::Pipeline& p = pipelines_[n_];
        p.signal = SIG_NONE;                  // outputs consume the bus; they don't publish
        p.n = 0;
        p.reset_state();

        // 1) ARBITRATE — o.cand is a CandSub[4] {sig, role}; pack the live ones (n_cand<=4).
        const uint8_t nc = (o.n_cand <= 4) ? o.n_cand : 4;
        for (uint8_t k = 0; k < nc; k++) pool.cand[k] = { o.cand[k].sig, o.cand[k].role };
        const float failsafe = static_cast<float>(o.failsafe_x10) / 10.0f;
        slot_of_[n_] = i;
        // A FRESH GATE for a rebuilt slot: its conditions may have just changed, and inheriting a
        // latch across a reconfigure would leave an output on for a rule that no longer exists.
        //
        // STAMPED WITH NOW, not left at zero. Every timing is measured from these two marks — so a
        // gate born at 0 while the clock already reads minutes believes it has been on since the ECU
        // booted. A slot with a maximum-on time then tripped it on its FIRST frame and went into its
        // re-arm lockout, which for a starter means the first press of the button after a rebuild is
        // refused.
        //
        // AND BORN OFF IF IT HAS A CONDITION TO ASK. "A gate starts on" is right for an output that
        // asks nothing — and update_gates() handles that case on its own, before it ever looks at the
        // state (`if (!has_on && !has_off) { g.on = 1; }`). For a slot that DOES have conditions, born
        // on means the output drives until something turns it off, and the only thing that can is the
        // off condition being true on the very first frame. That held the starter down only because
        // its off condition happened to name the button; the moment a starter latches its crank —
        // which is what makes a momentary press crank for ten seconds — neither condition is true at
        // rest and the first frame after every key-on and every Outputs write cranks the engine.
        //
        // So: an output does nothing until its own condition says to. The answer for a slot that asks
        // a question is not "yes" until it has been asked.
        gates_[n_] = OutputGate{};
        const bool asks = !expr::is_empty(o.on_expr,  sizeof(o.on_expr)) ||
                          !expr::is_empty(o.off_expr, sizeof(o.off_expr));
        if (asks) gates_[n_].on = 0;
        gates_[n_].since_ms = gates_[n_].on_since_ms = platform_get_tick_ms();
        const float off_value = static_cast<float>(o.clamp_lo_x10) / 10.0f;

        // WHERE THE VALUE COMES FROM — one of three stages, chosen once at build: the bus, this
        // slot's own duty map, or a constant. All three honour the same gate, so "when" and "how
        // much" stay separate questions with one answer each.
        if (o.value_source == 1) {                       // Table — a fan on coolant, a pump on demand
            // THE TABLE THIS SLOT NAMES, resolved once here rather than every frame: a table id is a
            // switch over descriptors, and the descriptor is stable for as long as the build is. A
            // slot pointing at nothing (unset, or an id this firmware has no table for) falls to the
            // FAILSAFE stage rather than reading a table that is not there.
            if (o.duty_table_sel >= 0 &&
                expr_table_desc(static_cast<uint16_t>(o.duty_table_sel), pool.duty)) {
                pool.src = { &pool.duty, 0.0f, &gates_[n_].on, off_value };
                p.stages[p.n++] = { pipe::value_from_table, &pool.src };
            } else {
                pool.src = { nullptr, failsafe, &gates_[n_].on, off_value };
                p.stages[p.n++] = { pipe::value_fixed, &pool.src };
            }
        } else if (o.value_source == 2) {                // Fixed — most relays, and many pumps
            pool.src = { nullptr, static_cast<float>(o.fixed_x10) / 10.0f, &gates_[n_].on, off_value };
            p.stages[p.n++] = { pipe::value_fixed, &pool.src };
        } else if (o.value_source == 3) {                // Expression — a duty that is arithmetic
            pool.src = { nullptr, 0.0f, &gates_[n_].on, off_value,
                         &pool.duty_val, &pool.duty_ok, failsafe };
            p.stages[p.n++] = { pipe::value_from_expr, &pool.src };
        } else {                                         // Candidates — what every slot did before
            pool.arb = { pool.cand, nc, o.primary_policy, failsafe, &gates_[n_].on, off_value };
            p.stages[p.n++] = { pipe::arbitrate_roled, &pool.arb };
        }

        // 2) ENCODE — scaled-int config -> float (packed floats would land unaligned -> M7 VLDR fault).
        pool.enc = { static_cast<float>(o.scale_x1000)  / 1000.0f,
                     static_cast<float>(o.offset_x10)    / 10.0f,
                     static_cast<float>(o.clamp_lo_x10)  / 10.0f,
                     static_cast<float>(o.clamp_hi_x10)  / 10.0f };
        p.stages[p.n++] = { pipe::encode_linear, &pool.enc };

        // 3) EMIT — bind the hardware sink at the edge; the pointer rides anonymously in the cfg.
        if (o.kind == 0) {                                // PWM via SoftPwm
            const int ch = pwm_ ? pwm_->claim(SoftPwm::OWNER_OUTPUTS) : -1;
            if (ch < 0) continue;                          // pool full: pin stays claimed+Hi-Z (safe)
            pwm_->set_pin(ch, sink, o.active_high != 0);
            pool.pwm  = { pwm_, ch, o.pwm_freq_hz ? (tps_ / o.pwm_freq_hz) : 0u, 0u };
            // The carrier's table, by the same id space. An unresolvable one leaves pool.freq_ok
            // false, and update_values() then leaves the sink on its FIXED period — a duty with no
            // carrier is not an output at all, so there is always one.
            pool.freq_ok = o.freq_source == 1 && o.freq_table_sel >= 0 &&
                           expr_table_desc(static_cast<uint16_t>(o.freq_table_sel), pool.freq);
            pool.emit = { pipe::pwm_emit_sink, &pool.pwm, failsafe, &pool.last_cmd };
        } else {                                          // digital GPIO
            sink->enable_output(o.active_high ? OutputAction::DRIVE_LOW : OutputAction::DRIVE_HIGH);
            // No dedicated threshold field: switch at the midpoint of the configured clamp range, so
            // it adapts to the encode span (e.g. [0,100] -> 50, [0,1] -> 0.5).
            const float thr = 0.5f * (pool.enc.lo + pool.enc.hi);
            pool.dig  = { sink, o.active_high != 0, thr };
            pool.emit = { pipe::digital_emit_sink, &pool.dig, failsafe, &pool.last_cmd };
        }
        p.stages[p.n++] = { pipe::emit_sink, &pool.emit };
        n_++;
    }
}
