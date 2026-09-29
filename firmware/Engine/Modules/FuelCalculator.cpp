#include "FuelCalculator.h"
#include "well_known_signals.h"   // wk:: roles — rename-safe signal bindings
#include "../../Diagnostics/DtcManager.h"
#include "../../../generated/module_dtc.h"
#include "../../Diagnostics/Dtc.h"
#include "../EngineStateMachine.h"          // EngineRunState — cranking-vs-running gating
#include "../../Platform/platform_hal.h"   // platform_get_tick_ms (wall-film dt)
#include "../TelemScale.h"
#include "../../Scheduler/SchedulerTypes.h"   // engine_cycle_angle / FACES_PER_ROTOR — ONE cycle definition
#include "../TableEval.h"          // tbl::table_eval — one channel-driven path for every table
#include "../../../generated/table_descs.h"   // <table>_desc(cfg) builders
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../../generated/ecu_config.h"   // g_config.engine.cylinder_count (live-read, like Ignition)
#include <algorithm>
#include <cmath>


void FuelCalculator::init(const FuelCalculatorConfig& cfg) {
    cfg_ = &cfg;
}

void FuelCalculator::on_config_change(const FuelCalculatorConfig& cfg) {
    cfg_ = &cfg;
}

void FuelCalculator::on_engine_start() {
    // Engine just caught (cranking -> running). Anchor the post-start run-time clock HERE so the
    // post-start enrichment table decays from the real start. Dispatched on the per-cycle task by
    // EngineTask, before this module's update() runs in the same wake.
    run_start_ms_ = platform_get_tick_ms();
    running_      = true;
}

void FuelCalculator::on_engine_stop() {
    for (float& f : film_pw_) f = 0.0f;   // clear the fuel-film model; the next start re-anchors post-start via on_engine_start()
    running_ = false;  // and run time goes back to 0 — a stopped engine has not been running for a while
    // outputs decay on the bus via the normal publish path; nothing else to zero here
}

