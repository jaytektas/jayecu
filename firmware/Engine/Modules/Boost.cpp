#include "Boost.h"
#include "well_known_signals.h"            // wk:: roles — rename-safe (rpm/map)
#include "../../Diagnostics/DtcManager.h"
#include "../../../generated/module_dtc.h"
#include "../../Diagnostics/Dtc.h"
#include "../EngineStateMachine.h"          // EngineRunState
#include "../TableEval.h"                   // tbl::table_eval
#include "../../../generated/table_descs.h" // <table>_desc(cfg)
#include "../../Platform/platform_hal.h"    // platform_get_tick_ms
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../../generated/learned_layout.h"   // LEARNED_BOOST_LTT_* (fixed offset + dims)
#include <algorithm>
#include <cmath>

namespace {
// Tenths of a second to milliseconds, saturated: OutputGateTimings counts in uint16 ms, so 65.5 s is
// the most it can express and a longer setting would otherwise wrap to a very short one.
inline uint16_t to_ms(uint16_t tenths) {
    const uint32_t ms = static_cast<uint32_t>(tenths) * 100u;
    return ms > 65535u ? static_cast<uint16_t>(65535) : static_cast<uint16_t>(ms);
}
}  // namespace

void Boost::reset_state() {
    pi_.reset();
    last_ms_  = 0;
    last_err_ = 0.0f;
    active_since_ms_ = 0;
    ltt_dwell_cell_ = -1;      // the learned VALUES persist; only the dwell tracking restarts
    gate_pi_.reset();
    gate_cmd_ = -1.0f;
    gate_last_err_ = 0.0f;
    gate_last_ms_ = 0;
    scramble_ = OutputGate{};
    scramble_.on = 0;          // OutputGate ships ON (a fan's failsafe); a boost bump starts OFF
}

void Boost::init(const BoostConfig& cfg) {
    cfg_ = &cfg;
    reset_state();

    // Map the trim onto its FIXED slice of the learned region. The region is live RAM restored from
    // the SD totem before init, so mapping is all that is needed: zero means "nothing learned yet",
    // which is also the neutral value, so a board that has never run reads exactly as one that has
    // been reset. Mapped once — init runs once, in compose.
    if (!ltt_) {
        auto* base = platform_learned_block(LEARNED_BOOST_LTT_OFFSET, LEARNED_BOOST_LTT_BYTES);
        if (base) {
            ltt_ = reinterpret_cast<int16_t*>(base);
        } else {
            static int16_t fallback[LEARNED_BOOST_LTT_CELLS] = {};   // no room -> RAM, re-learn each boot
            ltt_ = fallback;
        }
    }
}

void Boost::on_config_change(const BoostConfig& cfg) {
    cfg_ = &cfg;
    reset_state();   // a retune shouldn't carry stale wind-up
}

// The inner position loop. `demand` is the outer loop's answer in the units the rest of the module
// speaks — per cent of "hold it shut" — and the valve's own command is its complement, clamped to the
// travel the gate actually has and slewed so a step from above becomes a ramp.
//
// It publishes NOTHING for the solenoid duty, because in this mode there is no solenoid; and when the
// module is not controlling at all it publishes nothing here either, which drops the bridge and leaves
// the gate on its spring. That is why the gate must be spring-OPEN: it is the only failure mode a
// pressure loop cannot see coming.
void Boost::drive_gate(SignalBus& bus, uint32_t now, float demand) {
    const float lo = static_cast<float>(cfg_->gate_min_pos_pct) * 0.1f;
    const float hi = static_cast<float>(cfg_->gate_max_pos_pct) * 0.1f;
    float want = std::clamp(100.0f - demand, std::min(lo, hi), std::max(lo, hi));

    const float dt = (gate_last_ms_ != 0 && now > gate_last_ms_) ? (now - gate_last_ms_) / 1000.0f : 0.0f;
    gate_last_ms_ = now;
    if (gate_cmd_ < 0.0f) gate_cmd_ = want;                       // first command: start where asked
    if (cfg_->gate_rate_pct_s && dt > 0.0f) {
        const float step = static_cast<float>(cfg_->gate_rate_pct_s) * dt;
        gate_cmd_ += std::clamp(want - gate_cmd_, -step, step);
    } else {
        gate_cmd_ = want;
    }
    bus.set(wk::wastegate_pos_target, gate_cmd_, true, now, ttl());

    const float actual = bus.get(static_cast<SignalId>(cfg_->gate_pos_sig), 0.0f);
    const float err    = gate_cmd_ - actual;
    gate_pi_.kp      = cfg_->gate_kp * 0.001f;
    gate_pi_.ki      = cfg_->gate_ki * 0.001f;
    gate_pi_.out_min = -100.0f;                                   // a motor goes both ways
    gate_pi_.out_max =  100.0f;
    float out = (dt > 0.0f) ? gate_pi_.step(err, dt) : gate_pi_.integ;

    const float imax = static_cast<float>(cfg_->gate_iterm_max_pct) * 0.1f;
    if (imax > 0.0f) {
        const float capped = std::clamp(gate_pi_.integ, -imax, imax);
        out -= (gate_pi_.integ - capped);   // the clip comes off the output too — see the outer loop
        gate_pi_.integ = capped;
    }
    if (dt > 0.0f && cfg_->gate_kd != 0)
        out += (cfg_->gate_kd * 0.001f) * (err - gate_last_err_) / dt;
    gate_last_err_ = err;

    bus.set(wk::wastegate_pos_duty, std::clamp(out, -100.0f, 100.0f), true, now, ttl());
}

