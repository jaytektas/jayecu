#include "CruiseControl.h"
#include "well_known_signals.h"
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../Platform/platform_hal.h"
#include "../../Signal/Expr.h"                 // expr::exec — every button is a condition
#include "../TableEval.h"                      // tbl::table_eval — the gains are curves
#include "../../../generated/table_descs.h"
#include "../../../generated/ecu_config.h"     // g_config (live tune RAM)
#include "../../../generated/table_registry.h" // expr_table_* — the tables a condition may read
#include "../../../generated/signal_ids.h"
#include "../../../generated/module_dtc.h"
#include "../../../generated/sensors_catalog.h"  // SENSOR_CATALOG — which sensor publishes a signal
#include <algorithm>

extern volatile uint32_t g_config_generation;  // bumped by every config write (CommsManager.cpp)

namespace {
// The eight programs, in Btn order. One place that knows which field is which.
struct Prog { const uint8_t* p; uint16_t len; };
Prog prog_of(const CruiseControlConfig& c, int k) {
    switch (k) {
        case CruiseControl::BTN_ENABLE:  return { c.enable_expr,         (uint16_t)sizeof c.enable_expr };
        case CruiseControl::BTN_DISABLE: return { c.disable_expr,        (uint16_t)sizeof c.disable_expr };
        case CruiseControl::BTN_TOGGLE:  return { c.enable_disable_expr, (uint16_t)sizeof c.enable_disable_expr };
        case CruiseControl::BTN_SET:     return { c.set_expr,            (uint16_t)sizeof c.set_expr };
        case CruiseControl::BTN_RESUME:  return { c.resume_expr,         (uint16_t)sizeof c.resume_expr };
        case CruiseControl::BTN_CANCEL:  return { c.cancel_expr,         (uint16_t)sizeof c.cancel_expr };
        case CruiseControl::BTN_UP:      return { c.bump_up_expr,        (uint16_t)sizeof c.bump_up_expr };
        default:                         return { c.bump_down_expr,      (uint16_t)sizeof c.bump_down_expr };
    }
}

// A cancel input is read fail-SAFE, the opposite of an expression: a brake switch that has stopped
// reporting must never be mistaken for a foot off the brake. Unassigned (-1) is simply not fitted.
bool pressed_failsafe(const SignalBus& bus, int16_t sig) {
    if (sig < 0) return false;
    const SignalId s = static_cast<SignalId>(sig);
    return !bus.valid(s) || bus.get_bool(s);
}

// IS ANYTHING WATCHING THIS INPUT? The fail-safe rule above rests entirely on a faulty sensor going
// INVALID, and a sensor only goes invalid when one of its own checks trips — but diag_enable ships as
// 0, so out of the box nothing is checked. A brake switch that is stuck, shorted or unplugged then
// publishes a perfectly valid "not braking" for ever and the interlock never fires. So cruise asks
// whether its own safety inputs are monitored, and refuses to arm when they are not.
// Returns false only when the signal IS a sensor and that sensor has no checks armed.
bool sensor_is_watched(int16_t sig) {
    if (sig < 0) return true;                                  // not fitted is not unmonitored
    for (unsigned i = 0; i < SENSOR_COUNT; ++i) {
        if (SENSOR_CATALOG[i].primary_channel != static_cast<uint16_t>(sig)) continue;
        return g_config.sensors.sensor[i].diag_enable != 0;
    }
    return true;                                               // not a catalogue sensor (CAN, Lua, derived)
}
}  // namespace

void CruiseControl::reset() {
    state_      = CruiseState::OFF;
    target_kph_ = 0.0f;
    inhibit_    = 0;
    last_ms_    = 0;
    pi_.reset();
    last_err_ = 0.0f; ramped_ = 0.0f; ramp_valid_ = false; decay_rate_ = 0.0f;
    runaway_ms_ = 0; sw_bad_ms_ = 0; last_speed_ = 0.0f; have_last_speed_ = false;
    latched_fault_ = 0;
    for (int k = 0; k < BTN_COUNT; ++k) { prev_[k] = false; press_ms_[k] = 0; long_fired_[k] = false; }
}