void FuelCalculator::update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) {
    // Read sensor signals from the bus using configured source slots.
    const uint32_t now = platform_get_tick_ms();   // real tick for every fresh publish (ttl())
    const SignalId map_id = cfg_ ? static_cast<SignalId>(cfg_->map_src) : wk::map;
    const SignalId clt_id = cfg_ ? static_cast<SignalId>(cfg_->clt_src) : wk::clt;

    // ATMOSPHERE, RESOLVED ONCE. The sensor while it is publishing, the tuner's stated assumption when
    // it is not — so a car with no baro sensor is fuelled for the altitude it says it lives at rather
    // than for a hardcoded sea level. Published when nothing else is publishing it, because the
    // barometric CORRECTION table indexes on this channel and was reading an absent one as zero.
    const SignalId baro_id  = static_cast<SignalId>(cfg_->baro_src);
    const bool     baro_ok  = (baro_id != SIG_NONE) && bus.valid(baro_id);
    const float    baro_kpa = baro_ok ? bus.get(baro_id) : (cfg_->baro_assumed_kpa * 0.1f);
    if (!baro_ok) bus.set(SIG_BARO_KPA, baro_kpa, true, now, ttl());

    // A MISSING MANIFOLD READS AS ATMOSPHERE, not as zero. This defaulted to 0.0 kPa, which is a hard
    // vacuum: on Alpha-N — where MAP is optional by design — the injector differential came out a whole
    // bar high and the flow table was read in the wrong place entirely.
    const bool  map_ok    = (map_id != SIG_NONE) && bus.valid(map_id);
    const float map_meas  = map_ok ? bus.get(map_id) : baro_kpa;
    float       map_kpa   = map_meas;     // the ESTIMATE: measured, unless prediction is standing in
    const float clt_c   = bus.get(clt_id, 20.0f);


    if (!cfg_) {
        bus.set(wk::base_pw, 0.0f, true, now, ttl());
        return;
    }

    // THE ETHANOL CONTENT FUELLING USES, resolved before any table reads it (the VE ethanol plane, the
    // specific-gravity and fuel-composition tables all index on flex_ethanol). A dead sensor used to read
    // as 0 % — fuelling an E85 engine as petrol, a third lean at every operating point, with nothing said.
    // Now: the sensor while it is valid; the LAST GOOD reading if it drops out (the tank does not change
    // mid-drive); the configured fallback if it has never read since power-up. The sensor's own P0177-79
    // say why; this makes sure the engine is fuelled sanely while they do.
    //
    // PER STAGE. Each stage's fuel is its own (FuelCalculatorConfig stageN_*): a stage with Flex Fuel OFF
    // runs a fixed blend — its Ethanol % — and one with it ON runs the rule above on its own sensor, which
    // may be the same sensor another stage reads. Turning a stage's flex off forgets its last good
    // reading, so turning it back on starts from the fallback rather than from a stale blend.
    {
        struct StageFuel { uint8_t flex; int16_t src; uint8_t pct; };
        const StageFuel sf[kStages] = {
            { cfg_->stage1_flex_enabled, cfg_->stage1_ethanol_src, cfg_->stage1_ethanol_pct },
            { cfg_->stage2_flex_enabled, cfg_->stage2_ethanol_src, cfg_->stage2_ethanol_pct },
            { cfg_->stage3_flex_enabled, cfg_->stage3_ethanol_src, cfg_->stage3_ethanol_pct },
            { cfg_->stage4_flex_enabled, cfg_->stage4_ethanol_src, cfg_->stage4_ethanol_pct },
        };
        static const SignalId ETH_SIG[kStages] = {
            SIG_STAGE1_ETHANOL, SIG_STAGE2_ETHANOL, SIG_STAGE3_ETHANOL, SIG_STAGE4_ETHANOL };
        for (int s = 0; s < kStages; ++s) {
            const SignalId eth_id = static_cast<SignalId>(sf[s].src);   // -1 in the tune = SIG_NONE
            if (!sf[s].flex) {
                ethanol_pct_[s]  = static_cast<float>(sf[s].pct);
                ethanol_seen_[s] = false;
            } else if (eth_id != SIG_NONE && bus.valid(eth_id)) {
                ethanol_pct_[s]  = std::clamp(bus.get(eth_id), 0.0f, 100.0f);
                ethanol_seen_[s] = true;
            } else if (!ethanol_seen_[s]) {
                ethanol_pct_[s]  = static_cast<float>(sf[s].pct);
            }                                   // else: hold the last good reading
            bus.set(ETH_SIG[s], ethanol_pct_[s], true, now, ttl());
        }
        // The CHARGE: last cycle's mass-weighted blend once staging has run, else stage 1's own — which
        // is exact for a one-stage engine, where the blend IS stage 1.
        const bool staged = std::clamp<uint8_t>(g_config.engine.num_inj_stages, 1, kStages) > 1;
        bus.set(SIG_FLEX_ETHANOL, (staged && charge_known_) ? charge_ethanol_ : ethanol_pct_[0],
                true, now, ttl());
    }

    // E-2b: FuelCalculator runs on the PER_CYCLE cadence — once per engine cycle (or the 50 ms
    // timeout while stopped) — so the heavy charge/VE/correction math below executes exactly when
    // it should. The Stage-E software self-decimation (cache + cycle-count gate) is gone; the
    // task cadence IS the decimation. fuel_cut comes from the bus (wk::fuel_cut, published by the
    // 1 kHz RevLimiter) since this module no longer shares RevLimiter's frame.

    // --- Model-selectable air estimation behind one air_mass (Stage F) + the MAP-PREDICTION blend
    //     (fuel_model=3). The published fuel-load signal (wk::fuel_load) is the per-model load that
    //     the VE + lambda tables index on; air mass = MAF airflow (model 2) or VE × charge density.
    //     The blend is the clean ITB path: ONE MAP-indexed VE table fed by an "effective MAP" that
    //     crossfades from PREDICTED MAP (predicted_map_table, rpm×TPS — what the manifold would read
    //     at this throttle) up to MEASURED MAP as RPM rises. So Alpha-N-down-low → SD-up-top with a
    //     single VE table. T_charge / cyl volume are shared. ---
    // Publish rpm so the rpm-indexed tables (VE/lambda/predicted/ign via table_eval) read the freshest
    // value off the bus regardless of 1 kHz-task ordering — the position rpm is the authoritative source.
    bus.set(wk::rpm, pos.rpm, true, now, ttl());
    const float tps_pct = bus.get(static_cast<SignalId>(cfg_->tps_src), 0.0f);

    // --- MAP PREDICTION -----------------------------------------------------------------------------
    // A MAP reading is averaged over at least a cylinder period — the manifold breathes with every
    // intake stroke — so it is necessarily late. Snap the throttle open at idle and the plenum fills in
    // milliseconds while the averaged signal still reports vacuum, and the fuel for those cycles is
    // computed from a number that is out of date. So while the throttle is MOVING, prefer the predicted
    // table; when it settles, hand back to the sensor.
    //
    // The trigger is the throttle's RATE against the scaling table, not its position: what makes the
    // sensor wrong is the change, and how wrong depends on how fast. At or above the table's rate the
    // estimate IS the predicted MAP; below it, rate / table of the way from measured to predicted.
    // Under a tenth of the table's rate is NOT a movement: the filtered rate decays towards zero without
    // reaching it, and a still pedal's noise sits there too (the table is set about ten times it) — both
    // would otherwise re-arm the hold for ever, and closed-loop O2 holds while prediction is active.
    // Prediction takes the HIGHER of the two, never the lower — this exists to cover a lean hole, and a
    // predicted value below the measured one would only ever make one.
    const float predicted_map = tbl::table_eval(predicted_map_table_desc(cfg_), bus);
    {
        const float dt_s = (tps_last_ms_ != 0) ? (now - tps_last_ms_) / 1000.0f : 0.0f;
        tps_last_ms_ = now;
        if (dt_s > 0.0f && dt_s < 0.5f) {
            const float raw = (tps_pct - tps_last_) / dt_s;
            tps_rate_ = tps_rate_ + 0.5f * (raw - tps_rate_);   // same light filter the transient uses
        } else if (dt_s >= 0.5f) {
            tps_rate_ = 0.0f;                                    // a long gap is not a throttle movement
        }
        tps_last_ = tps_pct;
    }
    bus.set(SIG_TPS_RATE, tps_rate_, true, now, ttl());
    bus.set(SIG_MAP_PREDICTED, predicted_map, true, now, ttl());

    uint8_t map_source = 0;                                      // 0 measured, 1 predicted, 2 failover
    if (cfg_->map_predict_enabled) {
        const float thr = tbl::table_eval(map_predict_scale_table_desc(cfg_), bus);
        // A LIFT is predicted only when asked for (map_predict_tipout): the rate's size is compared the
        // same way, and the estimate goes DOWN towards the table — the lower of the two, as a tip-in
        // takes the higher. Off, a closing throttle is left to the sensor.
        const bool  down = cfg_->map_predict_tipout && tps_rate_ < 0.0f;
        const float rate = down ? -tps_rate_ : tps_rate_;
        if (thr > 0.0f && rate > 0.0f) {
            const float ratio = rate / thr;
            const float wt    = ratio >= 0.1f ? std::min(ratio, 1.0f) : 0.0f;
            // A stronger movement re-arms the hold and raises the weight; a weaker one during an
            // existing hold does not cut it short — the sensor still has not caught up.
            if (wt > 0.0f && (wt >= predict_wt_ || static_cast<int32_t>(now - predict_until_ms_) >= 0)) {
                predict_wt_       = wt;
                predict_until_ms_ = now + cfg_->map_predict_hold_ms;
                predict_down_     = down;
            }
        }
        if (static_cast<int32_t>(now - predict_until_ms_) < 0 && predict_wt_ > 0.0f) {
            const float toward = predict_down_ ? std::min(map_meas, predicted_map)
                                               : std::max(map_meas, predicted_map);
            map_kpa    = map_meas + predict_wt_ * (toward - map_meas);
            map_source = 1;
        } else {
            predict_wt_ = 0.0f;
        }
    }
    // A FAILED MAP IS NOT PREDICTED. The predicted table is for TRANSIENTS — it stands in only while the
    // throttle is moving and only when prediction is switched on — and it is only as good as someone has
    // made it for that job. Used as a failover it drove the fuel model from a table that was
    // switched off and never tuned: a disabled MAP sensor fuelled on an untuned 25 kPa, and every reading
    // (fuel load, MAP est) looked plausible, which hid the fault. A missing MAP now reads as atmosphere
    // (map_meas, above) — rich at idle, which is the safe side — and FUEL_MAP below says why.
    if (cfg_->fuel_model != 1 && map_id != SIG_NONE && !bus.valid(map_id))
        map_source = 2;                                          // failed: reading atmosphere, fault raised
    bus.set(SIG_MAP_EST,    map_kpa, true, now, ttl());
    bus.set(SIG_MAP_SOURCE, static_cast<float>(map_source), true, now, ttl());

    const float iat_c   = bus.get(static_cast<SignalId>(cfg_->iat_src), 20.0f);
    const float w_iat   = cfg_->charge_temp_iat_pct * 0.1f / 100.0f;
    const float t_charge_k = (w_iat * iat_c + (1.0f - w_iat) * clt_c) + 273.15f;   // IAT/CLT charge blend
    bus.set(wk::charge_temp, t_charge_k - 273.15f, true, now, ttl());
    // Per-cylinder swept volume = displacement / cylinders (the CONFIGURED count, the engine's static
    // factor — not the board's max-supported count). This is the volume ONE cylinder ingests per intake
    // event; VE x this x charge density = the air mass trapped per event. The 4-stroke "one induction per
    // 2 revs" is a RATE fact (air per time), so it belongs in the flow rate, NOT this per-event volume —
    // it lives correctly in the MAF model's events_per_s below. (Air per event is stroke-count-agnostic:
    // a cylinder fills its full swept volume every intake stroke regardless.) So NO /2 here, which also
    // makes the SD air mass match the MAF model's per-event mass for the same real VE, and makes VE read
    // physically (100% cell = 100% volumetric efficiency, as in the default table).
    const uint8_t cfg_ncyl = g_config.engine.cylinder_count ? g_config.engine.cylinder_count : 1;
    // Working volume of ONE combustion chamber.
    //
    // For a piston engine that is displacement / cylinders. A ROTARY is quoted differently: a 13B is
    // "1308cc" meaning 2 rotors x 654cc chamber — the nameplate counts ONE chamber per rotor, not
    // all three faces. Since each face is a slot here, dividing by the slot count would give 218cc
    // and run the engine lean by 3x, with nothing to report it.
    const bool  rotary   = (g_config.engine.cycle_type == uint8_t(EngineCycleType::ROTARY));
    const float chambers = rotary ? static_cast<float>(cfg_ncyl) / FACES_PER_ROTOR   // = rotors
                                  : static_cast<float>(cfg_ncyl);
    const float cyl_vol_cc = static_cast<float>(g_config.engine.displacement) / chambers;

    // --- Fuel load: the per-air-model load that the VE + target-lambda tables index on, published as
    //     wk::fuel_load so those tables are channel-driven (the engine reads bus.get(fuel_table_y_src)).
    //     SD -> MAP, Alpha-N -> TPS, Blend -> effective MAP (predicted MAP crossfading to measured by
    //     RPM). density_kpa is the manifold density reference for the gas law (baro for Alpha-N). ---
    const uint8_t model = cfg_->fuel_model;

    // --- Signal validity checks: raise a unique DTC per missing required input.
    //     MAP is only needed if fuel_model != 1 (Alpha-N uses TPS for load, not MAP).
    //     TPS is needed by Alpha-N (1) and Blend (3). MAF is needed only in MAF mode (2).
    //     CLT and IAT are required by ALL models (enrichment + charge-temp).
    if (dtc_) {
        const uint32_t now_ms = platform_get_tick_ms();
        const bool need_map = (model != 1);
        const bool need_tps = (model == 1 || model == 3);
        const bool need_maf = (model == 2);
        // Same split the HBridge uses, so the key-on gate treats each required input correctly:
        //   source UNSET (SIG_NONE)  -> DtcSource::CONFIG @ level 1 -> shown even key-off / engine-stopped, so
        //     a tuner on the bench sees exactly which fuel inputs still need a signal assigned.
        //   configured but INVALID   -> DtcSource::MODULE @ sev  -> key-on gated, so an input that's simply
        //     absent at rest (engine not running, sensor unpowered) doesn't flood the bench table.
        auto report = [&](uint16_t code, bool needed, SignalId src, uint8_t sev) {
            const bool unset   = needed && (src == SIG_NONE);
            const bool invalid = needed && !unset && !bus.valid(src);
            if (unset)        dtc_->raise(code, DtcSource::CONFIG, DTC_SEV_LEVEL1, now_ms, dtc_ttl());
            else if (invalid) dtc_->raise(code, DtcSource::MODULE, sev, now_ms, dtc_ttl());
            else              dtc_->heal(code);
        };
        report(ModuleDtc::FUEL_MAP, need_map, map_id,                               ModuleDtc::FUEL_MAP_SEV);
        report(ModuleDtc::FUEL_TPS, need_tps, static_cast<SignalId>(cfg_->tps_src), ModuleDtc::FUEL_TPS_SEV);
        report(ModuleDtc::FUEL_CLT, true,     clt_id,                               ModuleDtc::FUEL_CLT_SEV);
        report(ModuleDtc::FUEL_IAT, true,     static_cast<SignalId>(cfg_->iat_src), ModuleDtc::FUEL_IAT_SEV);
        report(ModuleDtc::FUEL_MAF, need_maf, static_cast<SignalId>(cfg_->maf_src), ModuleDtc::FUEL_MAF_SEV);
    }
    float load_real, density_kpa;
    // BLEND IS TWO MAPS. Below Blend Start RPM the Alpha-N VE table (RPM x throttle) carries the charge;
    // above Blend End RPM the VE table on measured MAP does; between, their AIR MASSES are crossfaded (see
    // below). It used to feed one VE table a MAP estimated from the Predicted MAP table — a table that is
    // for transients — which made low-RPM fuel depend on two tables and made fixing it move the
    // MAP-indexed VE cells the upper range shares. Fuel load is MAP here: that is what the VE table reads.
    float map_share = 1.0f;                            // Blend: the MAP map's share of the charge
    if (model == 3) {                                  // Blend
        const float lo = static_cast<float>(cfg_->blend_rpm_lo), hi = static_cast<float>(cfg_->blend_rpm_hi);
        map_share = (hi > lo) ? std::clamp((pos.rpm - lo) / (hi - lo), 0.0f, 1.0f)
                              : (pos.rpm >= hi ? 1.0f : 0.0f);
        load_real = map_kpa; density_kpa = map_kpa;
    } else if (model == 1) {                           // Alpha-N
        load_real = tps_pct; density_kpa = baro_kpa;   // no manifold reading — the air is at atmosphere
    } else {                                           // Speed-Density (0) / MAF (2): MAP load + density
        load_real = map_kpa; density_kpa = map_kpa;
    }
    bus.set(wk::fuel_load, load_real, true, now, ttl());
    // THE ALPHA-N MAP'S SHARE of the charge (0 outside Blend's low range), for every model. Stated this way
    // round on purpose: the VE autotune rejects records where it is above zero, and a log or record that
    // does not carry the channel reads it as 0 — which must mean "the VE table carried it", not "reject".
    bus.set(SIG_BLEND_ALPHA_SHARE, (1.0f - map_share) * 100.0f, true, now, ttl());

    // --- VE (one channel-driven lookup drives BOTH the air-mass estimate and wk::ve). The optional
    //     Z/depth axis (ve_table_z_en, default off) adds the flex-composition dimension. ---
    const float ve = tbl::table_eval(ve_table_desc(cfg_), bus);
    frame.ve_pct = ve;
    bus.set(wk::ve, ve, true, now, ttl());

    // --- Air mass per model: MAF measures airflow directly; the rest are VE × charge density. ---
    float air_mass_mg;
    // A FAILED MAF FALLS BACK TO SPEED-DENSITY. Read as 0 g/s it left the injectors open for their dead
    // time alone and the engine died. MAF mode already reads MAP for the charge density, so VE × density
    // is right there; FUEL_MAF above says why the engine is running on it.
    const SignalId maf_id = static_cast<SignalId>(cfg_->maf_src);
    const bool maf_failed = (model == 2) && (maf_id == SIG_NONE || !bus.valid(maf_id));
    bus.set(SIG_MAF_FAILOVER, maf_failed ? 1.0f : 0.0f, true, now, ttl());
    if (model == 2 && !maf_failed) {                   // MAF: airflow / induction events (VE bypassed)
        const float maf_g_s      = bus.get(maf_id, 0.0f);
        // Induction events per second. Each chamber inducts ONCE PER ENGINE CYCLE, so the divisor is
        // simply how many crank (or eccentric-shaft) revolutions a cycle spans:
        //   four-stroke  720 deg  -> 2 revs -> ncyl/2
        //   two-stroke   360 deg  -> 1 rev  -> ncyl
        //   rotary      1080 deg  -> 3 revs -> ncyl/3   (a 13B at 6000 rpm = 200 pulses/s)
        // Getting it wrong is a silent fuel error of exactly that ratio: the engine runs, badly, and
        // nothing reports it.
        const float revs_per_cycle =
            static_cast<float>(engine_cycle_angle(g_config.engine.cycle_type)) / ANGLE_360;
        const float events_per_s = (pos.rpm / 60.0f) * (cfg_ncyl / revs_per_cycle);
        air_mass_mg = (events_per_s > 0.0f) ? (maf_g_s * 1000.0f / events_per_s) : 0.0f;
    } else {
        constexpr float R_AIR = 287.05f;               // J/(kg·K)
        const float air_density_mg_cc = (density_kpa * 1000.0f) / (R_AIR * t_charge_k);
        air_mass_mg = cyl_vol_cc * (ve / 100.0f) * air_density_mg_cc;
        if (model == 3) {
            // THE ALPHA-N MAP'S AIR, at ATMOSPHERE: its VE was tuned against throttle with no manifold
            // reading, so baro is its density reference (and altitude comes with it for free). Mixed with
            // the MAP map's air by the share — air with air, never VE with VE across two references.
            const float ve_a = tbl::table_eval(alpha_ve_table_desc(cfg_), bus);
            bus.set(SIG_VE_ALPHA, ve_a, true, now, ttl());
            const float air_a = cyl_vol_cc * (ve_a / 100.0f) * ((baro_kpa * 1000.0f) / (R_AIR * t_charge_k));
            air_mass_mg = (1.0f - map_share) * air_a + map_share * air_mass_mg;
        }
    }
    bus.set(wk::air_mass, air_mass_mg, true, now, ttl());
    // CHARGE LOAD: that air as a % of a full charge (100 % VE at baro and this charge temperature). Every
    // model's, in the same terms, and continuous through Blend's crossover — the load axis a Blend or MAF
    // tune points target lambda and timing at.
    {
        constexpr float R_AIR = 287.05f;
        const float full_mg = cyl_vol_cc * (baro_kpa * 1000.0f) / (R_AIR * t_charge_k);
        bus.set(SIG_CHARGE_LOAD, (full_mg > 0.0f) ? air_mass_mg / full_mg * 100.0f : 0.0f, true, now, ttl());
    }

    // --- Target lambda — same rpm×load surface as VE ---
    float lambda_target = tbl::table_eval(target_lambda_table_desc(cfg_), bus);
    lambda_target = std::clamp(lambda_target, 0.5f, 1.5f);
    frame.target_lambda = lambda_target;
    bus.set(wk::lambda_target, lambda_target, true, now, ttl());

    // EACH STAGE'S STOICH: its base fuel blended toward ethanol by its own ethanol content (resolved at the
    // top). Stage 1's is the one the base charge is computed in; the others enter at the split, where
    // each stage's share of the air is turned back into its own fuel.
    float stage_stoich[kStages];
    {
        const uint16_t base_x10[kStages] = { cfg_->stage1_stoich_x10, cfg_->stage2_stoich_x10,
                                             cfg_->stage3_stoich_x10, cfg_->stage4_stoich_x10 };
        const uint16_t eth_x10[kStages]  = { cfg_->stage1_stoich_ethanol_x10, cfg_->stage2_stoich_ethanol_x10,
                                             cfg_->stage3_stoich_ethanol_x10, cfg_->stage4_stoich_ethanol_x10 };
        for (int s = 0; s < kStages; ++s) {
            const float ef = std::clamp(ethanol_pct_[s] / 100.0f, 0.0f, 1.0f);
            stage_stoich[s] = std::max(1.0f, (1.0f - ef) * (base_x10[s] / 10.0f) + ef * (eth_x10[s] / 10.0f));
        }
    }
    const float stoich = stage_stoich[0];

    // Injector pressure differential (rail − manifold) — the X channel of both the flow + dead-time
    // tables, PER STAGE. Not always a measurement: most installs have a regulator and a number written
    // on it, and this used to subtract the manifold from an absent sensor reading zero. What the
    // regulator REFERENCES is the whole difference between the three answers:
    //
    //   Sensor       the rail is measured, so the differential is measured minus manifold.
    //   Fixed        the rail is held at a constant pressure, so the differential FALLS as the manifold
    //                rises. base is gauge at atmosphere — the number on the regulator — so the rail in
    //                absolute terms is base + baro.
    //   Rising rate  the rail climbs with boost at the regulator's RATIO. At 1.0:1 it is manifold-
    //                referenced — the rail follows the manifold exactly, so the differential never moves
    //                and no pressure sensor of any kind is needed. Above 1.0 the rail climbs faster than
    //                the manifold and the differential RISES with boost, which is what an aftermarket
    //                rising-rate regulator is for. Vacuum is ignored: below atmosphere the regulator is
    //                on its seat and the differential is just the base.
    //
    // Note what Fixed does when there is no MAP sensor: map_kpa is atmosphere, so it reduces to base.
    // That is not a fallback bolted on, it is the same equation telling the truth about what it knows.
    //
    // AND ONE PER STAGE, because staged injection is very often a second set of injectors on a second
    // rail — its own pump, its own regulator, its own sensor or none. Every stage indexed on the
    // primary's differential before this, which is right only while they share a rail.
    // src is a SignalId, NOT a small index — the ids run past 255 (inj_press_diff_4 is 440), so a
    // uint8_t here silently truncated a rail sensor to whatever signal (id & 0xFF) happens to be.
    struct RailCfg { uint8_t mode; uint16_t base_x10; uint8_t ratio_x10; int16_t src; };
    const RailCfg rail[MAX_INJ_STAGES] = {
        { cfg_->stage1_fuel_press_mode, cfg_->stage1_fuel_press_base_kpa, cfg_->stage1_fuel_press_ratio, cfg_->stage1_fuel_press_src },
        { cfg_->stage2_fuel_press_mode, cfg_->stage2_fuel_press_base_kpa, cfg_->stage2_fuel_press_ratio, cfg_->stage2_fuel_press_src },
        { cfg_->stage3_fuel_press_mode, cfg_->stage3_fuel_press_base_kpa, cfg_->stage3_fuel_press_ratio, cfg_->stage3_fuel_press_src },
        { cfg_->stage4_fuel_press_mode, cfg_->stage4_fuel_press_base_kpa, cfg_->stage4_fuel_press_ratio, cfg_->stage4_fuel_press_src },
    };
    static const SignalId DP_SIG[MAX_INJ_STAGES] = {
        SIG_INJ_PRESS_DIFF, SIG_INJ_PRESS_DIFF_2, SIG_INJ_PRESS_DIFF_3, SIG_INJ_PRESS_DIFF_4 };
    for (uint8_t st = 0; st < MAX_INJ_STAGES; ++st) {
        const float base = rail[st].base_x10 * 0.1f;
        float dp;
        switch (rail[st].mode) {
            case 1:  dp = base + baro_kpa - map_kpa; break;             // Fixed regulator
            case 2: {                                                   // Rising rate, at its ratio
                // THE REFERENCE TRACKS BOTH WAYS — this clamp is not saying otherwise. A referenced
                // regulator's rail follows the manifold up AND down: suck on the hose and the rail
                // pressure drops, blow and it rises. That is already what this computes, because the
                // rail is dP + manifold: hold dP at base and the gauge reading falls by exactly the
                // vacuum. Constant differential and a rail that tracks vacuum are the same sentence.
                //
                // What is clamped is only the RATIO SURPLUS. A 6:1 amplifies boost by five; amplifying
                // VACUUM by five would put the differential at 300 + 5 x (30 - 101.3) = -56 kPa at
                // idle, which is fuel flowing backwards up the rail. No regulator does that — under
                // vacuum a rising-rate unit behaves as 1:1, which is what falls out here.
                const float boost = std::max(0.0f, map_kpa - baro_kpa);
                dp = base + (rail[st].ratio_x10 * 0.1f - 1.0f) * boost;
                break;
            }
            default: dp = bus.get(static_cast<SignalId>(rail[st].src),  // Sensor
                                  base + baro_kpa) - map_kpa; break;
        }
        bus.set(DP_SIG[st], dp, true, now, ttl());
    }
    // NOTHING READS THE DIFFERENTIAL AFTER THIS. It is not a term in any equation — it is an
    // AXIS. It reaches the flow and dead-time tables the way every other table input does, off the bus,
    // which is why publishing it is the whole of using it. (The sqrt(dP) correction it would otherwise
    // feed is gone: the dP curve in the flow table IS the flow-vs-pressure relationship.)
    // Flow rate from the 2D table (inj-pressure-diff × battery voltage) → cc/min; the dP curve IS the
    // flow-vs-pressure relationship, so there's no separate sqrt(dP) correction any more.
    const float inj_flow_ul_us = tbl::table_eval(stage1_inj_flow_table_desc(cfg_), bus) * 1000.0f / 60.0e6f;  // cc/min → µl/µs (×1000 µl/cc ÷ 60e6 µs/min)
    const float fuel_mass_mg   = air_mass_mg / (std::max(1.0f, stoich) * lambda_target);
    // Fuel MASS → VOLUME: divide by specific gravity (SG = density in g/cc = mg/µl) before the
    // volumetric injector flow. Collapsed 1×1 (0.740 = 98 premium) by default; blend over fuel temp × ethanol.
    // Per stage (each stage's own fuel, temperature and blend); stage 1's converts the base charge.
    using SgDescFn = tbl::TableDesc (*)(const FuelCalculatorConfig*);
    static const SgDescFn STAGE_SG[kStages] = {
        stage1_specific_gravity_table_desc, stage2_specific_gravity_table_desc,
        stage3_specific_gravity_table_desc, stage4_specific_gravity_table_desc };
    float stage_sg[kStages];
    for (int s = 0; s < kStages; ++s)
        stage_sg[s] = std::max(0.01f, tbl::table_eval(STAGE_SG[s](cfg_), bus));   // SG = mg/µl directly
    const float fuel_sg = stage_sg[0];
    const float fuel_vol_ul = fuel_mass_mg / fuel_sg;
    // PUBLISH WHAT THE MODEL BELIEVED. On a flex car the specific gravity moves with the blend and the
    // fuel temperature, changing every pulse width with it — and until now nothing could see which
    // number did that.
    bus.set(SIG_FUEL_SG, fuel_sg, true, now, ttl());
    // Fuel flow as a volume, since the sensor reads mass and everything that quotes a flow wants cc/min.
    // Published only when the sensor is actually there: a flow derived from an absent reading is zero,
    // and zero is a plausible flow.
    if (bus.valid(SIG_FUEL_FLOW))
        bus.set(SIG_FUEL_FLOW_VOL, bus.get(SIG_FUEL_FLOW) / fuel_sg * 60.0f, true, now, ttl());
    // Litres, from the sender's percentage and the tank it is in. Both have to be known: no capacity
    // means no quantity, and saying nothing is better than saying zero on a full tank.
    if (cfg_->tank_capacity_l > 0 && bus.valid(SIG_FUEL_LEVEL_1))
        bus.set(SIG_FUEL_LEVEL_L,
                bus.get(SIG_FUEL_LEVEL_1) * 0.01f * (cfg_->tank_capacity_l * 0.1f), true, now, ttl());
    const float base_pw_us = (inj_flow_ul_us > 0.0f) ? fuel_vol_ul / inj_flow_ul_us : 0.0f;

    // --- Step 3: publish correction factors onto the bus (a fixed set of named signals,
    //             visible in telemetry; Step 4 aggregates them — see [[fueling-subsystem]]) ---
    // Current engine run-state (published by EngineTask's state machine) — gates the cranking factor
    // below and the wall-film model further down. Read once, reused.
    const EngineRunState run_state =
        static_cast<EngineRunState>(static_cast<int>(bus.get(wk::engine_state, 0.0f)));

    // SLOW corrections (warmup/clt, iat, fuelcomp, baro, gear, generic1-4) are produced OFF this hot
    // path by the 1 kHz FuelTrim module (round-robin, one table/frame) and read back via the m()
    // aggregation in Step 4 — their inputs (coolant/IAT/baro/ethanol/gear) move far slower than the
    // engine cycle. Only the genuinely FAST corrections stay here (cranking/post-start lifecycle,
    // map/rev-limit/dead-time rpm/MAP-driven). See [[fueling-subsystem]] / FuelTrim.

    // Cranking enrichment: an ABSOLUTE-% fuel multiplier (100% = ×1.0) applied ONLY while CRANKING (RPM
    // below cranking_rpm). Same generic table path + correction signal as every other factor; its axes are fully
    // configurable (default CLT × optional composition). Neutral (×1.0) outside CRANKING — it just stacks.
    // FLOOD CLEAR — hold the throttle open while cranking and the fuel stops, so the engine pumps a
    // flooded cylinder out instead of being given more to drown in. It cuts through wk::fuel_cut, the
    // same shared cut every other requester ORs into (validity IS the OR), so the output stage stops the
    // injectors and nothing here has to reach into the scheduler.
    //
    // Three guards, all of them the point rather than caution: only while CRANKING (a running engine is
    // never asking for this), only above the configured throttle (nothing else wants wide-open throttle
    // at cranking speed, so it cannot be reached by accident), and only with a VALID throttle reading —
    // a dead or unassigned TPS must never be able to stop an engine from starting.
    const bool tps_ok = cfg_->tps_src != SIG_NONE && bus.valid(static_cast<SignalId>(cfg_->tps_src));
    const bool flood_clear = cfg_->flood_clear_enabled
                          && run_state == EngineRunState::CRANKING
                          && tps_ok
                          && tps_pct >= static_cast<float>(cfg_->flood_clear_tps_pct);
    if (flood_clear)
        bus.set_bool(wk::fuel_cut, true, now, ttl());
    bus.set(wk::flood_clear, flood_clear ? 1.0f : 0.0f, true, now, ttl());

    // Each correction carries its own enable, and an enable SHORT-CIRCUITS: the table is not evaluated
    // and the signal is published as exactly 1.000. A neutral table was the old way to say "off", which
    // is a different statement — it still costs an evaluation, still reads its axis channels, and leaves
    // a page showing 1.000 with no way to say whether that is deliberate or unconfigured.
    const bool crank_on = cfg_->enable_cranking && run_state == EngineRunState::CRANKING;
    const float crank_pct = crank_on ? tbl::table_eval(cranking_fuel_table_desc(cfg_), bus) : 0.0f;
    bus.set(wk::fuel_corr_cranking, crank_on ? std::clamp(crank_pct, 0.0f, 600.0f) / 100.0f : 1.0f,
            true, now, ttl());

    // Prime pulse: a ONE-SHOT simultaneous squirt, computed here (reusing the fuel-mass math) and
    // fired by EngineTask via the HAL. Latched (primed_) to once per power-up, on the first crank.
    // prime_mode picks how the prime table cell reads: 0 = injection time (ms), 1 = % of the 100%-VE
    // fuel mass for the engine at rest (density = baro, no running vacuum).
    // …ON SYNC, which is the whole rule: the prime is the first fuel the engine gets when it cranks and
    // the decoder locks, not a squirt at key-on. Without the sync condition every key-on that turned the
    // engine even slightly delivered one, so a session of key on/off while tuning built liquid fuel in the
    // ports with nothing burning it off — and the fix is not a temperature gate or a timer, it is firing
    // when the engine is actually starting.
    frame.prime_pw_us = 0;
    // …and NOT while flood clear is asking for the fuel to stop. The prime is fired directly by the
    // scheduler rather than through the injector events, so the output-stage cut does not reach it:
    // without this, clearing a flooded engine would begin by squirting every injector once. The latch
    // is left alone too, so the prime still happens on a later crank with the throttle closed.
    if (cfg_->prime_enable && !primed_ && pos.is_synchronized && !flood_clear
            && run_state == EngineRunState::CRANKING) {
        const float prime_val = tbl::table_eval(prime_fuel_table_desc(cfg_), bus);
        float prime_pw;
        if (cfg_->prime_mode == 1) {                       // VE %
            constexpr float R_AIR = 287.05f;               // J/(kg·K)
            const float density_rest_mg_cc = (baro_kpa * 1000.0f) / (R_AIR * t_charge_k);
            const float fuel_mass_100ve_mg = cyl_vol_cc * density_rest_mg_cc / std::max(1.0f, stoich);
            const float prime_mass_mg      = (prime_val / 100.0f) * fuel_mass_100ve_mg;
            prime_pw = (inj_flow_ul_us > 0.0f) ? (prime_mass_mg / fuel_sg) / inj_flow_ul_us : 0.0f;
            // …plus the injector's opening time. A computed MASS becomes a pulse only once the dead time
            // is added — without it the prime delivered less than the table asked for, most of all on a
            // cold, low-voltage crank where the dead time is longest. (Injection-Time mode is the total
            // pulse as written, so it is left alone.)
            if (prime_pw > 0.0f)
                prime_pw += std::clamp(tbl::table_eval(stage1_dead_time_table_desc(cfg_), bus), 0.0f, 65000.0f);
        } else {                                           // Injection Time: cell is ms
            prime_pw = std::clamp(prime_val, 0.0f, 10000.0f) * 1000.0f;   // ms → µs
        }
        frame.prime_pw_us = static_cast<uint32_t>(std::max(0.0f, prime_pw));
        primed_ = true;
    }
    bus.set(wk::prime_pw, static_cast<float>(frame.prime_pw_us), true, now, ttl());

    // Post-start enrichment: decaying CLT×run-time extra fuel on top of warmup. run_start_ms_ is
    // anchored by on_engine_start() at the real cranking->running edge (EngineTask drives the state
    // machine), so wk::run_time is seconds-since-the-engine-caught — not since boot.
    const uint32_t now_ms = platform_get_tick_ms();
    bus.set(wk::run_time, running_ ? static_cast<float>(now_ms - run_start_ms_) / 1000.0f : 0.0f,
            true, now, ttl());
    // ONLY ONCE IT HAS CAUGHT. run_time reads 0 while cranking, which is the table's maximum — so this
    // multiplied the cranking enrichment by the full post-start figure, over-fuelling every cold crank.
    // The help says it: extra fuel for the first seconds AFTER the engine catches.
    bus.set(wk::fuel_corr_poststart,
            (cfg_->enable_poststart && running_)
                ? 1.0f + tbl::table_eval(post_start_table_desc(cfg_), bus) / 100.0f : 1.0f,
            true, now, ttl());

    // Rev-limiter fuel trim (8×8 RPM-before-cut × MAP) — RPM-driven, stays on the per-cycle path.
    bus.set(wk::fuel_corr_revlimit,
            cfg_->enable_revlimit ? 1.0f + tbl::table_eval(rev_limit_fuel_corr_table_desc(cfg_), bus) / 100.0f : 1.0f,
            true, now, ttl());

    // (warmup/iat/fuelcomp/baro/gear/generic1-4 are slow → produced by FuelTrim off this path; the m()
    // in Step 4 reads them off the bus.)

    // Overall (global % scalar) — trivial, stays here.
    bus.set(wk::fuel_corr_overall,
            cfg_->enable_overall ? 1.0f + (cfg_->overall_corr_pct * 0.1f) / 100.0f : 1.0f,
            true, now, ttl());

    // Injector dead time from the 2D table (inj-pressure-diff × battery voltage) → µs.
    bus.set(wk::pw_add_deadtime, tbl::table_eval(stage1_dead_time_table_desc(cfg_), bus), true, now, ttl());

    // Throttle-movement transient fuel now lives in the 1 kHz TransientThrottle module (it needs a fast,
    // un-windowed source); it publishes fuel_corr_accel, which the aggregation below composes.

    // --- Step 4: compose the corrections — hand-summed (like the ignition side). Each is read off the
    // bus with a neutral default (1.0 absent = no effect) and its own clamp, then multiplied onto fuel
    // mass; the cross-module ones (STFT/LTFT from Lambda, transient from TransientThrottle, protection from
    // EngineProtection) come from the bus, the rest FuelCalc published above. A total-authority guard
    // caps the product. Adding a correction = one m(...) line. ---
    float mass_mult = 1.0f;
    auto m = [&](SignalId s, float lo, float hi) { mass_mult *= std::clamp(bus.get(s, 1.0f), lo, hi); };
    m(wk::fuel_corr_warmup,    0.2f, 5.0f);
    m(wk::fuel_corr_cranking,  0.0f, 6.0f);    // absolute-% cranking enrichment (gated to CRANKING)
    m(wk::fuel_corr_poststart, 0.2f, 5.0f);
    m(wk::fuel_corr_iat,       0.2f, 5.0f);
    m(wk::fuel_corr_revlimit,  0.2f, 5.0f);
    m(wk::fuel_corr_generic1,  0.2f, 5.0f);
    m(wk::fuel_corr_generic2,  0.2f, 5.0f);
    m(wk::fuel_corr_generic3,  0.2f, 5.0f);
    m(wk::fuel_corr_generic4,  0.2f, 5.0f);
    m(wk::fuel_corr_overall,   0.2f, 5.0f);
    // (Fuel composition is NOT here: it is per stage, a trim on the fuel each stage delivers — the split
    // below applies stage 1's and the others' to their own shares.)
    m(wk::fuel_corr_baro,      0.2f, 5.0f);
    m(wk::fuel_corr_gear,      0.2f, 5.0f);
    m(wk::fuel_corr_protection, 0.2f, 5.0f);   // EngineProtection
    m(wk::fuel_corr_egt,       1.0f, 3.0f);    // EgtProtect (enrich-only, >=1)
    m(wk::fuel_corr_launch,    0.2f, 5.0f);    // Launch (its own map, 1.0 whenever launch is not holding)
    m(wk::fuel_corr_accel,     0.2f, 4.0f);    // TransientThrottle (>1 enrich, <1 disenrich)
    m(wk::fuel_corr_stft,      0.5f, 1.5f);    // Lambda
    m(wk::fuel_corr_ltft,      0.5f, 1.5f);    // Lambda
    mass_mult = std::clamp(mass_mult, 0.1f, 10.0f);          // total-authority guard
    const float pw_add = std::clamp(bus.get(wk::pw_add_deadtime, 0.0f), 0.0f, 65000.0f);

    // Wall-film (X-τ) transient compensation, on the FUEL pw (before the dead-time adder). A
    // fraction X of injected fuel wets the port wall; the film evaporates into the cylinder with
    // time constant τ. To deliver the desired charge: injected = (desired - evaporated)/(1-X); the
    // film then gains X·injected and loses the evaporated. Steady state -> injected == desired (no
    // net correction); tip-in over-injects to fill the film, tip-out under-injects (film gives back).
    //
    // Only modelled when RUNNING: during cranking the port wetting is dominated by cold-start priming,
    // not the steady X-τ dynamics, and the slow per-cycle dt at crank speeds makes the film estimate
    // meaningless. When not running we hold the
    // film at zero and disarm the dt clock so the first running frame re-primes cleanly. (run_state was
    // read once in Step 3.)
    // (The fuel film is applied PER STAGE after the split below: a direct-injected stage has no port wall,
    // and each stage's film is its own injectors' fuel.)
    float fuel_pw = std::max(0.0f, base_pw_us * mass_mult);
    // --- Split the cycle's charge across its injection EVENTS. Everything above computes what one
    //     chamber needs for one engine CYCLE; the distribution mode decides how many squirts that
    //     arrives in (injection_events_per_cycle — the same function the scheduler builds its event
    //     list from, so the two cannot disagree). Multi-point and batch take the count from the tune,
    //     semi-sequential fires once per crank revolution, full-sequential once per cycle.
    //
    //     THE DIVIDE HAPPENS HERE, before dead time and the short-pulse adder, because those are
    //     per-OPENING costs and do not divide: two squirts of half the fuel each still pay the
    //     injector's opening latency twice. Splitting after them would have subtracted a dead time's
    //     worth of fuel from every squirt. ---
    //     Injection is now PER STAGE; the primary stage (inj_stage[0]) drives the base charge split.
    //     AT THE CURRENT SYNC LEVEL: at crank-only sync every event fires once per revolution, so a
    //     sequential engine gets two squirts a cycle and must be given half each (see the function).
    const uint8_t inj_events = injection_deliveries_per_cycle(g_config.engine.inj_stage[0].mode,
                                                              g_config.engine.cycle_type,
                                                              g_config.engine.inj_stage[0].injections_per_cycle,
                                                              pos.sync_level == SyncLevel::PHASE);
    if (inj_events > 1) fuel_pw /= static_cast<float>(inj_events);

    // --- Sequential N-stage staging. Each stage carries the charge up to its Staging
    //     Duty Cycle, then holds while the NEXT stage begins — the overflow rises to the next stage.
    //     Once EVERY stage is at its cap, they all rise to 100% together, so no injector's cap->100% headroom is left unused at high
    //     load — the earlier last-stage-uncapped model wasted it and ran lean at max. The TOTAL delivered
    //     fuel is unchanged (staging only moves work between injectors, AFR preserved); a single-stage
    //     engine puts everything on the primary, byte-identical to the un-staged path.
    //
    //     Work in fuel MASS carried as PRIMARY-flow PW (a mass proxy): a stage's own PW = mass /
    //     stage_flow, and its duty = pw × events / cycle_us. The duty window is ONE ENGINE CYCLE, counted
    //     over ALL the cycle's events (not one squirt), so a multi-squirt stage's saturation shows.
    //     Each staged stage has its OWN injector tables (flow / dead-time / short-pulse / angle) — stage
    //     2 = the stage2_* tables, stage 3 = stage3_*, stage 4 = stage4_*. ---
    using StageDescFn = tbl::TableDesc (*)(const FuelCalculatorConfig*);
    static const StageDescFn STAGE_FLOW[MAX_STAGED_STAGES] = {
        stage2_inj_flow_table_desc, stage3_inj_flow_table_desc, stage4_inj_flow_table_desc };
    static const StageDescFn STAGE_DEAD[MAX_STAGED_STAGES] = {
        stage2_dead_time_table_desc, stage3_dead_time_table_desc, stage4_dead_time_table_desc };
    static const StageDescFn STAGE_SHORT[MAX_STAGED_STAGES] = {
        stage2_short_pw_adder_table_desc, stage3_short_pw_adder_table_desc, stage4_short_pw_adder_table_desc };
    static const StageDescFn STAGE_ANGLE[MAX_STAGED_STAGES] = {
        stage2_inj_angle_table_desc, stage3_inj_angle_table_desc, stage4_inj_angle_table_desc };
    // The bus channel each staged stage reports its OWN duty cycle on. Deliberately not called
    // STAGE_DUTY — that name is taken just below by the STAGING duty table, which is the flow
    // percentage at which a stage hands over to the next. Two different quantities, and confusing
    // them would silently publish a hand-over threshold as an injector duty.
    static const SignalId STAGED_DUTY_SIG[MAX_STAGED_STAGES] = {
        wk::inj_duty_2, wk::inj_duty_3, wk::inj_duty_4 };
    // Staging Duty Cycle is a COLLAPSIBLE table now (a flow-percentage table), indexed ALL stages incl.
    // the primary — a single 1×1 cell of 50% by default (= the old scalar), expandable to RPM × MAP.
    static const StageDescFn STAGE_DUTY[MAX_INJ_STAGES] = {
        stage1_staging_duty_table_desc, stage2_staging_duty_table_desc,
        stage3_staging_duty_table_desc, stage4_staging_duty_table_desc };

    const float cycle_revs = static_cast<float>(engine_cycle_angle(g_config.engine.cycle_type)) / ANGLE_360;
    const float cycle_us   = (pos.rpm > 0.0f) ? (60.0e6f * cycle_revs / pos.rpm) : 1.0e9f;
    const uint8_t nstages  = std::clamp<uint8_t>(g_config.engine.num_inj_stages, 1, MAX_INJ_STAGES);
    // MIXED FUELS. The split below works in AIR, not fuel: a stage delivering its fuel at flow x SG
    // burns flow x SG x stoich of air per microsecond, and trimming its fuel by its composition
    // correction delivers the same air in more fuel. That rate is what each stage is compared by, so a
    // stage on E85 carries its share of the charge in its own fuel, at its own density. With every stage
    // on one fuel the rates are the flows times one constant, which cancels — the split is the old one.
    // The base charge (fuel_pw) is in stage 1's fuel with no trim, so in these units it is fuel_pw x
    // stage 1's trim.
    const float stage_corr[kStages] = {
        std::clamp(bus.get(wk::fuel_corr_fuelcomp, 1.0f),       0.2f, 5.0f),
        std::clamp(bus.get(SIG_FUEL_CORR_FUELCOMP_2, 1.0f),     0.2f, 5.0f),
        std::clamp(bus.get(SIG_FUEL_CORR_FUELCOMP_3, 1.0f),     0.2f, 5.0f),
        std::clamp(bus.get(SIG_FUEL_CORR_FUELCOMP_4, 1.0f),     0.2f, 5.0f) };
    auto rate = [&](int s, float flow) { return flow * stage_sg[s] * stage_stoich[s] / stage_corr[s]; };
    const float prim_flow  = (inj_flow_ul_us > 0.0f) ? rate(0, inj_flow_ul_us) : 1.0f;   // stage 1's RATE
    // Per-staged-stage flow (µl/µs), evaluated once for the fill. Flow tables index on inj_press_diff ×
    // battery, both already on the bus.
    float staged_flow[MAX_STAGED_STAGES] = {};
    for (uint8_t st = 0; st < MAX_STAGED_STAGES; ++st)
        staged_flow[st] = tbl::table_eval(STAGE_FLOW[st](cfg_), bus) * 1000.0f / 60.0e6f;

    float stage_fuel_pw[MAX_INJ_STAGES] = {};   // each stage's OWN fuel PW (before dead-time / short-pulse)
    float stage_mass[MAX_INJ_STAGES]    = {};   // mass each stage carries, in primary-flow-PW units
    float stage_flow_[MAX_INJ_STAGES]   = {};   // per-stage flow (µl/µs), 0 = stage absent
    float remaining = fuel_pw * stage_corr[0];  // total charge, as stage 1's rate x PW (air proxy)
    const float full_pw = (inj_events > 0) ? (cycle_us / inj_events) : 0.0f;   // one squirt's 100%-duty PW
    // Pass 1: fill each stage sequentially up to its Staging Duty Cycle. EVERY stage is capped now
    // (including the last) — that is what lets pass 2 raise them all together.
    for (uint8_t s = 0; s < nstages; ++s) {
        const float raw  = (s == 0) ? inj_flow_ul_us : staged_flow[s - 1];
        const float flow = (raw > 0.0f) ? rate(s, raw) : 0.0f;     // this stage's RATE (see above)
        stage_flow_[s] = flow;
        if (flow <= 0.0f) continue;
        const float max_duty = tbl::table_eval(STAGE_DUTY[s](cfg_), bus) / 100.0f;   // table % -> fraction
        const float cap_mass = max_duty * full_pw * flow / prim_flow;   // the cap's mass, in primary-flow-PW
        const float taken    = std::min(remaining, cap_mass);
        remaining     = std::max(0.0f, remaining - taken);
        stage_mass[s] = taken;
    }
    // Pass 2: demand still left once every stage holds at its cap -> raise them ALL to 100% together,
    // splitting the leftover by each stage's cap->100% headroom so they saturate simultaneously.
    if (remaining > 0.0f) {
        float head[MAX_INJ_STAGES] = {}, total_head = 0.0f;
        for (uint8_t s = 0; s < nstages; ++s) {
            if (stage_flow_[s] <= 0.0f) continue;
            head[s] = std::max(0.0f, full_pw * stage_flow_[s] / prim_flow - stage_mass[s]);  // cap->100%
            total_head += head[s];
        }
        if (total_head > 0.0f) {
            const float frac = std::min(1.0f, remaining / total_head);   // 1.0 = every stage at 100%
            for (uint8_t s = 0; s < nstages; ++s) stage_mass[s] += head[s] * frac;
            remaining -= total_head * frac;
        }
    }
    // Pass 3: beyond every injector at 100% is out-of-injector — command the residual on the last active
    // stage so no fuel is silently dropped (this is exactly the un-staged path for a single stage).
    if (remaining > 0.0f)
        for (int8_t s = static_cast<int8_t>(nstages) - 1; s >= 0; --s)
            if (stage_flow_[s] > 0.0f) { stage_mass[s] += remaining; break; }
    for (uint8_t s = 0; s < nstages; ++s)   // each stage's mass -> its own PW
        stage_fuel_pw[s] = (stage_flow_[s] > 0.0f) ? stage_mass[s] * prim_flow / stage_flow_[s] : 0.0f;
    // THE CHARGE'S ETHANOL: each stage's content weighted by the fuel MASS it delivers (PW x flow x SG) —
    // what the cylinder actually burns, read next cycle by the ethanol-indexed tables (see the top). And
    // each staged stage's SG, published beside stage 1's (fuel_sg) while it carries fuel.
    {
        static const SignalId SG_SIG[kStages] = { SIG_FUEL_SG, SIG_FUEL_SG_2, SIG_FUEL_SG_3, SIG_FUEL_SG_4 };
        float mass = 0.0f, eth = 0.0f;
        for (uint8_t s = 0; s < nstages; ++s) {
            const float raw = (s == 0) ? inj_flow_ul_us : staged_flow[s - 1];
            const float m   = stage_fuel_pw[s] * std::max(0.0f, raw) * stage_sg[s];
            mass += m;
            eth  += m * ethanol_pct_[s];
            if (s > 0 && stage_fuel_pw[s] > 0.0f) bus.set(SG_SIG[s], stage_sg[s], true, now, ttl());
        }
        if (mass > 0.0f) { charge_ethanol_ = eth / mass; charge_known_ = true; }
    }

    // --- THE FUEL FILM, PER STAGE. Part of every port injection wets the wall and reaches the cylinder over
    //     the following cycles. Each stage's film is tracked in its own per-cycle PW: every update X of what
    //     is injected sticks and a fraction 1 - e^(-dt/tau) of the film evaporates in. The injection is the
    //     one that makes the cylinder receive what was asked: steady state x1.000 (deposit = evaporation), a
    //     load rise injects more to build the film, a fall gives it back. It runs all the time — the
    //     correction at a throttle snap depends on how big the film already is. During a cut nothing is
    //     injected, so the film only evaporates. X and tau are tables (Film Pooling %: CLT x MAP; Film
    //     Evaporation Time: RPM x CLT); a stage with Port Film off (direct injection) has no film. ---
    {
        const bool running = run_state == EngineRunState::RUNNING;
        const uint32_t now_ms = platform_get_tick_ms();
        const float dt_s = (running && wf_last_ms_ != 0) ? (now_ms - wf_last_ms_) / 1000.0f : 0.0f;
        wf_last_ms_ = running ? now_ms : 0u;
        const uint8_t film_on[kStages] = { cfg_->stage1_film_enabled, cfg_->stage2_film_enabled,
                                           cfg_->stage3_film_enabled, cfg_->stage4_film_enabled };
        float wanted_sum = 0.0f, injected_sum = 0.0f;
        if (!running || !cfg_->wallfilm_enabled) {
            for (float& f : film_pw_) f = 0.0f;
        } else if (dt_s > 0.0f && dt_s < 1.0f) {
            const float X     = std::clamp(tbl::table_eval(film_pool_table_desc(cfg_), bus) / 100.0f, 0.0f, 0.9f);
            const float tau_s = std::clamp(tbl::table_eval(film_evap_table_desc(cfg_), bus), 1.0f, 1000.0f) / 1000.0f;
            const float evap_frac = 1.0f - std::exp(-dt_s / tau_s);
            const bool  cut = bus.valid(wk::fuel_cut);
            const float ev  = static_cast<float>(std::max<uint8_t>(inj_events, 1));   // per event <-> per cycle
            for (uint8_t st = 0; st < nstages; ++st) {
                if (!film_on[st]) { film_pw_[st] = 0.0f; continue; }
                const float evap = film_pw_[st] * evap_frac;
                if (cut) { film_pw_[st] = std::max(0.0f, film_pw_[st] - evap); continue; }
                const float wanted   = stage_fuel_pw[st] * ev;
                const float injected = std::max(0.0f, (wanted - evap) / (1.0f - X));
                film_pw_[st] = std::max(0.0f, film_pw_[st] + X * injected - evap);
                wanted_sum += wanted; injected_sum += injected;
                stage_fuel_pw[st] = injected / ev;
            }
        }
        // What the film asked for, as one multiplier beside every other correction: 1.000 steady.
        bus.set(SIG_FUEL_CORR_FILM, wanted_sum > 0.0f ? injected_sum / wanted_sum : 1.0f, true, now, ttl());
    }

    // Stage 1 (primary): its fuel + primary dead-time/short-pulse. When single-stage this is the whole
    // charge — byte-identical to the un-staged path.
    const float prim_fuel_pw = stage_fuel_pw[0];
    bus.set(wk::inj_pw, prim_fuel_pw / 1000.0f, true, now, ttl());     // short-pw indexes on commanded PW (ms)
    const float prim_short = tbl::table_eval(stage1_short_pw_adder_table_desc(cfg_), bus) * 1000.0f;
    float pw_us = std::max(0.0f, prim_fuel_pw + pw_add + prim_short);

    // Staged stages 2..N: each its OWN tables — dead-time, firing angle, and a short-pulse adder
    // re-evaluated on that stage's own commanded PW.
    float   staged_pw_arr[MAX_STAGED_STAGES]   = {};
    float   staged_fuel_arr[MAX_STAGED_STAGES] = {};   // the FUEL part alone — what a trim may scale
    float   staged_add_arr[MAX_STAGED_STAGES]  = {};   // dead time + short-pulse: opening, not fuel
    bool    staged_active[MAX_STAGED_STAGES]   = {};
    int16_t staged_btdc_arr[MAX_STAGED_STAGES] = { 3550, 3550, 3550 };
    for (uint8_t st = 0; st < MAX_STAGED_STAGES; ++st) {
        const uint8_t s = static_cast<uint8_t>(st + 1);
        if (s >= nstages || stage_fuel_pw[s] <= 0.0f) continue;
        // A staged stage rides the primary's events, so it shares the primary's per-squirt basis
        // (stage_fuel_pw was divided by the primary's inj_events above).
        const float stage_pw = stage_fuel_pw[s];
        const float dead = std::clamp(tbl::table_eval(STAGE_DEAD[st](cfg_), bus), 0.0f, 65000.0f);
        staged_btdc_arr[st] = static_cast<int16_t>(tbl::table_eval(STAGE_ANGLE[st](cfg_), bus) * 10.0f);
        bus.set(wk::inj_pw, stage_pw / 1000.0f, true, now, ttl());
        const float sh = tbl::table_eval(STAGE_SHORT[st](cfg_), bus) * 1000.0f;
        staged_pw_arr[st]   = std::max(0.0f, stage_pw + dead + sh);
        staged_fuel_arr[st] = stage_pw;
        staged_add_arr[st]  = dead + sh;
        staged_active[st] = true;
        // This stage's duty, against the same squirt window as the primary — a staged stage rides the
        // primary's events, so it shares the denominator. Published only while the stage is carrying
        // fuel: an inactive stage `continue`s above and its channel expires on its ttl, which is how a
        // consumer tells "stage 3 is at 0%" from "there is no stage 3".
        if (full_pw > 0.0f)
            bus.set(STAGED_DUTY_SIG[st], staged_pw_arr[st] * 100.0f / full_pw, true, now, ttl());
    }
    bus.set(wk::inj_pw, pw_us / 1000.0f, true, now, ttl());            // telemetry: the primary injection PW
    // Primary duty, from the PW actually commanded (dead time and short-pulse adder included — those are
    // injector-open time like any other) over one squirt's 100%-duty window. full_pw is 0 only when the
    // stage injects zero times per cycle, and a divide there would publish an infinity onto the bus.
    if (full_pw > 0.0f)
        bus.set(wk::inj_duty, pw_us * 100.0f / full_pw, true, now, ttl());

    // --- Step 5: Apply fuel cut. Cut requesters publish wk::fuel_cut=true (only while they want a cut,
    // each at its own cadence with a ttl); the OR is the signal's VALIDITY — valid = someone is cutting,
    // expired = nobody is. This decouples the requesters' rates (they no longer have to share a frame). ---
    // The cut is applied at the OUTPUT (EnginePositionHal::set_output_cuts clears the injector's
    // execution bit, so INJ_OPEN never drives it). What the model asked for is reported unchanged:
    // this used to publish 0 while cutting, which told the tuner the fuel model wanted nothing when
    // it wanted plenty, and made a cut indistinguishable from a collapsed calculation.
    frame.base_fuel_pw_us = pw_us;
    const bool  cut      = bus.valid(wk::fuel_cut);   // still needed for the STAGED split below
    const float final_pw = pw_us;
    bus.set(wk::base_pw, final_pw, true, now, ttl());

    // Per-cylinder / per-bank fuel trim, gated by EACH stage's own injection mode. The correction scopes
    // are separable: OVERALL/bulk is already in mass_mult (base PW), so here we add the per-BANK and
    // per-CYLINDER deviations — but only where the stage can physically deliver them. A stage delivers
    // bank on any mode that resolves banks (everything except Multi-Point); it delivers per-cylinder only
    // on SEQUENTIAL (the one mode with a clean single per-hole pulse). Lambda publishes the deviations as
    // % signals (absent -> 0 -> ×1.0), so an overall-only or open-loop install is neutral.
    using CylDescFn = tbl::TableDesc (*)(const FuelCalculatorConfig*);
    static const CylDescFn CYL_FUEL[MAX_CYLINDERS] = {
        cyl1_fuel_corr_table_desc,  cyl2_fuel_corr_table_desc,  cyl3_fuel_corr_table_desc,
        cyl4_fuel_corr_table_desc,  cyl5_fuel_corr_table_desc,  cyl6_fuel_corr_table_desc,
        cyl7_fuel_corr_table_desc,  cyl8_fuel_corr_table_desc,  cyl9_fuel_corr_table_desc,
        cyl10_fuel_corr_table_desc, cyl11_fuel_corr_table_desc, cyl12_fuel_corr_table_desc };
    static const SignalId LTFT_BANK[2] = { SIG_LTFT_BANK_1_PCT, SIG_LTFT_BANK_2_PCT };
    // …AND THE FAST ONE. Lambda runs a loop per bank when banked, and publishes each bank's DEVIATION
    // from what the two agree on — the shared part rides in fuel_corr_stft with every other global
    // correction. So a cylinder gets its bank's whole answer as (common + deviation) and neither half
    // is counted twice. Unbanked these are zero and this is exactly the old behaviour.
    static const SignalId STFT_BANK[2] = { SIG_STFT_BANK_1_PCT, SIG_STFT_BANK_2_PCT };
    const int16_t inj_btdc_x10 = static_cast<int16_t>(tbl::table_eval(stage1_inj_angle_table_desc(cfg_), bus) * 10.0f);
    const uint8_t ncyl = std::min<uint8_t>(g_config.engine.cylinder_count, MAX_CYLINDERS);   // configured cylinders

    // What each stage's mode can deliver: bank on anything but Multi-Point, per-cyl on Sequential only.
    const auto deliver_bank = [](uint8_t mode) { return static_cast<InjectionMode>(mode) != InjectionMode::MULTI_POINT; };
    const auto deliver_cyl  = [](uint8_t mode) { return static_cast<InjectionMode>(mode) == InjectionMode::SEQUENTIAL; };
    const bool prim_bank = deliver_bank(g_config.engine.inj_stage[0].mode);
    const bool prim_cyl  = deliver_cyl (g_config.engine.inj_stage[0].mode);
    bool staged_bank[MAX_STAGED_STAGES], staged_cyl[MAX_STAGED_STAGES];
    for (uint8_t st = 0; st < MAX_STAGED_STAGES; ++st) {
        staged_bank[st] = deliver_bank(g_config.engine.inj_stage[st + 1].mode);
        staged_cyl[st]  = deliver_cyl (g_config.engine.inj_stage[st + 1].mode);
    }

    for (uint8_t i = 0; i < ncyl; i++) {
        const uint8_t bk = g_config.engine.cyl[i].bank;                 // 1..2 -> index 0..1
        const uint8_t bi = (bk >= 1 && bk <= 2) ? static_cast<uint8_t>(bk - 1) : 0;
        // CLOSED LOOP REACHES A CYLINDER AS ITS BANK'S OFFSET, and nothing finer. There is no
        // per-cylinder learned trim any more — a bank offset is the only closed-loop term there is,
        // and the per-cylinder table below is the tuner's, set from per-cylinder evidence.
        const float bank_off = 1.0f + (bus.get(LTFT_BANK[bi], 0.0f)
                                     + bus.get(STFT_BANK[bi], 0.0f)) / 100.0f;
        // MULTI-POINT CANNOT ADDRESS A BANK. Every injector fires on one common pulse, so a per-bank
        // difference is physically undeliverable — the two offsets collapse to their mean rather than
        // being dropped, which would throw away a correction both banks agree on.
        const float bank_mean = 1.0f + (bus.get(LTFT_BANK[0], 0.0f) + bus.get(LTFT_BANK[1], 0.0f)
                                      + bus.get(STFT_BANK[0], 0.0f) + bus.get(STFT_BANK[1], 0.0f))
                                       / 200.0f;
        const float cyl_tbl  = 1.0f + tbl::table_eval(CYL_FUEL[i](cfg_), bus) / 100.0f;   // manual per-cyl table
        // A stage's trim: the closed-loop bank offset (or its mean where banks cannot be addressed)
        // times the manual per-cylinder table, that table included only when the stage delivers
        // per-cylinder at all.
        const auto trim = [&](bool ab, bool ac) -> float {
            const float cl = std::clamp(ab ? bank_off : bank_mean, 0.5f, 1.5f);
            return cl * (ac ? cyl_tbl : 1.0f);
        };
        // A TRIM SCALES FUEL, NOT THE INJECTOR'S OPENING TIME. Dead time and the short-pulse adder are the
        // time the injector takes to open and flow linearly — the same for every cylinder whatever fuel it
        // needs. Multiplying the whole pulse by the trim scaled them too: at idle (1.5 ms fuel + 1.0 ms dead
        // time) a +5 % cylinder trim delivered +8.3 %, and every bank offset had a load-dependent gain.
        const float prim_add = pw_add + prim_short;
        frame.cyl[i].inj_pw_us    = static_cast<uint32_t>(std::max(0.0f,
                                        prim_fuel_pw * trim(prim_bank, prim_cyl) + prim_add));
        frame.cyl[i].inj_btdc_x10 = inj_btdc_x10;
        // Staged stages 2..N: each carries the PW the sequential fill gave it, trimmed per THAT stage's
        // mode. A fuel cut gates the staged injectors (a separate injector decision, not the output cut
        // that leaves the primary PW reported).
        for (uint8_t st = 0; st < MAX_STAGED_STAGES; ++st) {
            frame.cyl[i].staged_pw_us[st]    = (staged_active[st] && !cut)
                ? static_cast<uint32_t>(std::max(0.0f, staged_fuel_arr[st] * trim(staged_bank[st], staged_cyl[st])
                                                       + staged_add_arr[st])) : 0u;
            frame.cyl[i].staged_btdc_x10[st] = staged_btdc_arr[st];
            frame.cyl[i].staged_enabled[st]  = staged_active[st] && !cut;
        }
    }
}