// THE OVERBOOST BACKSTOP, on EVERY path — including boost control switched off, unarmed or below its
// activation point. It used to sit at the end of the active path only, so with the arm switch off, boost
// control disabled or rpm under activation_rpm there was no cut at all — and "spring only" is exactly
// when a stuck gate or boost creep happens. `target` < 0 means there is no target to be relative to
// (off / unarmed / inactive), so only the absolute limit applies.
//
// HYSTERESIS: once cutting, it holds until the manifold is overboost_hyst below the limit that tripped
// it. Without it the cut chattered on and off at the limit every frame.
void Boost::overboost(SignalBus& bus, float map, float target, uint32_t now) {
    if (!cfg_) return;
    const float hyst   = static_cast<float>(cfg_->overboost_hyst_kpa) * 0.1f;
    const float abs_lim = static_cast<float>(cfg_->overboost_limit_kpa) * 0.1f;
    const float rel_lim = target + static_cast<float>(cfg_->overboost_offset_kpa) * 0.1f;
    const bool  rel_ok  = cfg_->overboost_offset_kpa != 0 && target >= 0.0f
                       && target >= static_cast<float>(cfg_->activation_kpa) * 0.1f;
    const float release = ob_cutting_ ? hyst : 0.0f;     // trip AT the limit, release BELOW it by hyst
    const bool over_abs = cfg_->overboost_limit_kpa != 0 && map >= abs_lim - release;
    const bool over_rel = rel_ok && map >= rel_lim - release;
    ob_cutting_ = over_abs || over_rel;
    if (ob_cutting_) {
        // cut_method: 0 = fuel, 1 = ignition, 2 = both. validity-OR: publish true only while over the limit.
        if (cfg_->overboost_cut_method == 0 || cfg_->overboost_cut_method == 2) bus.set_bool(wk::fuel_cut, true, now, ttl());
        if (cfg_->overboost_cut_method == 1 || cfg_->overboost_cut_method == 2) bus.set_bool(wk::ign_cut,  true, now, ttl());
    }
}

