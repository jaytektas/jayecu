#include "Launch.h"
#include "well_known_signals.h"   // wk:: rename-safe signal roles
#include "../../Diagnostics/DtcManager.h"
#include "../../../generated/module_dtc.h"
#include "../../Diagnostics/Dtc.h"
#include "../../Platform/platform_hal.h"       // platform_get_tick_ms
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../../../generated/signal_ids.h"  // SIG_PEDAL_DEMAND
#include "../../Signal/Expr.h"                 // expr::exec — "Arm When" is a condition, not a switch
#include "../EngineFrame.h"
#include "../TableEval.h"                      // tbl::table_eval — the limit and both maps are tables
#include "../../../generated/table_descs.h"
#include "../../../generated/table_registry.h" // expr_table_* — the tables a condition may read
#include "../../../generated/ecu_config.h"     // g_config (live tune RAM)
#include <algorithm>

extern volatile uint32_t g_config_generation;  // bumped by every config write (CommsManager.cpp)

void Launch::reset() {
    fuel_ = Channel{};
    ign_  = Channel{};
    armed_prev_ = false;
    timed_out_  = false;
    armed_ms_   = 0;
}

void Launch::init(const LaunchConfig& cfg) {
    cfg_ = &cfg;
    reset();
}

void Launch::on_config_change(const LaunchConfig& cfg) {
    cfg_ = &cfg;
    reset();                    // a retune shouldn't carry a latched cut or a running timeout
}

void Launch::on_engine_stop() {
    reset();
}

void Launch::revalidate() {
    // exec() assumes validate() has passed (Expr.h), and on_config_change is dead API for this, so the
    // module watches the generation counter itself — the ScriptEngine/Sensors/TractionControl pattern.
    expr_bad_ = cfg_ && !expr::is_empty(cfg_->arm_expr, sizeof cfg_->arm_expr) &&
                expr::validate(cfg_->arm_expr, sizeof cfg_->arm_expr,
                               sizeof(g_config), EXPR_TABLE_COUNT) != expr::Invalid::None;
}

void Launch::publish_idle(SignalBus& bus, uint32_t now) {
    fuel_ = Channel{};
    ign_  = Channel{};
    bus.set_bool(wk::launch_active, false, now, ttl());
    bus.set(wk::launch_cut_pct,   0.0f, true, now, ttl());
    bus.set(wk::launch_end_rpm,   0.0f, true, now, ttl());
    // NEUTRAL, NOT ABSENT. Ignition reads launch_ign_adv only while launch_active, and FuelCalculator
    // defaults an absent correction to 1.0 — but publishing the neutral value anyway keeps the channels
    // readable on a gauge and means a log shows the launch chain resting rather than going blank.
    bus.set(wk::launch_ign_adv,   0.0f, true, now, ttl());
    bus.set(wk::fuel_corr_launch, 1.0f, true, now, ttl());
}

// One cut channel at its own threshold. Hard: latch at the threshold, release a resume band below it.
// Soft: a duty rising from nothing at (threshold − range) to everything at the threshold, spread frame
// by frame by CutDuty — the difference between a limiter that buzzes and one that bounces.
float Launch::cut_channel(float rpm, float threshold, Channel& ch, bool& fire) const {
    if (cfg_->cut_type != SoftCut) {
        const float resume = threshold - static_cast<float>(cfg_->resume_band_rpm);
        if (ch.latched && rpm < resume) ch.latched = false;
        if (rpm >= threshold)           ch.latched = true;
        ch.duty.reset();
        fire = ch.latched;
        return ch.latched ? 100.0f : 0.0f;
    }
    ch.latched = false;
    const float range = static_cast<float>(cfg_->cut_range_rpm);
    const float pct   = (range <= 0.0f) ? ((rpm >= threshold) ? 100.0f : 0.0f)
                                        : std::clamp((rpm - (threshold - range)) / range * 100.0f,
                                                     0.0f, 100.0f);
    fire = ch.duty.step(pct);
    return pct;
}

