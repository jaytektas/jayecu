#include "TractionControl.h"
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../TableEval.h"                      // tbl::table_eval — the target and the responses are curves
#include "../../Signal/Expr.h"                 // expr::exec — "Active When" is a condition, not a switch
#include "../../../generated/signal_ids.h"
#include "well_known_signals.h"
#include "../../../generated/table_descs.h"
#include "../../../generated/ecu_config.h"     // g_config (live tune RAM)
#include "../../../generated/table_registry.h" // expr_table_* — the tables a condition may read
#include "../../../generated/module_dtc.h"
#include "../../Platform/platform_hal.h"       // platform_get_tick_ms (publish TTL timestamp)
#include <algorithm>
#include <cmath>

extern volatile uint32_t g_config_generation;  // bumped by every config write (CommsManager.cpp)

namespace {
// Steady slip that means the two speed sources disagree rather than that a wheel is spinning. Below
// 1% is tyre-to-tyre noise; above 2%, held for ten seconds at a cruise with the engine doing nothing
// about it, is a calibration difference and nothing else.
constexpr float kCalWarnPct   = 2.0f;
constexpr float kCalHealPct   = 1.0f;
constexpr uint32_t kCalHoldMs = 10000;
constexpr float kCalMinSpeed  = 30.0f;         // below this a percentage of a small number is noise
}   // namespace

void TractionControl::reset() {
    cap_ = 100.0f; slip_ = 0.0f; retard_ = 0.0f; cut_pct_ = 0.0f; cutting_ = false;
    reduction_ = 0.0f; cut_duty_.reset(); last_ms_ = 0;
    cal_ema_ = 0.0f; cal_bad_ms_ = 0;
    pi_.reset();
}

void TractionControl::revalidate() {
    // exec() assumes validate() has passed (Expr.h), and on_config_change is dead API for this, so the
    // module watches the generation counter itself — the ScriptEngine/Sensors/CruiseControl pattern.
    expr_bad_ = cfg_ && !expr::is_empty(cfg_->enable_expr, sizeof cfg_->enable_expr) &&
                expr::validate(cfg_->enable_expr, sizeof cfg_->enable_expr,
                               sizeof(g_config), EXPR_TABLE_COUNT) != expr::Invalid::None;
}

void TractionControl::publish_neutral(SignalBus& bus, uint32_t now) {
    cap_ = 100.0f; slip_ = 0.0f; retard_ = 0.0f; cut_pct_ = 0.0f; cutting_ = false;
    reduction_ = 0.0f; cut_duty_.reset();
    pi_.reset();
    bus.set(SIG_TRACTION_CAP,      100.0f, true, now, ttl());   // 100% = no cap
    bus.set(SIG_TRACTION_SLIP,       0.0f, true, now, ttl());
    bus.set(SIG_TRACTION_SLIP_ERR,   0.0f, true, now, ttl());
    bus.set(SIG_TRACTION_RETARD,     0.0f, true, now, ttl());
    bus.set(SIG_TRACTION_CUT_PCT,    0.0f, true, now, ttl());
}