void Boost::update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) {
    const uint32_t now = platform_get_tick_ms();
    // THE ARM SWITCH, read before anything else because it is the same answer as being switched off —
    // nothing published, the wastegate on its spring. A gate, not a mode: the tune is unchanged behind
    // it, so closing the switch resumes rather than re-configures.
    const bool armed = !cfg_ || cfg_->arm_sig < 0
                    || bus.get(static_cast<SignalId>(cfg_->arm_sig), 0.0f) > 0.5f;
    if (!cfg_ || !cfg_->enabled || !armed) {
        reset_state();
        if (cfg_) overboost(bus, bus.get(wk::map, 0.0f), -1.0f, now);   // the backstop still stands
        frame.boost_target_kpa = 0.0f;
        bus.set(wk::boost_target, 0.0f, true, now, ttl());
        // Heal what we raised: this return skips the heal() below — see DtcManager::heal.
        if (was_enabled_ && dtc_) dtc_->heal(ModuleDtc::BOOST_MAP);
        was_enabled_ = false;
        return;                            // wastegate_duty: publish nothing -> output failsafes (spring)
    }
    was_enabled_ = true;

    const float    rpm = pos.rpm;
    const float    map = bus.get(wk::map, 0.0f);   // manifold pressure [kPa]

    if (dtc_) {
        if (!bus.valid(wk::map))
            dtc_->raise(ModuleDtc::BOOST_MAP, DtcSource::MODULE, ModuleDtc::BOOST_MAP_SEV, now, dtc_ttl());
        else
            dtc_->heal(ModuleDtc::BOOST_MAP);
    }

    // --- Boost target (kPa) from the target table, published for telemetry regardless of activation ---
    float target = tbl::table_eval(boost_target_table_desc(cfg_), bus);

    // THE TRIM KNOB: an analog input read as 0-100 %, moving the target by up to its authority. At 0 %
    // it does nothing, so the knob's off position is its own bottom stop. The authority is signed and
    // ships NEGATIVE, which is what makes a knob left somewhere, or an input failed high, unable to ask
    // for more boost than the map authorised.
    if (cfg_->trim_sig >= 0) {
        const float knob = std::clamp(bus.get(static_cast<SignalId>(cfg_->trim_sig), 0.0f), 0.0f, 100.0f);
        target += static_cast<float>(cfg_->trim_max_kpa) * 0.1f * (knob * 0.01f);
    }

    // SCRAMBLE, through the same gate an output slot uses. Hold is its minimum-on (which is what turns
    // a momentary switch into a tap), maximum-on is what stops the button being a permanent boost
    // switch, and the rest is the lockout that follows a maximum trip — OutputGate applies the lockout
    // only on that trip, which is exactly the rule wanted here.
    //
    // >= 0, NOT != 0: selectors are int16 with -1 meaning unassigned.
    bool scramble = false;
    if (cfg_->scramble_sig >= 0) {
        const bool ask = bus.get(static_cast<SignalId>(cfg_->scramble_sig), 0.0f) > 0.5f;
        OutputGateTimings gt{};
        gt.min_on_ms = to_ms(cfg_->scramble_hold_s);
        gt.max_on_ms = to_ms(cfg_->scramble_max_s);
        gt.rearm_ms  = to_ms(cfg_->scramble_rest_s);
        scramble_.step(ask, !ask, gt, now);
        scramble = scramble_.on != 0;
    }
    if (scramble) target += cfg_->scramble_kpa * 0.1f;       // scramble_kpa scale 0.1

    // --- The correction slots -------------------------------------------------------------------
    //
    // Four slots, each a channel, a curve and which of the two numbers it trims. Summed here into two
    // totals so the rest of the function reads the same whether nobody enabled a slot or everybody did.
    //
    // Per cent either way, and each is the natural operation for what it trims: the TARGET correction
    // multiplies (a tenth less boost is a tenth less at any target, which is also how the protection
    // pull-back below already works), the DUTY correction adds (duty is a percentage already, so the
    // number is points of it).
    float corr_target_pct = 0.0f, corr_duty_pct = 0.0f;
    {
        const uint8_t  en[4]  = { cfg_->corr1_en, cfg_->corr2_en, cfg_->corr3_en, cfg_->corr4_en };
        const uint8_t  ap[4]  = { cfg_->corr1_applies, cfg_->corr2_applies,
                                  cfg_->corr3_applies, cfg_->corr4_applies };
        for (int i = 0; i < 4; ++i) {
            if (!en[i]) continue;                       // not evaluated at all, which is not the same
            const tbl::TableDesc td =                   //  thing as a curve of zeroes: a slot that is
                  (i == 0) ? corr1_table_desc(cfg_)     //  off costs nothing, not even its descriptor
                : (i == 1) ? corr2_table_desc(cfg_)
                : (i == 2) ? corr3_table_desc(cfg_)
                           : corr4_table_desc(cfg_);
            const float v = tbl::table_eval(td, bus);
            if (ap[i] == 0) corr_target_pct += v; else corr_duty_pct += v;
        }
    }
    // PER CENT OF BOOST, not of absolute pressure. The target is absolute kPa, so multiplying it whole
    // made a -10 % correction on a 200 kPa target take 20 kPa — a FIFTH of the 100 kPa of boost — and a
    // -50 % one remove all of it. The help says what was meant: "-10 % asks for a tenth less boost". So
    // the per-cent terms scale the part above atmosphere and leave the atmosphere alone.
    const float baro = bus.get(SIG_BARO_KPA, 101.3f);
    auto scale_boost = [baro](float t, float factor) { return baro + std::max(0.0f, t - baro) * factor; };
    target = scale_boost(target, std::max(0.0f, 1.0f + corr_target_pct * 0.01f));

    // SAFETY: EngineProtection's boost correction pulls the target back. Read off the bus (wk::prot_boost_corr,
    // signed %; -100 -> kill boost, 0/absent -> no change). corr = 1 + pct/100, clamped to [0,1] so it only
    // ever reduces. Absent/expired => 0 => no correction (protection not asking) — the safe default.
    const float prot_corr = bus.valid(wk::prot_boost_corr) ? bus.get(wk::prot_boost_corr, 0.0f) : 0.0f;
    const float corr = std::clamp(1.0f + prot_corr / 100.0f, 0.0f, 1.0f);
    target = scale_boost(target, corr);                 // -100 % -> atmospheric: no boost at all

    frame.boost_target_kpa = target;
    bus.set(wk::boost_target, target, true, now, ttl());

    // THE ERROR, PUBLISHED, and published here rather than inside the closed-loop branch. It is the
    // one number that says whether boost control is working, so it belongs in a log beside the two it
    // is made of whatever mode the module is in — and the integral gain curve reads it as an AXIS,
    // which a float living on the stack could not be.
    const float error = target - map;      // under boost -> positive -> open the wastegate less
    bus.set(wk::boost_error, error, true, now, ttl());

    // --- Activation gate: rpm above threshold AND map above the load floor (don't fight the spring) ---
    const bool active = rpm >= static_cast<float>(cfg_->activation_rpm)
                     && map >= static_cast<float>(cfg_->activation_kpa) * 0.1f;   // activation_kpa scale 0.1
    if (!active) {
        // Below activation: relax the loop so closed-loop re-engages cleanly; publish nothing (spring).
        reset_state();
        overboost(bus, map, -1.0f, now);             // …but the backstop still stands
        return;
    }
    if (active_since_ms_ == 0) active_since_ms_ = now ? now : 1;   // 0 is the "not active" sentinel

    const float max_duty = static_cast<float>(cfg_->max_duty_pct) * 0.1f;

    // WHICH TRIM CELL the engine is sitting in — found through the base duty table's OWN axes, which
    // is what keeps the two grids in step: trim cell (i,j) is base duty cell (i,j) by construction,
    // not by two places agreeing about breakpoints. Row 0 when the rpm axis is switched off, because
    // then the base table is a single row and so is its trim.
    const tbl::TableDesc bd_desc = base_boost_duty_table_desc(cfg_);
    const int ltt_col = tbl::nearest_bin(bd_desc.x, target);
    const int ltt_row = cfg_->base_boost_duty_table_y_en ? tbl::nearest_bin(bd_desc.y, rpm) : 0;
    int ltt_cell = ltt_row * static_cast<int>(LEARNED_BOOST_LTT_COLS) + ltt_col;
    if (ltt_col < 0 || ltt_row < 0 || ltt_cell >= static_cast<int>(LEARNED_BOOST_LTT_CELLS))
        ltt_cell = -1;
    const float ltt_pct = (ltt_ && ltt_cell >= 0) ? ltt_[ltt_cell] * 0.1f : 0.0f;
    bus.set(wk::boost_ltt_pct, ltt_pct, true, now, ttl());

    // --- Feed-forward base duty, plus whatever the duty-side slots ask for ---
    //
    // The slot correction goes on BEFORE the protection scaling, not after: everything the tune asks
    // for is assembled first and the safety correction then scales the lot. Added after it, a +20 %
    // duty correction would have survived a kill-boost and gone on driving the solenoid.
    //
    // It also goes on before the PID rather than onto the final output, so the loop's authority window
    // below is computed against the duty actually being delivered.
    // The scramble duty bump rides with the slot corrections — inside the protection scaling, for the
    // same reason: a bump added after it would go on driving the solenoid through a kill-boost.
    const float scramble_duty = scramble ? static_cast<float>(cfg_->scramble_base_pct) * 0.1f : 0.0f;
    float duty = (tbl::table_eval(bd_desc, bus)
                  + ltt_pct + corr_duty_pct + scramble_duty) * corr;

    // --- Closed loop: PID trim on (target - map), but only once the loop has TAKEN OVER ---
    bool handed_over = false;
    if (cfg_->mode == 1) {
        // THE TWO HANDOVER TESTS, and both must pass. The control point asks whether boost is close
        // enough to target for the loop to be correcting rather than waiting; the start delay asks
        // whether enough time has passed since activation for there to be anything to correct. They
        // answer different failure modes — a gate that reaches the control point early still waits,
        // and one that never reaches it is never handed over however long the delay was.
        const float control_point = target - static_cast<float>(cfg_->control_point_kpa) * 0.1f;
        bool ready = map >= control_point;
        // THE THROTTLE GATE. At part throttle the manifold is nowhere near target through no fault of
        // the wastegate, and a loop that closed there would read a large standing error and wind up
        // against something it cannot move. An unreadable throttle (no signal, dead sensor) reads 0
        // and so keeps the loop out — the same resting state, which is the safe direction.
        if (ready && cfg_->min_tps_pct)
            ready = bus.get(wk::tps, 0.0f) >= static_cast<float>(cfg_->min_tps_pct) * 0.1f;
        if (ready && cfg_->start_delay_en) {
            const float delay_s = tbl::table_eval(start_delay_table_desc(cfg_), bus);
            ready = (now - active_since_ms_) >= static_cast<uint32_t>(delay_s * 1000.0f);
        }

        if (!ready) {
            // NOT YET OURS. Rest on the feed-forward — or hold the gate shut with spool assist, which
            // is what makes the climb quickest and the arrival least forgiving. The integrator is
            // FROZEN rather than reset: a reset here would wipe the loop's trim on every lift and every
            // gearchange, and the reason for holding off is that nothing should accumulate during the
            // spool, not that what was learned before it is wrong.
            if (cfg_->spool_assist) duty = 100.0f;   // the final clamp brings it to max_duty
            // The CLOCK AND THE ERROR HISTORY keep running even though the loop does not, so the
            // handover costs nothing: dt is one frame rather than zero (a zero dt returns the bare
            // integrator and drops the proportional term for a frame) and the derivative sees no step
            // where there was none.
            last_ms_  = now;
            last_err_ = error;
        } else {
            handed_over = true;
            const float dt = (last_ms_ != 0) ? (now - last_ms_) / 1000.0f : 0.0f;

            // PI authority is the headroom around the feed-forward, so anti-windup matches the final clamp.
            pi_.kp      = cfg_->kp * 0.001f;        // %/kPa
            // KI, SCHEDULED OR NOT. The curve reads the error published above, so it is this frame's
            // error and not the last one's. It is the only gain worth scheduling: near target a large
            // Ki hunts, because it goes on integrating a difference that is mostly noise, and far from
            // target the same difference is a real offset nothing else is going to cover.
            pi_.ki      = cfg_->ki_sched_en ? tbl::table_eval(ki_table_desc(cfg_), bus)
                                            : cfg_->ki * 0.001f;   // %/kPa/s
            pi_.out_min = -duty;                    // trim may pull the base all the way to 0
            pi_.out_max = max_duty - duty;          // ...up to the ceiling
            float trim = (dt > 0.0f) ? pi_.step(error, dt) : pi_.integ;

            // THE INTEGRATOR'S OWN CEILING, tighter than the anti-windup. Anti-windup asks what the
            // output can deliver; this asks how much of the answer should come from accumulated error
            // rather than from the base table. The excess comes off the OUTPUT as well as the
            // accumulator — trim was (p + integ), so returning the clipped integ means subtracting
            // exactly what was clipped, and forgetting that would leave the loop delivering a term it
            // had just decided it was not allowed.
            const float iterm_max = static_cast<float>(cfg_->iterm_max_pct) * 0.1f;
            if (iterm_max > 0.0f) {
                const float capped = std::clamp(pi_.integ, -iterm_max, iterm_max);
                trim -= (pi_.integ - capped);
                pi_.integ = capped;
            }

            // Optional derivative term (kd scale 0.001), damps overshoot. dt>0 only.
            //
            // The RATE is clamped, not the term. D differentiates, so it amplifies noise by
            // definition, and one ragged MAP sample on a fast spool is otherwise a duty spike out of
            // nothing. Limiting kPa/s rather than the resulting per cent keeps the number in units
            // that can be read off a log and compared with what the engine actually did.
            float d_term = 0.0f;
            if (dt > 0.0f && cfg_->kd != 0) {
                float de = (error - last_err_) / dt;               // kPa/s
                const float dmax = static_cast<float>(cfg_->max_deriv_kpa_s) * 0.1f;
                if (dmax > 0.0f) de = std::clamp(de, -dmax, dmax);
                d_term = (cfg_->kd * 0.001f) * de;
            }
            last_err_ = error;
            last_ms_  = now;

            duty += trim + d_term;
        }
    }

    // THE FLOOR IS SCALED BY THE PROTECTION CORRECTION, not applied under it. A floor holds the gate
    // partly shut, which makes MORE boost, so a fixed one would answer a correction demanding less
    // boost by refusing to go below itself — and at kill-boost it would still be driving the solenoid.
    // Scaled, it falls with the correction and reaches zero exactly when the correction does. The
    // min(max_duty) keeps a misconfigured floor above the ceiling from inverting the clamp.
    // --- Long-term trim learn ---------------------------------------------------------------------
    //
    // What the loop keeps having to correct is a base table that is not finished, so the standing part
    // of the integrator is migrated into a cell and bled out of the loop. Total correction is
    // unchanged at the moment it happens; what changes is where it lives, and next time the
    // feed-forward already knows.
    //
    // ONLY WHILE HANDED OVER, and only inside the gates. A reading taken during the spool, at part
    // throttle, or in a gear whose target is deliberately lowered is a correction that belongs to the
    // condition rather than to the engine — and it would be written into a cell that will be read back
    // under quite different circumstances.
    const float ltt_tps  = bus.get(wk::tps, 0.0f);
    // SIG_GEAR directly: gear has no wk:: alias, and CruiseControl reads it the same way. An INVALID
    // gear (no signal, or a detector that has not resolved one) must not read as gear 0 and silently
    // fail a minimum-gear gate on a car that simply cannot answer the question — so an unanswerable
    // gear passes the gate, and the other three still have to agree.
    const float ltt_gear = bus.valid(SIG_GEAR) ? bus.get(SIG_GEAR, 0.0f) : 1e9f;
    const bool learn_ok = cfg_->ltt_en && handed_over && ltt_ && ltt_cell >= 0
                       && rpm >= static_cast<float>(cfg_->ltt_min_rpm)
                       && rpm <= static_cast<float>(cfg_->ltt_max_rpm)
                       && ltt_tps >= static_cast<float>(cfg_->ltt_min_tps_pct) * 0.1f
                       && (cfg_->ltt_min_gear == 0 || ltt_gear >= static_cast<float>(cfg_->ltt_min_gear));
    if (learn_ok) {
        // The dwell restarts whenever the operating point moves cell, so a sweep across the map writes
        // nothing: the loop has to settle somewhere before its integrator means what it says.
        if (ltt_cell != ltt_dwell_cell_) {
            ltt_dwell_cell_ = ltt_cell;
            ltt_dwell_since_ms_ = now;
        } else if ((now - ltt_dwell_since_ms_) >= cfg_->ltt_dwell_ms) {
            const float move = (static_cast<float>(cfg_->ltt_learn_pct) * 0.1f / 100.0f) * pi_.integ;
            const float lim  = static_cast<float>(cfg_->ltt_authority_pct) * 0.1f;
            const float next = std::clamp(ltt_pct + move, -lim, lim);
            ltt_[ltt_cell] = static_cast<int16_t>(std::lround(next * 10.0f));
            // Bleed only what the cell actually TOOK. Clamped at the authority the cell absorbs
            // nothing more, and bleeding the intended move anyway would hand the loop's correction to
            // a table that refused it — boost would fall away by exactly the amount that went nowhere.
            pi_.bleed(next - ltt_pct);
            ltt_dwell_since_ms_ = now;
        }
    } else {
        ltt_dwell_cell_ = -1;
    }

    const float min_duty = std::min(static_cast<float>(cfg_->min_duty_pct) * 0.1f * corr, max_duty);
    duty = std::clamp(duty, min_duty, max_duty);

    // --- What the answer is given TO -------------------------------------------------------------
    //
    // Everything above this line is the same question either way: how much boost to ask for, expressed
    // as "hold the gate this shut". A SOLENOID takes that as a duty and the spring does the rest. A
    // MOTORISED GATE takes a position, and the demand is the same number read the other way round —
    // a valve held 70 % shut is a valve 30 % open.
    if (cfg_->output_mode == 1 && cfg_->gate_pos_sig >= 0) {
        drive_gate(bus, now, duty);
    } else {
        bus.set(wk::wastegate_duty, duty, true, now, ttl());
    }

    // --- Overboost backstop: a hard cut as a last resort, in addition to the P0234 DTC path ---
    //
    // TWO DIFFERENT QUESTIONS. The absolute limit asks whether this is more than the engine can take,
    // and does not move. The offset asks whether this is much more than was ASKED for, which is what
    // catches a stuck gate or a split hose while the target is low and the absolute ceiling is miles
    // away. The offset needs a real target to be relative to, so it is skipped below the activation
    // pressure — otherwise a protection event that zeroes the target makes every remaining kilopascal
    // an infinite overshoot and asks for a cut on the spot.
    overboost(bus, map, target, now);
}
