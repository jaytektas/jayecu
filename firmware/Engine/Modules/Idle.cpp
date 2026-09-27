#include "Idle.h"
#include "well_known_signals.h"                 // wk:: roles — rename-safe (tps/clt)
#include "signal_enums.h"                      // IdleState — the published number IS the schema's value
#include "../EngineStateMachine.h"          // EngineRunState
#include "../TableEval.h"                   // tbl::table_eval
#include "../../../generated/table_descs.h" // <table>_desc(cfg)
#include "../../Platform/platform_hal.h"    // platform_get_tick_ms + platform_learned_block
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../../generated/learned_layout.h"  // LEARNED_IDLE_LTT_* (fixed offset + magic + dims)
#include <algorithm>
#include <cmath>

static_assert(LEARNED_IDLE_LTT_CELLS == IDLE_LTT_CLT_AXIS_ALLOC, "learned_layout Idle LTT dims != module");

// Nearest CLT breakpoint — learning goes into a discrete cell, not a blend.
//
// This was a linear search over the axis, which is the same question VvtControl, Lambda and Knock each
// answered their own way. It reads the same axis it always did (idle_ltt's x axis IS ltt_clt_axis); the
// only change is that the answer comes from the one bin lookup, so the four cannot drift apart again.
int Idle::ltt_cell_index(float clt) const {
    if (!cfg_) return 0;
    return tbl::nearest_bin(idle_ltt_desc(cfg_).x, clt);
}

void Idle::reset_state() {
    pi_.reset();
    last_err_ = 0.0f;
    for (auto& s : idle_up_) { s.frac = 0.0f; s.active_since_ms = 0; }
    follower_applied_ = 0.0f;
    ltt_dwell_cell_ = 0xFF;   // restart dwell tracking (learned values persist in NV)
    slew_valid_ = false;      // re-snap the target slew on the next run
    decel_applied_ = 0.0f;
    throttle_closed_prev_ = true;
}

void Idle::init(const IdleConfig& cfg) {
    cfg_ = &cfg;
    last_ms_ = 0;
    reset_state();

    // Map the LTT table onto its FIXED slice of the learned region (offset from generated/learned_layout.h).
    // The region is a live RAM buffer, restored from the SD totem before init, so just map it: 0 = neutral
    // (re-learn), non-zero = restored trim. Mapped once (init runs once in compose).
    if (!ltt_) {
        constexpr unsigned N = IDLE_LTT_CLT_AXIS_ALLOC;
        const uint32_t need = N * sizeof(float);
        auto* base = platform_learned_block(LEARNED_IDLE_LTT_OFFSET, need);
        if (base) {
            ltt_ = reinterpret_cast<float*>(base);   // live RAM slice; boots neutral, restored from SD totem
        } else {
            static float fallback[N] = {};   // region wouldn't fit -> RAM, re-learn each boot
            ltt_ = fallback;
        }
    }
}
void Idle::on_config_change(const IdleConfig& cfg) { cfg_ = &cfg; }

