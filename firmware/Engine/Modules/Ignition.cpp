#include "Ignition.h"
#include "well_known_signals.h"   // wk:: rename-safe signal roles
#include "../TelemScale.h"
#include "../TableEval.h"          // tbl::table_eval — one channel-driven path for every table
#include "../../../generated/table_descs.h"   // <table>_desc(cfg) builders
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../../generated/ecu_config.h"   // g_config.engine.cylinder_count (live-read, like OutputManager)
#include "../../Platform/platform_hal.h"     // platform_get_tick_ms (publish TTL timestamp)
#include <algorithm>
#include <cmath>

void Ignition::init(const IgnitionConfig& cfg) {
    cfg_ = &cfg;
}

void Ignition::on_config_change(const IgnitionConfig& cfg) {
    cfg_ = &cfg;
}

void Ignition::on_engine_stop() {
    knock_retard_deg_ = 0.0f;
}

void Ignition::update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) {
    const uint32_t now = platform_get_tick_ms();   // real tick for every fresh publish (ttl())
    bus.set(wk::knock_retard, knock_retard_deg_, true, now, ttl());    // applied last frame by Knock
    if (!cfg_) {
        bus.set(wk::advance, 0.0f, true, now, ttl());
        bus.set(wk::dwell,   3000.0f, true, now, ttl());
        return;
    }

    bus.set(wk::rpm, pos.rpm, true, now, ttl());   // freshest rpm for the rpm-indexed ign table (order-independent)

    // --- Base advance (ign_table: rpm × load) + CLT advance trim (clt_advance: CLT × optional MAP),
    //     both via the one channel-driven table path. Tables store deg ×10 (descriptor scale 0.1). ---
    // WHICH BASE. While cranking, the advance map has nothing useful to say: the engine is turning at
    // 200 rpm and reads whatever the map's lowest RPM bin holds, which was tuned for running. The
    // cranking table REPLACES it — and replaces the corrections with it, because the coolant advance
    // correction is indexed on the same coolant the cranking table is, and applying both would count
    // that once too often. The cell IS the advance while cranking. Retards still subtract below:
    // protection does not stop mattering because the engine has not caught yet.
    const auto engine_state = static_cast<EngineRunState>(
        static_cast<int>(bus.get(wk::engine_state, 0.0f)));
    const bool cranking_base = cfg_->cranking_ign_enable != 0
                            && engine_state == EngineRunState::CRANKING;
    // AND WHICH BASE WHILE LAUNCHING. Launch's ignition map is the ACTUAL advance for the whole time
    // launch control is armed, not a correction on this one — so it replaces the map here on the same
    // footing as the cranking table, and for the same reason: two absolute answers to one question
    // cannot be added together. Cranking wins if both are somehow true; an engine that has not caught
    // is not launching. Launch publishes the angle at its own cadence, 0 when it is not holding.
    const bool launch_base = !cranking_base && bus.valid(wk::launch_active)
                          && bus.get_bool(wk::launch_active);

    const float base_adv = cranking_base ? tbl::table_eval(cranking_ign_table_desc(cfg_), bus)
                         : launch_base   ? bus.get(wk::launch_ign_adv, 0.0f)
                                         : tbl::table_eval(ign_table_desc(cfg_), bus);
    const float rev_corr = (cranking_base || launch_base || !cfg_->enable_revlimit)
                             ? 0.0f
                             : tbl::table_eval(rpmlimit_ign_corr_table_desc(cfg_), bus);
    // The two FAST terms, published so the chain can be read end to end: base, then every correction,
    // then the final commanded angle. The slow nine are published by IgnitionTrim from the same values
    // it sums, and the retards each already have a channel. ign_base_kind says WHICH table the base
    // came from, so a breakdown page cannot name the map while the engine fires on the other one.
    bus.set(SIG_IGN_BASE_ADV,      base_adv, true, now, ttl());
    bus.set(SIG_IGN_CORR_REVLIMIT, rev_corr, true, now, ttl());
    bus.set(SIG_IGN_BASE_KIND,     cranking_base ? 1.0f : launch_base ? 2.0f : 0.0f, true, now, ttl());
    float adv = base_adv + rev_corr;
    // The SLOW corrections (clt / iat / fuelcomp / gear / gen1..4 / post-start) are computed off the
    // per-cycle path by IgnitionTrim (1 table/frame, round-robin) and summed here as one bus read.
    if (!cranking_base && !launch_base) {
        adv += bus.get(wk::ign_advance_trim, 0.0f);
        adv += bus.get(wk::ign_corr_transient, 0.0f);                       // transient-throttle spark trim (TransientThrottle, 1 kHz)
        adv += bus.get(wk::idle_ign_corr, 0.0f);                            // idle stabiliser (Idle publishes)
        adv += cfg_->overall_adv_trim * 0.1f;                               // overall ign trim (global scalar, cheap)
    } else if (launch_base) {
        // THE LAUNCH MAP IS ABSOLUTE — BUT NOT PAST THE PROTECTION. The coolant and intake-air corrections
        // were skipped with every other trim for as long as launch was armed, and the IAT one's own help
        // calls it protection: a heat-soaked car sitting on the line is exactly where it is needed. Their
        // RETARDS still apply; an advancing trim does not, which is what makes the launch map absolute.
        adv += std::min(0.0f, bus.get(SIG_IGN_CORR_CLT, 0.0f));
        adv += std::min(0.0f, bus.get(SIG_IGN_CORR_IAT, 0.0f));
    }

    // E-2 (ign→per-cycle): Ignition runs on the PER_CYCLE cadence, so its cross-module inputs come from
    // the bus (the 1 kHz frame is not shared): knock from our own member, protection retard from
    // EngineProtection's wk::prot_ign_retard, ign-cut from RevLimiter's wk::ign_cut.
    // Sum every graded retard pulling timing below the base map — published as spark_retard_total
    // (tuner-visible "total timing pulled") and used by TorqueModel as the spark-efficiency index.
    // TRACTION IS IN HERE BECAUSE THIS IS THE FAST PATH. Retard reaches the crank on the very next
    // spark, where a throttle plate takes tenths of a second to move air — so a wheel that has already
    // let go is caught here, and the throttle ceiling is what holds it afterwards.
    const float total_retard = knock_retard_deg_
                             + bus.get(wk::prot_ign_retard, 0.0f)   // DTC protection-level reaction
                             + bus.get(SIG_NITROUS_RETARD,  0.0f)   // nitrous (0 when inactive)
                             + bus.get(SIG_ANTILAG_RETARD,  0.0f)   // anti-lag (0 when inactive)
                             + bus.get(SIG_TRACTION_RETARD, 0.0f);  // wheelspin — the FAST path, see below
    adv -= total_retard;
    bus.set(SIG_SPARK_RETARD_TOTAL, total_retard, true, now, ttl());   // graded retards (the breakdown chain)

    const float min_adv = cfg_->min_adv_deg / 10.0f;
    const float max_adv = cfg_->max_adv_deg / 10.0f;
    adv = std::max(min_adv, std::min(max_adv, adv));

    // TIMING BELOW THE MAIN MAP, from the angle actually commanded — for the TORQUE MODEL. It indexed its
    // spark efficiency on spark_retard_total, the sum of the graded retards (which the breakdown page
    // needs exactly as it is), and that sum misses a launch or cranking base far below the map, negative
    // trims and the clamp — so torque was overstated during launch.
    // The main map is read here even when another base is in force, so "below the map" means the same
    // thing whichever base is firing.
    {
        const float map_adv = (cranking_base || launch_base) ? tbl::table_eval(ign_table_desc(cfg_), bus) : base_adv;
        bus.set(SIG_SPARK_BELOW_MAP, std::max(0.0f, map_adv - adv), true, now, ttl());
    }

    // FIXED TIMING. Hold one advance so a timing light reads a number the tuner chose — the only way
    // to check trigger_offset_btdc against a real engine. It lands AFTER the clamp and every trim so
    // nothing can move it: a correction drifting underneath is precisely what makes a light
    // unreadable. Fixed means fixed: no corrections of any kind are applied.
    const bool fixed_timing = cfg_->fixed_timing_enable != 0;
    if (fixed_timing) adv = cfg_->fixed_timing_deg / 10.0f;

    // No ign-cut handling here on purpose. A cut suppresses the COIL (EnginePositionHal::
    // set_output_cuts clears the channel's execution bit, so the dwell never starts); it does not
    // change the timing. This line used to read `if (bus.valid(wk::ign_cut)) adv = 0.0f`, which is
    // not "no spark" — it is a spark at TDC, on every plug, every time the rev limiter came in.

    frame.ign_advance_deg = adv;
    bus.set(wk::advance, adv, true, now, ttl());                        // canonical degrees — pack scales to wire

    // DWELL FOLLOWS THE SUPPLY. A coil charges to a CURRENT, and the current it draws follows the voltage
    // across it, so one fixed dwell is only right at one battery voltage: short (weak spark) while
    // cranking, long (coil heat, current wasted) on a charging system at 15 V. The table's X axis is
    // battery voltage by default; with its axes switched off it collapses to a single cell, which is
    // exactly the fixed number this used to read.
    const float dwell_ms = tbl::table_eval(dwell_table_desc(cfg_), bus);
    const uint16_t dwell_us = static_cast<uint16_t>(dwell_ms * 1000.0f + 0.5f);
    bus.set(wk::dwell,   static_cast<float>(dwell_us), true, now, ttl());
    frame.dwell_us        = dwell_us;

    // Per-cylinder ignition trim: 12 separate tables (one per cylinder). Additive deg, default 0.
    using CylDescFn = tbl::TableDesc (*)(const IgnitionConfig*);
    static const CylDescFn CYL_IGN[MAX_CYLINDERS] = {
        cyl1_ign_corr_table_desc,  cyl2_ign_corr_table_desc,  cyl3_ign_corr_table_desc,
        cyl4_ign_corr_table_desc,  cyl5_ign_corr_table_desc,  cyl6_ign_corr_table_desc,
        cyl7_ign_corr_table_desc,  cyl8_ign_corr_table_desc,  cyl9_ign_corr_table_desc,
        cyl10_ign_corr_table_desc, cyl11_ign_corr_table_desc, cyl12_ign_corr_table_desc };
    // Only the cylinders the engine actually has — don't evaluate trim tables for phantom cylinders.
    // The scheduler fires by the same configured count, so cyl[>=ncyl] are never used downstream.
    // Rotary leading/trailing split (deg of RETARD) over the same rpm x load surface as the timing
    // table. One eval per update — it does not vary by cylinder.
    const AngleDeg10 trail_split_x10 =
        static_cast<AngleDeg10>(tbl::table_eval(trail_split_table_desc(cfg_), bus) * 10.0f);

    const uint8_t ncyl = std::min<uint8_t>(g_config.engine.cylinder_count, MAX_CYLINDERS);
    for (uint8_t i = 0; i < ncyl; i++) {
        // Per-cylinder trim is a correction too, so fixed timing skips it — otherwise the cylinders
        // disagree and the light shows one of them rather than the number that was asked for.
        const float cyl_adv = fixed_timing ? 0.0f : tbl::table_eval(CYL_IGN[i](cfg_), bus);
        // THE CLAMP HOLDS PER CYLINDER. Max/Min Advance are the last word on "every table and correction
        // summed", and the per-cylinder trim is one of those corrections — it used to be added after the
        // clamp, so a +60° cell (the table's range) took one cylinder straight past the backstop.
        // Fixed timing is exempt: it is deliberately outside every limit and correction.
        // PER-CYLINDER KNOCK. The global advance carries the WORST cylinder's knock retard (so the
        // breakdown chain and "total timing pulled" still add up); a cylinder that has not knocked gets
        // back the difference. One noisy cylinder used to retard all of them.
        const float knock_relief = knock_retard_deg_ - knock_cyl_[i];
        const float cyl_total = fixed_timing ? adv
                              : std::max(min_adv, std::min(max_adv, adv + cyl_adv + knock_relief));
        frame.cyl[i].spark_btdc_x10 = static_cast<AngleDeg10>(cyl_total * 10.0f);
        // Rotary trailing plug. Both plugs fire the same chamber, so the trailing one has no advance
        // of its own — it is the leading angle RETARDED by the split, and the split is positive by
        // convention ("degrees of Retard"), hence the subtraction. Evaluated once above, outside
        // this per-cylinder loop: the split is a property of the operating point, not the cylinder.
        // The scheduler ignores this unless the engine cycle is ROTARY.
        frame.cyl[i].trail_btdc_x10 =
            static_cast<AngleDeg10>(frame.cyl[i].spark_btdc_x10 - trail_split_x10);
        frame.cyl[i].dwell_us       = dwell_us;   // the same charge time for every coil this cycle
    }
}

void Ignition::apply_knock_retard(float retard_deg) {
    knock_retard_deg_ = retard_deg;                   // published next update() (it has the bus)
    for (float& c : knock_cyl_) c = retard_deg;       // the whole engine alike
}

void Ignition::apply_knock_retard(const float* per_cyl, uint8_t n) {
    float worst = 0.0f;
    for (uint8_t i = 0; i < MAX_CYLINDERS; ++i) {
        knock_cyl_[i] = (per_cyl && i < n) ? per_cyl[i] : 0.0f;
        worst = std::max(worst, knock_cyl_[i]);
    }
    knock_retard_deg_ = worst;
}