void CruiseControl::revalidate() {
    for (int k = 0; k < BTN_COUNT; ++k) {
        const Prog pr = prog_of(*cfg_, k);
        expr_bad_[k] = !expr::is_empty(pr.p, pr.len) &&
                       expr::validate(pr.p, pr.len, sizeof(g_config), EXPR_TABLE_COUNT)
                           != expr::Invalid::None;
    }
}

void CruiseControl::raise_codes(uint32_t now) {
    if (!dtc_) return;
    // One table, so a bit and its code can never drift apart. All level 1: the right answer to any of
    // these is to drop cruise, which this module does itself — none of them justifies a higher level's
    // engine protection reaction.
    struct Row { uint32_t bit; uint16_t code; uint8_t sev; };
    static const Row kRows[] = {
        { INH_VSS_INVALID | INH_VSS_STALE, ModuleDtc::CRUISE_VSS,         ModuleDtc::CRUISE_VSS_SEV },
        { INH_VSS_IMPL,                    ModuleDtc::CRUISE_VSS_IMPL,    ModuleDtc::CRUISE_VSS_IMPL_SEV },
        { INH_SW_INVALID,                  ModuleDtc::CRUISE_SW,          ModuleDtc::CRUISE_SW_SEV },
        { INH_RUNAWAY,                     ModuleDtc::CRUISE_RUNAWAY,     ModuleDtc::CRUISE_RUNAWAY_SEV },
        { INH_WHEEL_DIFF,                  ModuleDtc::CRUISE_WHEEL_DIFF,  ModuleDtc::CRUISE_WHEEL_DIFF_SEV },
        { INH_PEDAL_FAULT,                 ModuleDtc::CRUISE_PEDAL,       ModuleDtc::CRUISE_PEDAL_SEV },
        { INH_EXPR_BAD,                    ModuleDtc::CRUISE_EXPR,        ModuleDtc::CRUISE_EXPR_SEV },
        { INH_UNMONITORED,                 ModuleDtc::CRUISE_UNMONITORED, ModuleDtc::CRUISE_UNMONITORED_SEV },
        { INH_NO_BRAKE,                    ModuleDtc::CRUISE_NO_BRAKE,    ModuleDtc::CRUISE_NO_BRAKE_SEV },
    };
    for (const Row& r : kRows) {
        if (inhibit_ & r.bit) dtc_->raise(r.code, DtcSource::MODULE, r.sev, now, dtc_ttl());
        else                  dtc_->heal(r.code);
    }
}

void CruiseControl::heal_all() {
    if (!dtc_) return;
    dtc_->heal(ModuleDtc::CRUISE_VSS);         dtc_->heal(ModuleDtc::CRUISE_VSS_IMPL);
    dtc_->heal(ModuleDtc::CRUISE_SW);
    dtc_->heal(ModuleDtc::CRUISE_RUNAWAY);     dtc_->heal(ModuleDtc::CRUISE_WHEEL_DIFF);
    dtc_->heal(ModuleDtc::CRUISE_PEDAL);       dtc_->heal(ModuleDtc::CRUISE_EXPR);
    dtc_->heal(ModuleDtc::CRUISE_UNMONITORED); dtc_->heal(ModuleDtc::CRUISE_NO_BRAKE);
}