void Launch::update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) {
    const uint32_t now = platform_get_tick_ms();
    if (g_config_generation != cfg_gen_seen_) { cfg_gen_seen_ = g_config_generation; revalidate(); }

    if (!cfg_ || !cfg_->enabled) {
        publish_idle(bus, now);
        armed_prev_ = false;
        timed_out_  = false;
        if (dtc_) dtc_->heal(ModuleDtc::LAUNCH_EXPR);
        return;
    }

    // A CONDITION THAT CANNOT BE ASKED IS NOT A CONDITION THAT IS FALSE. A program that failed to
    // compile leaves launch control off and says so with a code, rather than reading as "the driver is
    // not launching" — which is silence on a car whose two-step has quietly stopped existing.
    if (expr_bad_) {
        if (dtc_) dtc_->raise(ModuleDtc::LAUNCH_EXPR, DtcSource::MODULE,
                              ModuleDtc::LAUNCH_EXPR_SEV, now, dtc_ttl());
        publish_idle(bus, now);
        armed_prev_ = false;
        return;
    }
    if (dtc_) dtc_->heal(ModuleDtc::LAUNCH_EXPR);

    // EMPTY MEANS THE SAFE BUILT-IN RULE, not "always". "Armed whenever enabled" put the launch timing,
    // fuel and rev limit in charge from every engine start until the timeout — the first half-minute of
    // every drive on the launch maps — and then, the condition never going false, left launch dead for the
    // rest of the run. The built-in rule is the one nearly every launch uses: the car is STANDING STILL.
    // No road speed to ask means not armed; anything more (a button, the clutch, coolant) is what Arm When
    // is for.
    //
    // …AND THE THROTTLE OPEN. Standing still on its own armed launch at every set of traffic lights, and
    // while it is armed Ignition runs the launch map in place of the main one and drops the idle
    // ignition correction — so every stop idled on the launch calibration with the idle stabiliser off,
    // until the timeout. A launch is the car held still WITH the throttle open; idling is the car held
    // still with it closed. The pedal request where there is a drive-by-wire pedal (the plate is the
    // ECU's to move), the throttle position otherwise; neither readable means not armed.
    constexpr float kArmThrottlePct = 20.0f;
    const SignalId thr = bus.valid(SIG_PEDAL_DEMAND) ? SIG_PEDAL_DEMAND : static_cast<SignalId>(wk::tps);
    bool armed = bus.valid(wk::vehicle_spd) && bus.get(wk::vehicle_spd, 999.0f) < 5.0f
              && bus.valid(thr) && bus.get(thr, 0.0f) > kArmThrottlePct;
    if (!expr::is_empty(cfg_->arm_expr, sizeof cfg_->arm_expr)) {
        expr::Ctx ex;
        ex.bus = &bus; ex.cfg = reinterpret_cast<const uint8_t*>(&g_config);
        ex.cfg_size = sizeof(g_config); ex.now_ms = now;
        ex.table = expr_table_value; ex.curve = expr_table_at;
        ex.curve_user = &bus; ex.curve_count = EXPR_TABLE_COUNT;
        armed = expr::exec(cfg_->arm_expr, sizeof cfg_->arm_expr, ex).truthy();
    }

    // --- The timeout. Measured from the moment the condition went true, and once it has expired the
    //     launch stays dead until the condition goes false and true again: something has to end a
    //     launch held against a stuck switch, and a timeout that re-armed itself would not. ---
    if (armed && !armed_prev_) { armed_ms_ = now; timed_out_ = false; }
    if (!armed)                  timed_out_ = false;
    armed_prev_ = armed;
    if (armed && cfg_->timeout_s != 0 && !timed_out_ &&
        (now - armed_ms_) >= static_cast<uint32_t>(cfg_->timeout_s) * 1000u)
        timed_out_ = true;
    if (timed_out_) armed = false;

    if (!armed) {
        publish_idle(bus, now);
        return;
    }

    // --- ACTIVE. The launch maps are the engine's calibration from here until it disarms. ---
    bus.set_bool(wk::launch_active, true, now, ttl());
    bus.set(wk::launch_ign_adv, tbl::table_eval(ign_advance_table_desc(cfg_), bus), true, now, ttl());
    bus.set(wk::fuel_corr_launch,
            1.0f + tbl::table_eval(fuel_corr_table_desc(cfg_), bus) / 100.0f, true, now, ttl());

    // The limit itself. Published BEFORE anything reads it, on the same rule the boost target follows:
    // a channel a table axis may name has to exist before the tables are evaluated against it.
    const float end_rpm = tbl::table_eval(end_rpm_table_desc(cfg_), bus);
    bus.set(wk::launch_end_rpm, end_rpm, true, now, ttl());

    // WHICH CUT, AND WHERE. Fuel-only and ignition-only are one cut at the End RPM. Both is two, and
    // the adder puts the follower that many RPM higher — so the leading cut catches the rise on its own
    // and the second only joins if the engine is still climbing past it.
    const uint8_t method = cfg_->cut_method;
    const bool use_fuel  = (method == CutFuel || method == CutBoth);
    const bool use_ign   = (method == CutIgnition || method == CutBoth);
    float fuel_thr = end_rpm;
    float ign_thr  = end_rpm;
    if (method == CutBoth && cfg_->cut_adder_rpm != 0) {
        const float follow = end_rpm + static_cast<float>(cfg_->cut_adder_rpm);
        if (cfg_->cut_lead == LeadIgnition) fuel_thr = follow;
        else                                ign_thr  = follow;
    }

    float worst = 0.0f;
    bool  fire_fuel = false, fire_ign = false;
    if (use_fuel) worst = std::max(worst, cut_channel(pos.rpm, fuel_thr, fuel_, fire_fuel));
    else          fuel_ = Channel{};
    if (use_ign)  worst = std::max(worst, cut_channel(pos.rpm, ign_thr,  ign_,  fire_ign));
    else          ign_  = Channel{};

    // A CUT ENDS WHEN ITS SIGNAL EXPIRES (requesters publish only `true`; a `false` from this one would
    // overwrite a cut another requester holds). A hard cut is renewed every frame and released by
    // the ttl; a soft cut's frame is one frame wide, so it publishes with frame_ms() — ttl() is period
    // PLUS grace, which would hold each cut into the next frame and double every duty.
    const uint32_t pulse = (cfg_->cut_type == SoftCut) ? frame_ms() : ttl();
    if (fire_fuel) bus.set_bool(wk::fuel_cut, true, now, pulse);
    if (fire_ign)  bus.set_bool(wk::ign_cut,  true, now, pulse);
    bus.set(wk::launch_cut_pct, worst, true, now, ttl());

    // The hold defines the effective limit if it is the tightest one this frame — what the rev-limit
    // approach trim and the dash read as "the limiter in charge".
    if (worst > 0.0f && (frame.effective_rpm_limit == 0.0f || end_rpm < frame.effective_rpm_limit))
        frame.effective_rpm_limit = end_rpm;
}