void Idle::update(const EnginePosition& pos, SignalBus& bus, EngineFrame& /*frame*/) {
    const uint32_t now = platform_get_tick_ms();
    if (!cfg_ || !cfg_->enabled) {
        bus.set(wk::idle_ign_corr, 0.0f, true, now, ttl());  // additive to spark — must not linger stale when disabled
        bus.set(wk::idle_follower, 0.0f, true, now, ttl());
        bus.set(wk::idle_ltt_pct, 0.0f, true, now, ttl());
        bus.set(wk::idle_state, static_cast<float>(IdleState::OFF), true, now, ttl());
        return;                            // (idle_duty: publish nothing, so the output failsafes)
    }

    const float dt_s = (last_ms_ != 0) ? (now - last_ms_) / 1000.0f : 0.0f;
    last_ms_ = now;

    const auto state = static_cast<EngineRunState>(static_cast<int>(bus.get(wk::engine_state, 0.0f)));
    // THE DRIVER'S THROTTLE, not the plate. On drive-by-wire the plate is held open by the idle floor and
    // the ETB's own minimum (defaults 3 %), so "plate below 2 %" was never true: idle_active stayed false
    // and closed-loop idle, its learning and idle_ign_corr never ran on any DBW car. The pedal is what says
    // the driver has lifted; the plate stands in only on a cable throttle, where they are the same thing.
    const float tps  = bus.valid(SIG_PEDAL_DEMAND) ? bus.get(SIG_PEDAL_DEMAND, 0.0f) : bus.get(wk::tps, 0.0f);
    const float clt  = bus.get(wk::clt, 20.0f);   // for the LTT learned-cell index
    const float rpm  = pos.rpm;

    // Idle condition: running, throttle closed, below the off-idle (lockout) RPM. Gates closed-loop
    // learning + published for telemetry. The OPEN-LOOP base position below applies whenever enabled.
    const bool vss_ok = !cfg_->vss_check_enabled
                      || bus.get(wk::vehicle_spd, 0.0f) <= static_cast<float>(cfg_->max_vehicle_speed);
    const bool idle_active = (state == EngineRunState::RUNNING)
                          && (tps < static_cast<float>(cfg_->tps_closed_pct) * 0.1f)
                          && (rpm < static_cast<float>(cfg_->idle_lockout_rpm))
                          && vss_ok;                       // not "idling" while the vehicle is moving
    bus.set_bool(wk::idle_active, idle_active, now, ttl());

    // Engine stopped -> drop the closed-loop history so a restart begins clean (the start offsets,
    // not a stale integrator, supply the cold-start kick).
    if (state == EngineRunState::STOPPED) reset_state();

    // --- Idle Up: accessory-load compensation. Each generic slot, while its bound bus signal is
    // active (> threshold), ramps an offset in (after on_delay_ms) / out (over decay_ms) and adds
    // rpm_offset to the target + base_offset to the base %duty. Config-agnostic — input_sig binds
    // to any signal the user mapped (A/C switch, power steering, fan, ...). ---
    float rpm_up = 0.0f, base_up = 0.0f;
    for (unsigned i = 0; i < IDLE_IDLE_UP_COUNT; i++) {
        const IdleUpConfig& s = cfg_->idle_up[i];
        IdleUpState& st = idle_up_[i];
        if (!s.enabled) { st.frac = 0.0f; st.active_since_ms = 0; continue; }
        const SignalId sig = static_cast<SignalId>(s.input_sig);
        const bool active = bus.valid(sig) && bus.get(sig, 0.0f) > (s.threshold_x10 * 0.1f);
        if (active) {
            if (st.active_since_ms == 0) st.active_since_ms = now ? now : 1;     // first active edge
            if ((now - st.active_since_ms) >= s.on_delay_ms) st.frac = 1.0f;     // engage after on-delay
        } else {
            st.active_since_ms = 0;
            st.frac = (s.decay_ms > 0 && dt_s > 0.0f)
                    ? std::max(0.0f, st.frac - dt_s * 1000.0f / static_cast<float>(s.decay_ms))
                    : 0.0f;                                                       // decay out / instant off
        }
        rpm_up  += st.frac * static_cast<float>(s.rpm_offset);
        base_up += st.frac * (s.base_offset_x10 * 0.1f);
    }

    // --- Throttle follower (dashpot): a feedforward air % vs RPM x TPS that rises instantly with the
    // throttle and, on tip-out, bleeds back down at the decay rate (%/s vs RPM) instead of snapping
    // shut — bridging the over-run so the engine doesn't stall. Runs regardless of idle_active. ---
    if (cfg_->throttle_follower_enabled) {
        const float raw = tbl::table_eval(throttle_follower_target_table_desc(cfg_), bus);
        if (raw >= follower_applied_ || dt_s <= 0.0f) {
            follower_applied_ = raw;                                              // follow the throttle up
        } else {
            const float rate = tbl::table_eval(throttle_follower_decay_table_desc(cfg_), bus);  // %/s
            follower_applied_ = std::max(raw, follower_applied_ - rate * dt_s);   // bleed down on tip-out
        }
    } else {
        follower_applied_ = 0.0f;
    }
    bus.set(wk::idle_follower, follower_applied_, true, now, ttl());

    // --- Targets + demands from the tables (channel-driven; 2nd axes default off -> 1D X curves) ---
    const float raw_target = tbl::table_eval(target_rpm_table_desc(cfg_), bus)
                           + tbl::table_eval(start_target_offset_table_desc(cfg_), bus)
                           + rpm_up;                                              // idle-up RPM bump
    // Rate-limit the target so a step (warmup decay, idle-up, mode change) doesn't make the PI lurch.
    if (!slew_valid_ || dt_s <= 0.0f) {
        slewed_target_ = raw_target; slew_valid_ = true;
    } else {
        const float up = cfg_->rpm_rate_rising_limit  ? cfg_->rpm_rate_rising_limit  * dt_s : 1e30f;
        const float dn = cfg_->rpm_rate_falling_limit ? cfg_->rpm_rate_falling_limit * dt_s : 1e30f;
        slewed_target_ = std::clamp(raw_target, slewed_target_ - dn, slewed_target_ + up);
    }
    const float target = slewed_target_;
    bus.set(wk::idle_target_rpm, target, true, now, ttl());

    // Long-term trim: the learned per-CLT correction applied to the base air (read here; learnt below).
    const int   ltt_cell  = (cfg_->ltt_enabled && ltt_) ? ltt_cell_index(clt) : -1;
    const float ltt_apply = (ltt_cell >= 0) ? ltt_[ltt_cell] : 0.0f;
    bus.set(wk::idle_ltt_pct, ltt_apply, true, now, ttl());

    // Decel offset: a fixed air kick latched on a throttle tip-out (closed-throttle falling edge),
    // bleeding to 0 over decel_decay_ms — catches the over-run (sums with the throttle follower).
    const bool throttle_closed = tps < static_cast<float>(cfg_->tps_closed_pct) * 0.1f;
    if (throttle_closed && !throttle_closed_prev_) {
        decel_applied_ = static_cast<float>(cfg_->decel_offset_pct) * 0.1f;
    } else if (cfg_->decel_decay_ms > 0 && dt_s > 0.0f) {
        decel_applied_ = std::max(0.0f, decel_applied_
            - static_cast<float>(cfg_->decel_offset_pct) * 0.1f * dt_s * 1000.0f / static_cast<float>(cfg_->decel_decay_ms));
    } else {
        decel_applied_ = 0.0f;
    }
    throttle_closed_prev_ = throttle_closed;

    const float base_eff = tbl::table_eval(base_duty_table_desc(cfg_), bus)
                         + tbl::table_eval(start_base_offset_table_desc(cfg_), bus)
                         + base_up                                                // idle-up base bump
                         + follower_applied_                                      // throttle-follower dashpot
                         + ltt_apply                                              // learned long-term trim
                         + decel_applied_;                                        // decel tip-out kick
    // A FLOOR ABOVE THE CEILING IS A TUNE, not an impossibility — Min Output and Max Duty both run to
    // 100% and nothing relates them. std::clamp(v, lo, hi) is UNDEFINED when lo > hi, and the duty
    // clamp at the bottom of this function fed it these two straight: on this libstdc++ it returned
    // the floor, so an idle valve was driven ABOVE the ceiling that exists to protect it. Settle the
    // pair once, here, and the ceiling wins — it is the limit of the actuator, where the floor is a
    // preference about air. The PI's authority then falls out of an ordered pair instead of needing a
    // second guard of its own further down.
    const float max_out  = static_cast<float>(cfg_->max_duty_pct) * 0.1f;
    const float min_out  = std::min(tbl::table_eval(min_output_table_desc(cfg_), bus), max_out);  // % floor

    // Publish the error BEFORE evaluating the gain tables — they read it back as their X axis channel.
    const float error = target - rpm;
    bus.set(wk::idle_rpm_error, error, true, now, ttl());

    // --- Gain-scheduled PI(+D) trim ---
    // Closed loop engages in Closed mode, while idling, once RPM has fallen to within the activation
    // offset of target (a throttle blip free-falls before the loop grabs it). P/I/D table gains are in
    // % per 100-RPM(/s) units -> /100 to per-RPM; PID Scaler trims them globally.
    const bool closed = (cfg_->mode != 0) && idle_active && dt_s > 0.0f
                     && (rpm <= target + static_cast<float>(cfg_->cl_activation_offset_rpm));
    const float scaler = static_cast<float>(cfg_->pid_scaler_pct) * 0.1f / 100.0f;

    // Bound the trim to the headroom between the floor and the ceiling, so the integrator's anti-windup
    // is exactly the final clamp. min_out <= max_out by construction above, so the pair is ordered.
    pi_.out_min = min_out - base_eff;
    pi_.out_max = max_out - base_eff;

    float trim = pi_.integ;                       // frozen when not engaged (no windup off-idle)
    float d_term = 0.0f;
    if (closed) {
        // THE GAINS ARE READ WHERE THEY ARE USED. P and I were evaluated every frame and handed to a
        // controller that only steps when the loop is closed, so in open loop two table lookups
        // produced numbers nothing could act on — and, worse, the READ being unconditional is what a
        // gate audit goes on, so the three gain pages passed as live while the mode they belong to was
        // off. Evaluating them here costs open loop nothing and lets the audit see what is true.
        pi_.kp = tbl::table_eval(p_gain_table_desc(cfg_), bus) / 100.0f * scaler;
        pi_.ki = tbl::table_eval(i_gain_table_desc(cfg_), bus) / 100.0f * scaler;
        trim = pi_.step(error, dt_s);
        const float kd = tbl::table_eval(d_gain_table_desc(cfg_), bus) / 100.0f * scaler;
        d_term = kd * (error - last_err_) / dt_s; // % per (100 RPM/s)
    }
    last_err_ = error;

    // WHAT IT IS DOING, in one channel. `idle_active` is the CONDITION and says none of this: it is
    // true in open loop, true while the closed loop stands off waiting for a blip to fall, and true
    // when the PI is working. Those are three different situations and only the last one makes the gain
    // tables mean anything — so a page showing the flag alone cannot tell a tuner whether the numbers
    // they are editing are being used. Published here because this is the first point at which `closed`
    // exists; every branch above leads to one of the states below.
    const IdleState st = (state != EngineRunState::RUNNING) ? IdleState::NOT_RUNNING
                       : !idle_active                       ? IdleState::OFF_IDLE
                       : (cfg_->mode == 0)                  ? IdleState::OPEN_LOOP
                       : closed                             ? IdleState::CLOSED_LOOP
                                                            : IdleState::WAITING;
    bus.set(wk::idle_state, static_cast<float>(st), true, now, ttl());

    float duty = std::clamp(base_eff + trim + d_term, min_out, max_out);
    // Stall save: if a running engine drops below the stall floor, dump full air to catch it.
    if (cfg_->stall_offset_rpm > 0 && state == EngineRunState::RUNNING
        && rpm < static_cast<float>(cfg_->stall_offset_rpm)) {
        duty = max_out;
    }
    // Close-on-boost: a leaking idle bypass is pointless under boost — shut the valve. Last word.
    if (cfg_->close_on_boost_enabled && bus.get(wk::map, 0.0f) > static_cast<float>(cfg_->close_on_boost_kpa)) {
        duty = 0.0f;
    }
    bus.set(wk::idle_duty, duty, true, now, ttl());

    // Long-term-trim learn: while closed-loop + warmed up + dwelling in a CLT cell, migrate a fraction
    // of the steady PI trim into the learned cell (clamped to authority) and recenter the integrator,
    // so base+LTT carries the persistent correction and the PI works from ~0 next start (like LTFT).
    if (ltt_cell >= 0 && closed && bus.get(wk::run_time, 0.0f) >= static_cast<float>(cfg_->ltt_min_runtime_s)) {
        if (ltt_cell != ltt_dwell_cell_) { ltt_dwell_cell_ = static_cast<uint8_t>(ltt_cell); ltt_dwell_since_ms_ = now; }
        else if ((now - ltt_dwell_since_ms_) >= cfg_->ltt_dwell_ms) {
            const float move = (cfg_->ltt_learn_pct * 0.1f / 100.0f) * pi_.integ;       // %duty migrated
            const float lim  = static_cast<float>(cfg_->ltt_authority_pct) * 0.1f;
            ltt_[ltt_cell] = std::clamp(ltt_[ltt_cell] + move, -lim, lim);
            pi_.bleed(move);                                                      // recenter the PI trim
            ltt_dwell_since_ms_ = now;
        }
    } else {
        ltt_dwell_cell_ = 0xFF;
    }

    // Idle ignition correction: a spark trim vs the same RPM error, added to advance by
    // Ignition. Only while idling — 0 off-idle so it never yanks timing under load.
    bus.set(wk::idle_ign_corr, idle_active ? tbl::table_eval(idle_ign_corr_table_desc(cfg_), bus) : 0.0f, true, now, ttl());
}