void CruiseControl::update(const EnginePosition& /*pos*/, SignalBus& bus, EngineFrame& /*frame*/) {
    const uint32_t now  = platform_get_tick_ms();
    float dt_s = (last_ms_ != 0) ? static_cast<float>(now - last_ms_) * 0.001f : 0.0f;
    if (dt_s > 0.1f) dt_s = 0.0f;          // a scheduler stall is not a hundred milliseconds of integral
    last_ms_ = now;

    auto publish = [&](float demand) {
        bus.set(SIG_CRUISE_STATE, static_cast<float>(state_), true, now, ttl());
        bus.set_bool(SIG_CRUISE_ACTIVE, state_ == CruiseState::CRUISING, now, ttl());
        bus.set(SIG_CRUISE_DEMAND, demand, true, now, ttl());
        bus.set(SIG_CRUISE_TARGET, target_kph_, true, now, ttl());
        bus.set_u32(SIG_CRUISE_INHIBIT, inhibit_, true, now, ttl());
    };

    // KEY OFF IS CRUISE OFF. With the key off the sensors publish nothing, so road speed and the stalk
    // read invalid — and judged as faults, they latched FAULT (for a Ready power-on state), which the
    // key-on then inherited: a stored VSS code every key cycle, and cruise refusing to engage until a
    // button was pressed to acknowledge a fault that was only the key. Key-on is this module's power-on,
    // so key-off resets it exactly as switching it off does, and it wakes in its power-on state.
    extern bool g_system_active;                       // key-on: owned by Sensors
    if (!cfg_ || !cfg_->enabled || !g_system_active) {
        state_ = CruiseState::OFF;
        target_kph_ = 0.0f; inhibit_ = 0; pi_.reset();
        ramped_ = 0.0f; ramp_valid_ = false; decay_rate_ = 0.0f; last_err_ = 0.0f;
        runaway_ms_ = 0; sw_bad_ms_ = 0; have_last_speed_ = false; latched_fault_ = 0;
        for (int k = 0; k < BTN_COUNT; ++k) { prev_[k] = false; long_fired_[k] = false; }
        // This return would skip the heal in the enabled path, so a code raised while we were running
        // would stay ACTIVE for ever — see DtcManager::heal. Heal on the edge.
        if (was_enabled_) heal_all();
        was_enabled_ = false;
        bus.set(SIG_CRUISE_ERROR_KPH, 0.0f, true, now, ttl());
        publish(0.0f);
        return;
    }

    // --- the tune may have changed under us -------------------------------------------------------
    // exec() assumes validate() has passed, and on_config_change is not wired in this firmware, so the
    // module watches the generation counter itself (the ScriptEngine/Sensors pattern).
    was_enabled_ = true;
    if (g_config_generation != cfg_gen_seen_) { cfg_gen_seen_ = g_config_generation; revalidate(); }

    if (state_ == CruiseState::OFF) state_ = (cfg_->power_on_state == 1) ? CruiseState::READY
                                                                        : CruiseState::DISABLED;

    // --- read the conditions ----------------------------------------------------------------------
    expr::Ctx ex;
    ex.bus = &bus; ex.cfg = reinterpret_cast<const uint8_t*>(&g_config);
    ex.cfg_size = sizeof(g_config); ex.now_ms = now;
    // The tune's own tables, readable from a condition. The bus rides in the user pointer because a
    // table's axes ARE bus channels, which keeps the VM kernel clear of the table engine.
    ex.table = expr_table_value; ex.curve = expr_table_at;
    ex.curve_user = &bus; ex.curve_count = EXPR_TABLE_COUNT;

    bool tap[BTN_COUNT] = {}, held[BTN_COUNT] = {};
    bool any_bad = false;
    for (int k = 0; k < BTN_COUNT; ++k) {
        const Prog pr = prog_of(*cfg_, k);
        bool level = false;
        if (expr_bad_[k]) {
            any_bad = true;                       // a gate that cannot be answered is NOT "condition false"
        } else if (!expr::is_empty(pr.p, pr.len)) {
            level = expr::exec(pr.p, pr.len, ex).truthy();   // untrustworthy inputs fail to FALSE here
        }
        if (level && !prev_[k]) { press_ms_[k] = now; long_fired_[k] = false; }
        // A HOLD is a level, because the action it drives integrates dt. A TAP is reported on RELEASE,
        // and only if the hold never fired — which is the whole reason one press cannot be both.
        if (level && cfg_->long_press_ms != 0 &&
            static_cast<int32_t>(now - (press_ms_[k] + cfg_->long_press_ms)) >= 0) {
            held[k] = true; long_fired_[k] = true;
        }
        tap[k]  = !level && prev_[k] && !long_fired_[k];
        prev_[k] = level;
    }

    // --- what would stop it engaging --------------------------------------------------------------
    const float speed   = bus.get(wk::vehicle_spd, 0.0f);
    const float min_kph = static_cast<float>(cfg_->min_speed_kph) * 0.1f;
    const float max_kph = static_cast<float>(cfg_->max_speed_kph) * 0.1f;

    inhibit_ = 0;
    if (pressed_failsafe(bus, cfg_->brake_sig))     inhibit_ |= INH_BRAKE;
    if (pressed_failsafe(bus, cfg_->clutch_sig))    inhibit_ |= INH_CLUTCH;
    if (pressed_failsafe(bus, cfg_->handbrake_sig)) inhibit_ |= INH_HANDBRAKE;
    if (speed < min_kph)                            inhibit_ |= INH_SPEED_LOW;
    if (speed > max_kph)                            inhibit_ |= INH_SPEED_HIGH;
    if (any_bad)                                    inhibit_ |= INH_EXPR_BAD;

    // --- is the system even allowed to exist as configured? ---------------------------------------
    if (cfg_->brake_sig < 0) inhibit_ |= INH_NO_BRAKE;         // one way out, and it is a button
    if (!sensor_is_watched(cfg_->brake_sig) || !sensor_is_watched(cfg_->clutch_sig) ||
        !sensor_is_watched(cfg_->handbrake_sig)) inhibit_ |= INH_UNMONITORED;

    // --- can the road speed be believed? ----------------------------------------------------------
    // The old module read vehicle_spd with a fallback of 0 and no validity check at all, so a dead
    // sensor read as "stopped", fell under the minimum, and cruise simply never engaged — silently,
    // with nothing anywhere saying why.
    if (!bus.valid(wk::vehicle_spd)) {
        inhibit_ |= INH_VSS_INVALID;
    } else if (cfg_->spd_max_age_ms != 0 && bus.age_ms(wk::vehicle_spd, now) > cfg_->spd_max_age_ms) {
        inhibit_ |= INH_VSS_STALE;                             // stopped updating is not a speed of zero
    } else if (cfg_->max_accel_kph_s != 0 && have_last_speed_ && dt_s > 0.0f) {
        const float rate = (speed - last_speed_) / dt_s;
        if ((rate < 0.0f ? -rate : rate) > static_cast<float>(cfg_->max_accel_kph_s) * 0.1f)
            inhibit_ |= INH_VSS_IMPL;                          // no car changes speed that fast
    }
    last_speed_ = speed; have_last_speed_ = true;

    // Wheel-speed plausibility, but ONLY with all four fitted and reading. Fewer than four skips the
    // check rather than failing it, or a car with one sensor is held off cruise for ever.
    if (cfg_->max_wheel_diff_kph != 0) {
        const SignalId corners[4] = { SIG_WHEEL_FL, SIG_WHEEL_FR, SIG_WHEEL_RL, SIG_WHEEL_RR };
        bool all = true; float lo = 0.0f, hi = 0.0f;
        for (int i = 0; i < 4; ++i) {
            if (!bus.valid(corners[i])) { all = false; break; }
            const float v = bus.get(corners[i], 0.0f);
            if (i == 0 || v < lo) lo = v;
            if (i == 0 || v > hi) hi = v;
        }
        if (all && (hi - lo) > static_cast<float>(cfg_->max_wheel_diff_kph) * 0.1f)
            inhibit_ |= INH_WHEEL_DIFF;
    }

    // --- can the stalk be believed? ---------------------------------------------------------------
    // An invalid cruise_sw already makes every condition fail to false, which is safe but silent: it
    // looks exactly like a driver pressing nothing. This is what tells the two apart.
    // ONLY A STALK THAT IS FITTED CAN BE FAULTY. The conditions may read anything — separate buttons,
    // CAN flags, Lua — and a car with no cruise stalk has no cruise_sw at all. Counting "not valid" as a
    // switch fault there put cruise into Fault 300 ms after it was switched on, for want of a sensor
    // nobody had asked it to read. The check stands for the multi-position stalk sensor when it is
    // enabled, which is the case the band decoder can actually get wrong.
    bool stalk_fitted = false;
    for (unsigned i = 0; i < SENSOR_COUNT; ++i)
        if (SENSOR_CATALOG[i].primary_channel == static_cast<uint16_t>(SIG_CRUISE_SW)) {
            stalk_fitted = g_config.sensors.sensor[i].enabled != 0; break;
        }
    const bool sw_bad = stalk_fitted && !bus.valid(SIG_CRUISE_SW);
    sw_bad_ms_ = sw_bad ? (sw_bad_ms_ + static_cast<uint32_t>(dt_s * 1000.0f)) : 0;
    if (sw_bad && cfg_->sw_fault_ms != 0 && sw_bad_ms_ >= cfg_->sw_fault_ms) inhibit_ |= INH_SW_INVALID;

    // --- driveline ---------------------------------------------------------------------------------
    if (cfg_->min_rpm != 0 || cfg_->max_rpm != 0) {
        const float rpm = bus.get(SIG_RPM, 0.0f);
        if (cfg_->min_rpm != 0 && rpm < static_cast<float>(cfg_->min_rpm)) inhibit_ |= INH_RPM_LOW;
        if (cfg_->max_rpm != 0 && rpm > static_cast<float>(cfg_->max_rpm)) inhibit_ |= INH_RPM_HIGH;
    }
    if (cfg_->gear_check_enabled) {
        // Gear 0 is "neutral OR not known" and the two are treated the same on purpose: holding a road
        // speed with the box out of gear is holding an ENGINE speed, which is not what was asked for.
        const int gear = bus.valid(SIG_GEAR) ? static_cast<int>(bus.get(SIG_GEAR, 0.0f)) : 0;
        if (gear < static_cast<int>(cfg_->min_gear)) inhibit_ |= INH_GEAR;
    }
    if (bus.valid(SIG_APP_STATE) && bus.get(SIG_APP_STATE, 0.0f) >= 2.0f) inhibit_ |= INH_PEDAL_FAULT;

    // --- has the loop lost control? ---------------------------------------------------------------
    // A magnitude on its own false-trips on every hill crest, so the error has to persist. The
    // accumulator resets the instant it comes back inside the band, which is the half that matters.
    if (state_ == CruiseState::CRUISING && cfg_->max_error_kph != 0) {
        const float err = target_kph_ - speed;
        const float mag = err < 0.0f ? -err : err;
        if (mag > static_cast<float>(cfg_->max_error_kph) * 0.1f) {
            runaway_ms_ += static_cast<uint32_t>(dt_s * 1000.0f);
            if (runaway_ms_ >= cfg_->max_error_ms) inhibit_ |= INH_RUNAWAY;
        } else {
            runaway_ms_ = 0;
        }
    } else {
        runaway_ms_ = 0;
    }

    // --- the state machine ------------------------------------------------------------------------
    // The state as this frame FOUND it. A button may legitimately appear in several conditions — the
    // same stalk position in Set When and Speed Down When is how a Set/Coast button is described — so
    // what a press means has to be decided against the state it was pressed IN, not the state its own
    // first meaning just moved us to. Without this, one tap on such a button would engage cruise and
    // immediately coast the set speed back down by an increment.
    const CruiseState entry = state_;
    const bool want_disable = tap[BTN_DISABLE] || (tap[BTN_TOGGLE] && state_ != CruiseState::DISABLED);
    const bool want_enable  = tap[BTN_ENABLE]  || (tap[BTN_TOGGLE] && state_ == CruiseState::DISABLED);

    if (want_disable) {
        state_ = CruiseState::DISABLED;
        target_kph_ = 0.0f; pi_.reset();            // Disable throws the set speed away; Cancel does not
        ramped_ = 0.0f; ramp_valid_ = false; decay_rate_ = 0.0f;
    } else if (state_ == CruiseState::DISABLED) {
        if (want_enable) state_ = CruiseState::READY;
    }

    // A FAULT is not a cancel. It means the ECU no longer trusts its own picture — of the road speed,
    // of the stalk, or of its own loop — so it clears the set speed, drops the floor outright, and
    // BLOCKS engagement until the cause has gone AND every button has been let go. Resuming to a number
    // derived from a picture we just stopped believing is the thing this exists to prevent.
    const uint32_t live_faults = inhibit_ & kFaultMask;
    if (live_faults && state_ != CruiseState::DISABLED) {
        state_ = CruiseState::FAULT;
        latched_fault_ |= live_faults;
        target_kph_ = 0.0f; pi_.reset(); last_err_ = 0.0f;
        ramped_ = 0.0f; ramp_valid_ = false; decay_rate_ = 0.0f;
    } else if (state_ == CruiseState::FAULT) {
        // LEAVING A FAULT TAKES A DELIBERATE PRESS, not merely the absence of one. Some causes stop
        // being measurable the instant they take effect — the runaway timer only runs while cruising —
        // so "the cause has gone" becomes true one frame later, and a fault that rearmed on that alone
        // would exist for a single frame, which is indistinguishable from never having happened. A tap
        // also needs a release edge, so a stuck button cannot acknowledge anything.
        if (tap[BTN_ENABLE] || tap[BTN_TOGGLE] || tap[BTN_SET] || tap[BTN_RESUME]) {
            state_ = CruiseState::READY;                 // acknowledged: Ready, NOT straight to Cruising
            latched_fault_ = 0;
        }
    }
    if (state_ == CruiseState::FAULT) inhibit_ |= latched_fault_;   // the reason stays visible
    if (state_ != CruiseState::FAULT) latched_fault_ = 0;

    // Raised AFTER the state machine, so the codes and cruise_inhibit always say the same thing —
    // including the latched reason that is blocking engagement right now.
    raise_codes(now);

    if (state_ == CruiseState::CRUISING && (tap[BTN_CANCEL] || inhibit_ != 0)) {
        state_ = CruiseState::READY;                // the set speed SURVIVES, so Resume has a target
        pi_.reset(); last_err_ = 0.0f;
        if (inhibit_ & kHardCancel) {
            ramped_ = 0.0f; decay_rate_ = 0.0f;     // brake, clutch, handbrake: gone this frame
        } else {
            const float secs = static_cast<float>(cfg_->cancel_decay_s) * 0.001f;
            decay_rate_ = (secs > 0.0f) ? (ramped_ / secs) : 0.0f;
            if (decay_rate_ <= 0.0f) ramped_ = 0.0f;
        }
        ramp_valid_ = false;                        // the next engage snaps to the pedal again
    }

    // `entry` again, for the same reason as the bump buttons: the press that acknowledges a Fault was
    // made while faulted, so it may return the system to Ready and nothing more. Engaging needs a
    // second, deliberate press — made this time by someone who can see it is Ready.
    if (entry == CruiseState::READY && state_ == CruiseState::READY && inhibit_ == 0) {
        if (tap[BTN_SET]) {
            state_ = CruiseState::CRUISING;
            target_kph_ = speed;
        } else if (tap[BTN_RESUME] && target_kph_ > 0.0f) {
            state_ = CruiseState::CRUISING;         // straight back to the speed Cancel kept
        }
        if (state_ == CruiseState::CRUISING) {
            // Take over from the pedal the driver is holding. The error is zero at the latch, so the
            // output IS the integrator: starting it at pedal_demand puts the floor exactly where the
            // throttle already is, and lifting off hands over without the dip a zeroed integrator gives.
            const float pedal = bus.valid(SIG_PEDAL_DEMAND) ? bus.get(SIG_PEDAL_DEMAND, 0.0f) : 0.0f;
            pi_.out_min = 0.0f;
            pi_.out_max = static_cast<float>(cfg_->max_demand_pct) * 0.1f;
            pi_.integ   = std::clamp(pedal, pi_.out_min, pi_.out_max);
            ramped_     = pi_.integ;                // snap, not slew — see ramp_valid_ in the header
            ramp_valid_ = true;
            last_err_   = 0.0f;                     // so the first derivative term is zero, not a step
            decay_rate_ = 0.0f;
        }
    } else if (entry == CruiseState::CRUISING && state_ == CruiseState::CRUISING && tap[BTN_SET]) {
        target_kph_ = speed;                        // pressed again while cruising: re-latch to now
    }

    if (state_ != CruiseState::CRUISING) {
        // A button cancel bleeds the floor out over cancel_decay_s instead of dropping it, so the car
        // does not nod. ElectronicThrottle gates on bus.valid() alone and never reads cruise_active, so
        // a decaying demand is still honoured as a floor — which is exactly what makes this work.
        if (decay_rate_ > 0.0f && ramped_ > 0.0f) {
            ramped_ -= decay_rate_ * dt_s;
            if (ramped_ <= 0.0f) { ramped_ = 0.0f; decay_rate_ = 0.0f; }
        } else {
            ramped_ = 0.0f;
        }
        bus.set(SIG_CRUISE_ERROR_KPH, 0.0f, true, now, ttl());
        publish(ramped_);
        return;
    }

    // --- adjust the set speed ---------------------------------------------------------------------
    const float step = static_cast<float>(cfg_->speed_increment_kph) * 0.1f;
    if (entry == CruiseState::CRUISING) {
        if (tap[BTN_UP])   target_kph_ += step;
        if (tap[BTN_DOWN]) target_kph_ -= step;
        if (held[BTN_UP])   target_kph_ += static_cast<float>(cfg_->accel_rate_kph_s) * 0.1f * dt_s;
        if (held[BTN_DOWN]) target_kph_ -= static_cast<float>(cfg_->coast_rate_kph_s) * 0.1f * dt_s;
    }
    target_kph_ = std::clamp(target_kph_, min_kph, max_kph);

    // --- the loop ---------------------------------------------------------------------------------
    // The gains come off curves that index cruise_error_kph, so the error has to be ON the bus before
    // they are read. Publishing it afterwards would index last frame's error, silently.
    const float error = target_kph_ - speed;
    bus.set(SIG_CRUISE_ERROR_KPH, error, true, now, ttl());
    pi_.kp = tbl::table_eval(cruise_p_gain_desc(cfg_), bus);
    pi_.ki = tbl::table_eval(cruise_i_gain_desc(cfg_), bus);
    pi_.out_min = 0.0f;
    pi_.out_max = static_cast<float>(cfg_->max_demand_pct) * 0.1f;

    const float kd = tbl::table_eval(cruise_d_gain_desc(cfg_), bus);

    // DRIVER OVERRIDE. While the pedal is above the floor the driver is driving, not the loop. Freezing
    // the integrator here is what stops an overtake — a long, large negative error — from unwinding it
    // to zero, which would collapse the floor the instant the driver lifted, at the worst moment.
    const float pedal = bus.valid(SIG_PEDAL_DEMAND) ? bus.get(SIG_PEDAL_DEMAND, 0.0f) : 0.0f;
    const bool  overridden = pedal > ramped_ + 1.0f;

    float target_pct;
    if (overridden || dt_s <= 0.0f) {
        target_pct = std::clamp(pi_.integ, 0.0f, pi_.out_max);
        last_err_  = error;                       // keep the derivative honest across the freeze
    } else {
        const float d_term = kd * (error - last_err_) / dt_s;
        last_err_ = error;
        // The D term goes INTO the step, so the anti-windup judges saturation on what is applied —
        // the integrator stops accumulating demand the D term has already pushed past the ceiling.
        target_pct = std::clamp(pi_.step(error, dt_s, d_term), 0.0f, pi_.out_max);
    }

    // How fast the floor may move. Not at the moment of engagement — see ramp_valid_.
    const float rate = static_cast<float>(cfg_->max_ramp_pct_s) * 0.1f;
    if (!ramp_valid_ || dt_s <= 0.0f || rate <= 0.0f) {
        ramped_ = target_pct; ramp_valid_ = true;
    } else {
        const float step_pct = rate * dt_s;
        ramped_ = std::clamp(target_pct, ramped_ - step_pct, ramped_ + step_pct);
    }
    publish(std::clamp(ramped_, 0.0f, 100.0f));
}