void TractionControl::update(const EnginePosition& /*pos*/, SignalBus& bus, EngineFrame& /*frame*/) {
    const uint32_t now = platform_get_tick_ms();

    if (!cfg_ || !cfg_->enabled) {
        // Heal on the enabled->disabled EDGE, not every frame: a code raised by something else must not
        // be cleared by a module that is merely switched off.
        if (was_enabled_ && dtc_) {
            dtc_->heal(ModuleDtc::TRACTION_NO_REF);    dtc_->heal(ModuleDtc::TRACTION_NO_DRIVEN);
            dtc_->heal(ModuleDtc::TRACTION_AXLE_CAL);  dtc_->heal(ModuleDtc::TRACTION_EXPR);
            cal_raised_ = false;
        }
        was_enabled_ = false;
        publish_neutral(bus, now);
        return;
    }
    was_enabled_ = true;
    if (g_config_generation != cfg_gen_seen_) { cfg_gen_seen_ = g_config_generation; revalidate(); }

    const float dt_s = (last_ms_ == 0) ? 0.0f
                     : std::min(0.1f, static_cast<float>(now - last_ms_) * 0.001f);
    last_ms_ = now;

    // --- is it allowed to act at all --------------------------------------------------------------
    // A GATE THAT CANNOT BE ANSWERED IS NOT "CONDITION FALSE". A program that failed to validate holds
    // traction control off and says so; silently reading as "not asked for" is how a car ends up with no
    // traction control and nothing to explain it.
    if (expr_bad_) {
        if (dtc_) dtc_->raise(ModuleDtc::TRACTION_EXPR, DtcSource::MODULE,
                              ModuleDtc::TRACTION_EXPR_SEV, now, dtc_ttl());
        publish_neutral(bus, now);
        return;
    }
    if (dtc_) dtc_->heal(ModuleDtc::TRACTION_EXPR);
    if (!expr::is_empty(cfg_->enable_expr, sizeof cfg_->enable_expr)) {
        expr::Ctx ex;
        ex.bus = &bus; ex.cfg = reinterpret_cast<const uint8_t*>(&g_config);
        ex.cfg_size = sizeof(g_config); ex.now_ms = now;
        ex.table = expr_table_value; ex.curve = expr_table_at;
        ex.curve_user = &bus; ex.curve_count = EXPR_TABLE_COUNT;
        if (!expr::exec(cfg_->enable_expr, sizeof cfg_->enable_expr, ex).truthy()) {
            publish_neutral(bus, now);          // switched off by the driver: not a fault, no codes
            return;
        }
    }

    // --- what is turning --------------------------------------------------------------------------
    constexpr SignalId kW[4] = { SIG_WHEEL_FL, SIG_WHEEL_FR, SIG_WHEEL_RL, SIG_WHEEL_RR };
    float w[4]; bool ok[4];
    for (uint8_t i = 0; i < 4; ++i) {
        ok[i] = bus.valid(kW[i]);
        w[i]  = ok[i] ? bus.get(kW[i], 0.0f) : 0.0f;
    }
    const bool rear = (cfg_->driven_axle != 0);
    const uint8_t dA = rear ? 2 : 0, dB = rear ? 3 : 1;      // driven pair

    // THE FASTEST DRIVEN WHEEL, not their average. An open diff lets one wheel spin while the other
    // grips, and averaging the pair reports half the slip that is actually happening; on a locked or
    // limited-slip axle the two agree and the fastest IS the average. One answer, right in both cases.
    //
    // …OR THE DRIVE TRAIN, on the car that has no driven-wheel pickups. A gearbox pickup reads the
    // driven wheels averaged through the diff, which is the driven speed — so a gearbox sensor and one
    // undriven wheel is a complete traction control install, and used to be one this module refused.
    float driven = 0.0f; bool driven_ok = false;
    if (cfg_->driven_src == DriveTrain) {
        driven_ok = bus.valid(SIG_SHAFT_SPD);
        if (driven_ok) driven = bus.get(SIG_SHAFT_SPD, 0.0f);
    } else {
        if (ok[dA])            { driven = w[dA]; driven_ok = true; }
        if (ok[dB] && (!driven_ok || w[dB] > driven)) { driven = w[dB]; driven_ok = true; }
    }
    if (!driven_ok) {
        if (dtc_) dtc_->raise(ModuleDtc::TRACTION_NO_DRIVEN, DtcSource::MODULE,
                              ModuleDtc::TRACTION_NO_DRIVEN_SEV, now, dtc_ttl());
        publish_neutral(bus, now);
        return;
    }
    if (dtc_) dtc_->heal(ModuleDtc::TRACTION_NO_DRIVEN);

    // --- and what the road is doing ---------------------------------------------------------------
    // ONE ANSWER, FROM ONE PLACE. vehicle_spd is whatever Vehicle Speed's Main Source names — the
    // undriven axle on a two-wheel-drive car, GPS on a four-wheel-drive one. This module used to choose
    // for itself, which meant the ECU held two answers to "how fast is the car going" and could not say
    // which was right.
    if (!bus.valid(wk::vehicle_spd)) {
        if (dtc_) dtc_->raise(ModuleDtc::TRACTION_NO_REF, DtcSource::MODULE,
                              ModuleDtc::TRACTION_NO_REF_SEV, now, dtc_ttl());
        publish_neutral(bus, now);
        return;
    }
    if (dtc_) dtc_->heal(ModuleDtc::TRACTION_NO_REF);
    const float reference = bus.get(wk::vehicle_spd, 0.0f);

    if (reference < static_cast<float>(cfg_->min_speed_kph) || reference <= 0.0f) {
        publish_neutral(bus, now);                   // too slow to divide by: not a fault
        return;
    }

    // --- the measurement --------------------------------------------------------------------------
    slip_ = (driven - reference) / reference * 100.0f;
    bus.set(SIG_TRACTION_SLIP, slip_, true, now, ttl());

    // THE ERROR GOES ON THE BUS BEFORE THE TABLES ARE READ. Their axis IS traction_slip_err, so
    // publishing afterwards would index LAST frame's error — a silent one-frame lag, and the same rule
    // cruise control states at its own gain curves.
    const float target = tbl::table_eval(slip_target_desc(cfg_), bus);
    const float err    = slip_ - target;
    bus.set(SIG_TRACTION_SLIP_ERR, err, true, now, ttl());

    retard_  = std::max(0.0f, tbl::table_eval(retard_deg_desc(cfg_), bus));
    cut_pct_ = std::clamp(tbl::table_eval(cut_pct_desc(cfg_), bus), 0.0f, 100.0f);

    // --- the throttle ceiling ---------------------------------------------------------------------
    // PULL IMMEDIATELY, GIVE BACK SLOWLY. Over target the PI decides how much throttle to take; under
    // it, the reduction bleeds off at a fixed rate rather than vanishing. Snapping the ceiling back to
    // 100% the frame slip drops under target hands full throttle to wheels that have only just stopped
    // spinning, which starts the next slip — the cycling that makes bad traction control worse than none.
    const float cap_min = static_cast<float>(cfg_->cap_min_pct) * 0.1f;
    pi_.kp = static_cast<float>(cfg_->kp) * 0.01f;
    pi_.ki = static_cast<float>(cfg_->ki) * 0.01f;
    pi_.out_min = 0.0f;
    pi_.out_max = std::max(0.0f, 100.0f - cap_min);
    if (err > 0.0f) {
        reduction_ = pi_.step(err, dt_s);
    } else {
        reduction_ = std::max(0.0f, reduction_ - static_cast<float>(cfg_->release_pct_s) * dt_s);
        pi_.integ  = reduction_;                     // resume from where the release left it, not from 0
    }
    cap_ = std::clamp(100.0f - reduction_, cap_min, 100.0f);
    bus.set(SIG_TRACTION_CAP, cap_, true, now, ttl());
    bus.set(SIG_TRACTION_RETARD, retard_, true, now, ttl());
    bus.set(SIG_TRACTION_CUT_PCT, cut_pct_, true, now, ttl());

    // --- the cut ----------------------------------------------------------------------------------
    // A PERCENTAGE DELIVERED OVER TIME. ign_cut and fuel_cut are booleans — all cylinders or none — so a
    // duty is the only percentage available without a second cut-mask producer in the scheduler. The
    // accumulator spreads the cut events evenly across frames instead of holding the cut on in bursts,
    // which is what a bare threshold does (RevLimiter's "soft" cut is `pct > 0.5`, and is not soft).
    cutting_ = cut_duty_.step(cut_pct_);
    if (cutting_) {
        // frame_ms(), NOT ttl(). A cut is released by its signal expiring, and ttl() is the period plus
        // grace — so a one-frame cut published with it would still be live on the next frame and every
        // duty would come out roughly double. See CutDuty.h.
        const uint32_t pulse = frame_ms();
        if (cfg_->cut_method == CutFuel || cfg_->cut_method == CutBoth)
            bus.set_bool(wk::fuel_cut, true, now, pulse);
        if (cfg_->cut_method == CutIgnition || cfg_->cut_method == CutBoth)
            bus.set_bool(wk::ign_cut, true, now, pulse);
    }

    // --- are the two speed sources telling the same story ------------------------------------------
    // Steady slip at a cruise, with the engine doing nothing about it, is not wheelspin: it is the
    // driven and reference sources disagreeing — a tyre size, or a pulses/km that was captured at a
    // different moment from its neighbour's. It biases every measurement above, silently, which is why
    // it gets a code rather than being left for someone to notice.
    if (reference >= kCalMinSpeed && reduction_ <= 0.0f && cut_pct_ <= 0.0f && retard_ <= 0.0f) {
        const float a = std::min(1.0f, dt_s * 0.5f);            // ~2 s time constant
        cal_ema_ += (slip_ - cal_ema_) * a;
        if (std::fabs(cal_ema_) > kCalWarnPct) {
            cal_bad_ms_ += static_cast<uint32_t>(dt_s * 1000.0f);
            if (cal_bad_ms_ >= kCalHoldMs && dtc_) {
                dtc_->raise(ModuleDtc::TRACTION_AXLE_CAL, DtcSource::MODULE,
                            ModuleDtc::TRACTION_AXLE_CAL_SEV, now, dtc_ttl());
                cal_raised_ = true;
            }
        } else if (std::fabs(cal_ema_) < kCalHealPct) {
            cal_bad_ms_ = 0;
            if (cal_raised_ && dtc_) { dtc_->heal(ModuleDtc::TRACTION_AXLE_CAL); cal_raised_ = false; }
        }
    }
}
