#include "test_helpers.h"
#include "../firmware/Engine/Modules/FuelCalculator.h"
#include "../firmware/Engine/EngineStateMachine.h"   // EngineRunState — wall-film is gated on RUNNING
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "../generated/signal_ids.h"
#include "../generated/ecu_config.h"   // g_config — staging config now lives on the engine

static uint32_t g_test_ms = 0;                                  // settable clock for AE timing
extern "C" uint32_t platform_get_tick_ms() { return g_test_ms; }
extern "C" uint32_t platform_cyccnt() { return 0; }   // fuel per-stage profiler is a no-op on host

static FuelCalculatorConfig make_cfg() {
    FuelCalculatorConfig c{};
    // Per-cyl/bank correction is gated by each stage's injection mode now (not a bitmask): the engine
    // config's inj_stage[0].mode defaults to 0 = Sequential, so stage 1 gets per-cyl + per-bank.
    for (unsigned i = 0; i < FUEL_CALCULATOR_STAGE1_INJ_FLOW_TABLE_ALLOC; i++) c.stage1_inj_flow_table[i] = 265;   // uniform flow (cc/min)
    // Each stage's FUEL, as the schema ships it: petrol (stoich 14.7, SG 0.740 as a collapsed 1x1), the
    // ethanol end at 9.0, and stage 1 flex on the flex sensor (unpublished here -> Ethanol % 0 -> petrol).
    c.stage1_specific_gravity_table[0] = c.stage2_specific_gravity_table[0] =
    c.stage3_specific_gravity_table[0] = c.stage4_specific_gravity_table[0] = 740;
    c.stage1_stoich_x10 = c.stage2_stoich_x10 = c.stage3_stoich_x10 = c.stage4_stoich_x10 = 147;
    c.stage1_stoich_ethanol_x10 = c.stage2_stoich_ethanol_x10 =
    c.stage3_stoich_ethanol_x10 = c.stage4_stoich_ethanol_x10 = 90;
    c.stage1_ethanol_src = c.stage2_ethanol_src = c.stage3_ethanol_src = c.stage4_ethanol_src = SIG_ETHANOL;
    c.stage1_flex_enabled = 1;
    c.map_src            = SIG_MAP;
    c.clt_src            = SIG_CLT;
    c.iat_src            = SIG_IAT;
    c.stage1_fuel_press_src     = SIG_FUEL_PRESSURE;
    // Atmosphere and rail pressure, as the schema ships them. A zeroed struct says the air is at 0 kPa
    // and the regulator holds 0 bar, which is not a configuration any tune can hold (the schema floors
    // the assumed baro at 50 kPa) — it is just what a hand-built fixture defaults to.
    c.baro_src            = SIG_BARO_KPA;
    c.baro_assumed_kpa    = 1013;    // 101.3 kPa, sea level
    // All four rails, because a stage can be on its own — the shipped default is the common case where
    // they share one, which is the same answers four times.
    c.stage1_fuel_press_mode = c.stage2_fuel_press_mode =
    c.stage3_fuel_press_mode = c.stage4_fuel_press_mode = 1;         // Fixed regulator
    c.stage1_fuel_press_base_kpa = c.stage2_fuel_press_base_kpa =
    c.stage3_fuel_press_base_kpa = c.stage4_fuel_press_base_kpa = 3000;   // 300.0 kPa = 3 bar
    c.stage1_fuel_press_ratio = c.stage2_fuel_press_ratio =
    c.stage3_fuel_press_ratio = c.stage4_fuel_press_ratio = 10;           // 1.0:1 — manifold-referenced
    c.fuel_model         = 0;             // Stage F: 0=SpeedDensity (default)
    c.tps_src            = SIG_TPS;
    c.maf_src            = SIG_MAF;

    // Every correction now carries its own enable, and the schema defaults them ON — a zeroed struct
    // is not the shipped configuration, so the fixture states what the defaults are rather than
    // testing an ECU with every correction switched off.
    c.enable_warmup = c.enable_cranking = c.enable_poststart = c.enable_iat = c.enable_map =
        c.enable_revlimit = c.enable_baro = c.enable_gear =
        c.enable_generic1 = c.enable_generic2 = c.enable_generic3 = c.enable_generic4 =
        c.enable_overall = 1;
    c.flood_clear_tps_pct = 90;      // …and flood clear is opt-in, like the shipped default

    // Live bin counts for the resizable axes (dense stride = these, not the max allocation).
    c.ve_table_x_axis_n=16; c.ve_table_y_axis_n=16; c.target_lambda_table_x_axis_n=16; c.target_lambda_table_y_axis_n=16; c.predicted_map_table_x_axis_n=16;  // base tables own their axes now
    c.clt_axis_n=14; c.clt_map_axis_n=8; c.ps_clt_axis_n=8; c.run_time_axis_n=8; c.iat_axis_n=14; c.iat_map_axis_n=6; c.rl_headroom_axis_n=6; c.rl_map_axis_n=6; c.cyl_load_axis_n=8; c.cyl_rpm_axis_n=8;  // resizable correction-axis live bins
    // Per-axis channel selectors are now per-table (auto-generated <table>_<ax>_src). Point each at the
    // channels the engine publishes; FuelCalc publishes rpm/fuel_load/run_time itself during update().
    c.ve_table_x_src = SIG_RPM; c.ve_table_y_src = SIG_FUEL_LOAD; c.ve_table_z_en = 0;
    c.target_lambda_table_x_src = SIG_RPM; c.target_lambda_table_y_src = SIG_FUEL_LOAD;
    c.predicted_map_table_x_src = SIG_RPM; c.predicted_map_table_y_src = SIG_TPS;
    c.clt_corr_table_x_src = SIG_CLT; c.clt_corr_table_y_src = SIG_MAP; c.clt_corr_table_y_en = 0;
    c.cranking_fuel_table_x_src = SIG_CLT; c.cranking_fuel_table_y_src = SIG_ETHANOL; c.cranking_fuel_table_y_en = 0;
    c.cf_x_axis_n = 14;   // the Y axis is fixed at 2 bins now (16 x 2), so it has no live count
    c.prime_fuel_table_x_src = SIG_CLT; c.prime_fuel_table_y_src = SIG_RUN_TIME; c.prime_fuel_table_y_en = 0;
    c.pp_x_axis_n = 14; c.pp_y_axis_n = 2;
    c.post_start_table_x_src = SIG_CLT; c.post_start_table_y_src = SIG_RUN_TIME;
    c.iat_corr_table_x_src = SIG_IAT; c.iat_corr_table_y_src = SIG_MAP;

    // 16x16 LIVE inside the physical allocation. Cells are addressed at the allocation's stride —
    // <axis>_n only says how much of the grid is in play — so a row starts at r * <table>_X_AXIS_ALLOC,
    // not at r * 16. Filling 0..255 flat left every row past the eighth empty.
    for (int r = 0; r < 16; r++)
        for (int cc = 0; cc < 16; cc++) {
            c.ve_table[r * FUEL_CALCULATOR_VE_TABLE_X_AXIS_ALLOC + cc] = 800;               // 80.0% (VE% x10)
            c.target_lambda_table[r * FUEL_CALCULATOR_TARGET_LAMBDA_TABLE_X_AXIS_ALLOC + cc] = 1000;  // 1.000 lambda
        }

    for (int i = 0; i < 16; i++) {
        c.ve_table_x_axis[i]=c.target_lambda_table_x_axis[i]=c.predicted_map_table_x_axis[i] = static_cast<float>(500 + i * 500);
        c.ve_table_y_axis[i]=c.target_lambda_table_y_axis[i] = static_cast<float>(10 + i * 6);    // real kPa breakpoints
    }

    for (int i = 0; i < 16; i++) {
        c.clt_corr_table[i] = 0;
        c.clt_axis[i]       = static_cast<float>(-40 + i * 10);
    }

    return c;
}

// Displacement + cylinder count are live tune fields now (g_config.engine.*); default_config seeds them
// to the schema defaults (2000 cc / 4 cyl), which is what these expectations are calibrated for.

static EnginePosition make_pos(float rpm = 3000.0f, bool synced = true) {
    EnginePosition p{};
    p.rpm = rpm;
    p.is_synchronized = synced;   // the prime waits for sync; everything else here assumes it
    p.sync_level = synced ? SyncLevel::PHASE : SyncLevel::NONE;
    return p;
}

static SignalBus make_bus(float map_kpa = 100.0f, float clt_c = 80.0f) {
    SignalBus bus{};
    bus.set(SIG_MAP, map_kpa);
    bus.set(SIG_CLT,   clt_c);
    return bus;
}

int main() {
    fprintf(stdout, "=== FuelCalculator ===\n");

    SECTION("basic PW — positive, non-zero at 100kPa 80%VE");
    {
        auto cfg = make_cfg();
        FuelCalculator fc;
        fc.init(cfg);
        EngineFrame frame{};
        { SignalBus _b = make_bus(100.0f); fc.update(make_pos(3000.0f), _b, frame); }
        CHECK(frame.base_fuel_pw_us > 0.0f);
        fprintf(stdout, "    base_fuel_pw_us = %g µs\n", (double)frame.base_fuel_pw_us);
    }

    SECTION("staged injection — above the duty threshold, fuel splits to the secondary");
    {
        // Baseline: a single stage -> all fuel on the primary, nothing staged.
        auto cfg0 = make_cfg();
        FuelCalculator fc0; fc0.init(cfg0);
        EngineFrame f0{};
        { SignalBus _b = make_bus(100.0f); fc0.update(make_pos(3000.0f), _b, f0); }
        const uint32_t prim0 = f0.cyl[0].inj_pw_us;
        CHECK(!f0.cyl[0].staged_enabled[0]);

        // Two stages: cap stage 1 at a LOW Staging Duty so it saturates early and hands the overflow to
        // stage 2 (a bigger injector). Staging Duty is a collapsible fuel table now — set its 1×1 cell on cfg.
        const uint8_t  save_n = g_config.engine.num_inj_stages;
        const uint8_t  save_o = g_config.engine.inj_stage[0].num_outputs;
        g_config.engine.num_inj_stages                = 2;
        g_config.engine.inj_stage[0].num_outputs      = 4;

        auto cfg = make_cfg();
        cfg.stage1_staging_duty_table[0] = 20;                       // 2% duty cap -> primary saturates at once
        for (unsigned i = 0; i < FUEL_CALCULATOR_STAGE2_INJ_FLOW_TABLE_ALLOC; i++) cfg.stage2_inj_flow_table[i] = 530;   // 2× primary flow
        FuelCalculator fc; fc.init(cfg);
        EngineFrame f{};
        { SignalBus _b = make_bus(100.0f); fc.update(make_pos(3000.0f), _b, f); }

        CHECK(f.cyl[0].staged_enabled[0]);            // stage 2 engaged by the overflow
        CHECK(f.cyl[0].staged_pw_us[0] > 0u);         // stage 2 delivering fuel
        CHECK(f.cyl[0].inj_pw_us < prim0);            // primary capped -> less than carrying it all
        fprintf(stdout, "    single: primary=%u  |  staged: primary=%u stage2=%u us\n",
                prim0, f.cyl[0].inj_pw_us, f.cyl[0].staged_pw_us[0]);

        g_config.engine.num_inj_stages                = save_n;
        g_config.engine.inj_stage[0].num_outputs      = save_o;
    }

    SECTION("saturation — once every stage is capped, they rise to 100% TOGETHER (not last-stage dump)");
    {
        // Model: past the point where every stage holds at its Staging Duty Cycle, all stages increase
        // their duty to 100% together. Discriminating check: run the SAME high demand twice, differing
        // only in stage 2's cap. If stage 2 can absorb everything (cap 100%), the primary stays pinned at
        // its own low cap. If stage 2 is ALSO capped low, the leftover forces BOTH up together, so the
        // primary rises ABOVE its cap. The old last-stage-uncapped model pinned the primary in both runs
        // (primary_lowcap == primary_hicap); rising-together makes primary_lowcap > primary_hicap.
        const uint8_t  save_n  = g_config.engine.num_inj_stages;
        const uint8_t  save_o  = g_config.engine.inj_stage[0].num_outputs;
        g_config.engine.num_inj_stages                = 2;
        g_config.engine.inj_stage[0].num_outputs      = 4;

        auto run = [&](uint16_t stage2_cap) -> uint32_t {
            auto cfg = make_cfg();
            cfg.stage1_staging_duty_table[0]           = 100;        // 10% cap on the primary
            cfg.stage2_staging_duty_table[0] = stage2_cap;
            for (unsigned i = 0; i < FUEL_CALCULATOR_STAGE2_INJ_FLOW_TABLE_ALLOC; i++) cfg.stage2_inj_flow_table[i] = 530;
            FuelCalculator fc; fc.init(cfg);
            EngineFrame f{};
            { SignalBus _b = make_bus(100.0f); fc.update(make_pos(6000.0f), _b, f); }   // WOT-ish, high demand
            return f.cyl[0].inj_pw_us;
        };
        const uint32_t primary_hicap  = run(1000);   // stage 2 can absorb all -> primary pinned at 10%
        const uint32_t primary_lowcap = run(100);    // stage 2 also capped at 10% -> both rise together
        CHECK(primary_lowcap > primary_hicap);
        fprintf(stdout, "    primary @ stage2 cap 100%%=%u  vs  stage2 cap 10%%=%u us (rise-together)\n",
                primary_hicap, primary_lowcap);

        g_config.engine.num_inj_stages                = save_n;
        g_config.engine.inj_stage[0].num_outputs      = save_o;
    }

    SECTION("a fuel cut does NOT zero the pulse width — it stops the injector");
    {
        // The cut is applied at the OUTPUT (EnginePositionHal::set_output_cuts clears the injector's
        // execution bit, so INJ_OPEN never drives it). The commanded pulse width goes on reporting
        // what the fuel model asked for. Zeroing it here told the tuner the model wanted no fuel when
        // it wanted plenty, and made a cut indistinguishable from a collapsed calculation.
        auto cfg = make_cfg();
        FuelCalculator fc;
        fc.init(cfg);
        EngineFrame cutf{}, runf{};
        { SignalBus _b = make_bus(100.0f); _b.set_bool(SIG_FUEL_CUT, true);
          fc.update(make_pos(3000.0f), _b, cutf); }
        { SignalBus _b = make_bus(100.0f); fc.update(make_pos(3000.0f), _b, runf); }
        for (int i = 0; i < MAX_CYLINDERS; i++)
            CHECK(cutf.cyl[i].inj_pw_us == runf.cyl[i].inj_pw_us);
        CHECK(cutf.cyl[0].inj_pw_us > 0u);
        // Staged secondary IS gated here: it is a separate injector decision, not an output cut.
        for (int i = 0; i < MAX_CYLINDERS; i++)
            CHECK(cutf.cyl[i].staged_enabled[0] == false);
    }

    SECTION("half MAP → roughly half PW");
    {
        auto cfg = make_cfg();
        FuelCalculator fc;
        fc.init(cfg);

        EngineFrame f100{};
        { SignalBus _b = make_bus(100.0f); fc.update(make_pos(3000.0f), _b, f100); }
        const float pw100 = f100.base_fuel_pw_us;

        EngineFrame f50{};
        { SignalBus _b = make_bus(50.0f); fc.update(make_pos(3000.0f), _b, f50); }
        const float pw50 = f50.base_fuel_pw_us;

        fprintf(stdout, "    pw@100kPa=%g  pw@50kPa=%g  ratio=%.2f\n",
                (double)pw100, (double)pw50, (double)(pw50 / pw100));
        CHECK(pw50 < pw100);
        CHECK(pw50 > 0.0f);
    }

    SECTION("charge-temp/IAT gas-law density: colder charge -> more fuel");
    {
        auto cfg = make_cfg();
        cfg.charge_temp_iat_pct = 1000;   // pure IAT
        FuelCalculator fc; fc.init(cfg);
        EngineFrame cold{}, hot{};
        { SignalBus b = make_bus(100.0f, 80.0f); b.set(SIG_IAT, 20.0f);
          fc.update(make_pos(3000.0f), b, cold); }
        { SignalBus b = make_bus(100.0f, 80.0f); b.set(SIG_IAT, 60.0f);
          fc.update(make_pos(3000.0f), b, hot); }
        const float ratio  = cold.base_fuel_pw_us / hot.base_fuel_pw_us;
        const float expect = (60.0f + 273.15f) / (20.0f + 273.15f);   // ~1.136 (T_hot/T_cold)
        fprintf(stdout, "    pw(IAT20)/pw(IAT60) = %.4f (expect %.4f)\n", (double)ratio, (double)expect);
        CHECK(cold.base_fuel_pw_us > hot.base_fuel_pw_us);
        CHECK_NEAR(ratio, expect, 0.02);
    }

    SECTION("corrections: dead-time published + external bus correction composes");
    {
        auto cfg = make_cfg();
        for (unsigned i = 0; i < FUEL_CALCULATOR_STAGE1_DEAD_TIME_TABLE_ALLOC; i++) cfg.stage1_dead_time_table[i] = 200;   // uniform 200 µs
        FuelCalculator fc; fc.init(cfg);
        EngineFrame base{}, prot{};
        { SignalBus b = make_bus(100.0f, 80.0f); b.set(SIG_IAT, 20.0f);
          fc.update(make_pos(3000.0f), b, base);
          CHECK_NEAR(b.get(SIG_PW_ADD_DEADTIME, 0.0f), 200.0, 0.5); }  // dead-time published as pw-add (stays per-cycle)
        { SignalBus b = make_bus(100.0f, 80.0f); b.set(SIG_IAT, 20.0f);
          b.set(SIG_FUEL_CORR_PROTECTION, 1.20f);   // an external producer: +20% enrichment
          fc.update(make_pos(3000.0f), b, prot); }
        const float bf = base.base_fuel_pw_us - 200.0f;   // strip the dead-time adder
        const float pf = prot.base_fuel_pw_us - 200.0f;
        fprintf(stdout, "    fuel-part ratio with prot 1.20 = %.4f (expect 1.20)\n", (double)(pf / bf));
        CHECK_NEAR(pf / bf, 1.20, 0.01);
    }

    SECTION("flex: ethanol shifts stoich richer (E85 ~= +49% fuel)");
    {
        auto cfg = make_cfg();
        FuelCalculator fc; fc.init(cfg);
        EngineFrame e0{}, e85{};
        { SignalBus b = make_bus(100.0f, 80.0f); b.set(SIG_IAT, 20.0f); b.set(SIG_ETHANOL, 0.0f);
          fc.update(make_pos(3000.0f), b, e0); }
        { SignalBus b = make_bus(100.0f, 80.0f); b.set(SIG_IAT, 20.0f); b.set(SIG_ETHANOL, 85.0f);
          fc.update(make_pos(3000.0f), b, e85); }
        const float stoich85 = 0.15f * 14.7f + 0.85f * 9.0f;
        const float ratio = e85.base_fuel_pw_us / e0.base_fuel_pw_us;
        fprintf(stdout, "    E85/E0 fuel ratio = %.3f (expect %.3f)\n", (double)ratio, (double)(14.7f / stoich85));
        CHECK_NEAR(ratio, 14.7f / stoich85, 0.02);
    }

    SECTION("flex: a dead sensor holds the last good reading, or the fallback — never 0 %");
    {
        // A dead flex sensor read as 0 % fuelled an E85 engine as petrol: a third lean, silently.
        auto cfg = make_cfg(); cfg.stage1_ethanol_pct = 85;
        auto pw = [&](FuelCalculator& fc, bool publish, float eth) {
            EngineFrame f{};
            SignalBus b = make_bus(100.0f, 80.0f); b.set(SIG_IAT, 20.0f);
            if (publish) b.set(SIG_ETHANOL, eth);
            fc.update(make_pos(3000.0f), b, f);
            return std::make_pair(f.base_fuel_pw_us, b.get(SIG_FLEX_ETHANOL, -1.0f));
        };
        FuelCalculator ref; ref.init(cfg);
        const float e85_pw = pw(ref, true, 85.0f).first;

        FuelCalculator never; never.init(cfg);        // sensor has never read: the fallback (85)
        const auto n = pw(never, false, 0.0f);
        CHECK_NEAR(n.second, 85.0, 0.01);
        CHECK_NEAR(n.first, e85_pw, e85_pw * 0.005);

        FuelCalculator held; held.init(cfg);          // read 70 %, then dropped out: holds 70 %
        cfg.stage1_ethanol_pct = 0;                   // (the fallback must NOT be what it uses now)
        pw(held, true, 70.0f);
        const auto h = pw(held, false, 0.0f);
        fprintf(stdout, "    dropout holds %.0f%%\n", (double)h.second);
        CHECK_NEAR(h.second, 70.0, 0.01);
    }

    SECTION("mixed fuels: stage 1 petrol + stage 2 fixed E85 burn the same air as the charge needs");
    {
        // Each stage delivers its OWN fuel. What must hold is the MIXTURE: the air the two fuels burn at
        // stoich (mass x stoich, summed) equals what the same charge needs on petrol alone. And the
        // charge's ethanol (flex_ethanol, next cycle) is the stages' contents weighted by fuel mass.
        const uint8_t save_n = g_config.engine.num_inj_stages, save_o = g_config.engine.inj_stage[0].num_outputs;
        auto base = make_cfg();
        FuelCalculator fb; fb.init(base);
        EngineFrame f0{};
        { SignalBus b = make_bus(100.0f, 80.0f); b.set(SIG_IAT, 20.0f); fb.update(make_pos(3000.0f), b, f0); }
        const double air_petrol = double(f0.cyl[0].inj_pw_us) * 265.0 * 0.740 * 14.7;

        g_config.engine.num_inj_stages = 2; g_config.engine.inj_stage[0].num_outputs = 4;
        auto cfg = make_cfg();
        cfg.stage1_staging_duty_table[0] = 20;                    // 2 % cap: stage 2 carries most of it
        for (unsigned i = 0; i < FUEL_CALCULATOR_STAGE2_INJ_FLOW_TABLE_ALLOC; i++) cfg.stage2_inj_flow_table[i] = 530;
        cfg.stage2_flex_enabled = 0; cfg.stage2_ethanol_pct = 85;  // a fixed E85 stage, no sensor
        cfg.stage2_specific_gravity_table[0] = 785;
        FuelCalculator fc; fc.init(cfg);
        EngineFrame f{};
        SignalBus b = make_bus(100.0f, 80.0f); b.set(SIG_IAT, 20.0f);
        fc.update(make_pos(3000.0f), b, f);
        const double m1 = double(f.cyl[0].inj_pw_us) * 265.0 * 0.740;
        const double m2 = double(f.cyl[0].staged_pw_us[0]) * 530.0 * 0.785;
        const double st2 = 0.15 * 14.7 + 0.85 * 9.0;
        const double air_mixed = m1 * 14.7 + m2 * st2;
        fprintf(stdout, "    air burnt: petrol-only %.0f, mixed %.0f (ratio %.4f)\n", air_petrol, air_mixed, air_mixed / air_petrol);
        CHECK(f.cyl[0].staged_pw_us[0] > 0u);
        CHECK_NEAR(air_mixed / air_petrol, 1.0, 0.01);
        CHECK_NEAR(b.get(SIG_STAGE2_ETHANOL, -1.0f), 85.0, 0.01);
        SignalBus b2 = make_bus(100.0f, 80.0f); b2.set(SIG_IAT, 20.0f);
        fc.update(make_pos(3000.0f), b2, f);                       // the blend lands next cycle
        const double blend = m2 * 85.0 / (m1 + m2);
        fprintf(stdout, "    charge ethanol %.1f%% (mass-weighted %.1f%%)\n", (double)b2.get(SIG_FLEX_ETHANOL, -1.0f), blend);
        CHECK_NEAR(b2.get(SIG_FLEX_ETHANOL, -1.0f), blend, 0.5);
        g_config.engine.num_inj_stages = save_n; g_config.engine.inj_stage[0].num_outputs = save_o;
    }

    SECTION("shared flex sensor: two flex stages on one sensor both read it");
    {
        auto cfg = make_cfg();
        cfg.stage2_flex_enabled = 1;                               // both on SIG_ETHANOL (the fixture's source)
        FuelCalculator fc; fc.init(cfg);
        EngineFrame f{};
        SignalBus b = make_bus(100.0f, 80.0f); b.set(SIG_IAT, 20.0f); b.set(SIG_ETHANOL, 70.0f);
        fc.update(make_pos(3000.0f), b, f);
        CHECK_NEAR(b.get(SIG_STAGE1_ETHANOL, -1.0f), 70.0, 0.01);
        CHECK_NEAR(b.get(SIG_STAGE2_ETHANOL, -1.0f), 70.0, 0.01);
        CHECK_NEAR(b.get(SIG_STAGE3_ETHANOL, -1.0f), 0.0, 0.01);   // not flex: its fixed Ethanol % (0)
    }

    SECTION("injector dead-time TABLE (inj pressure diff × battery voltage)");
    {
        auto cfg = make_cfg();
        const uint16_t grid[20] = {1400,1630,1810,1980, 980,1130,1280,1450, 640,740,850,1050,
                                   370,430,580,810, 190,260,440,640};   // rows V[8,10,12,14,16] × cols dp[200..500]
        for (int v = 0; v < 5; v++) for (int d = 0; d < 4; d++)   // 4 dp x 5 volt, at the physical stride
            cfg.stage1_dead_time_table[v * FUEL_CALCULATOR_DEAD_TIME_DP_AXIS_ALLOC + d] = grid[v * 4 + d];
        const float dp[4] = {200,300,400,500}; for (int i = 0; i < 4; i++) cfg.dead_time_dp_axis[i]   = dp[i];
        const float vv[5] = {8,10,12,14,16};   for (int i = 0; i < 5; i++) cfg.dead_time_volt_axis[i] = vv[i];
        cfg.dead_time_dp_axis_n = 4; cfg.dead_time_volt_axis_n = 5;
        cfg.stage1_dead_time_table_x_src = SIG_INJ_PRESS_DIFF; cfg.stage1_dead_time_table_y_src = SIG_BATTERY;
        cfg.stage1_fuel_press_mode = 0;   // this section is about the SENSOR path: dp = measured rail − manifold
        FuelCalculator fc; fc.init(cfg);
        EngineFrame f{};
        // inj_press_diff = fuel_press(400) − map(100) = 300 kPa (dp col 1)
        { SignalBus b = make_bus(100.0f, 80.0f); b.set(SIG_FUEL_PRESSURE, 400.0f); b.set(SIG_BATTERY, 10.0f);
          fc.update(make_pos(3000.0f), b, f);
          CHECK_NEAR(b.get(SIG_PW_ADD_DEADTIME, 0.0f), 1130.0, 1.0); }   // 10 V, dp300
        { SignalBus b = make_bus(100.0f, 80.0f); b.set(SIG_FUEL_PRESSURE, 400.0f); b.set(SIG_BATTERY, 14.0f);
          fc.update(make_pos(3000.0f), b, f);
          CHECK_NEAR(b.get(SIG_PW_ADD_DEADTIME, 0.0f), 430.0, 1.0); }    // 14 V, dp300
    }

    SECTION("injector flow TABLE: higher pressure differential -> more flow -> less PW");
    {
        auto cfg = make_cfg();
        const uint16_t flow[7] = {1270,1796,2200,2540,2840,3111,3361};   // cc/min over dp[100..700]
        for (int r = 0; r < 4; r++) for (int c = 0; c < 7; c++)
            cfg.stage1_inj_flow_table[r * FUEL_CALCULATOR_INJ_FLOW_DP_AXIS_ALLOC + c] = flow[c];
        const float dp[7] = {100,200,300,400,500,600,700}; for (int i = 0; i < 7; i++) cfg.inj_flow_dp_axis[i] = dp[i];
        cfg.inj_flow_dp_axis_n = 7; cfg.inj_flow_volt_axis_n = 4;
        cfg.stage1_inj_flow_table_x_src = SIG_INJ_PRESS_DIFF; cfg.stage1_inj_flow_table_y_src = SIG_BATTERY;
        cfg.stage1_fuel_press_mode = 0;   // sensor path again — the point is dP moving, which needs a rail reading
        FuelCalculator fc; fc.init(cfg);
        EngineFrame lo{}, hi{};
        // inj_press_diff = fuel_press − map(100): dp=150 (fp250) vs dp=600 (fp700)
        { SignalBus b = make_bus(100.0f, 80.0f); b.set(SIG_IAT, 20.0f); b.set(SIG_FUEL_PRESSURE, 250.0f);
          fc.update(make_pos(3000.0f), b, lo); }
        { SignalBus b = make_bus(100.0f, 80.0f); b.set(SIG_IAT, 20.0f); b.set(SIG_FUEL_PRESSURE, 700.0f);
          fc.update(make_pos(3000.0f), b, hi); }
        fprintf(stdout, "    PW@dp150=%g  PW@dp600=%g\n", (double)lo.base_fuel_pw_us, (double)hi.base_fuel_pw_us);
        CHECK(hi.base_fuel_pw_us < lo.base_fuel_pw_us);   // more flow at higher dP -> shorter PW
    }

    // WHAT HOLDS THE RAIL UP, and what the air is doing when nothing measures it. Three ways to know the
    // injector's differential pressure and two ways to know atmosphere, and the whole point is that only
    // one of the five needs a sensor fitted.
    SECTION("fuel pressure: sensor, fixed regulator, and a 1:1 rising rate");
    {
        auto cfg = make_cfg();
        cfg.stage1_inj_flow_table_x_src = SIG_INJ_PRESS_DIFF;
        FuelCalculator fc; fc.init(cfg);
        auto dp_at = [&](uint8_t mode, float map_kpa, bool publish_rail) {
            cfg.stage1_fuel_press_mode = mode;
            FuelCalculator c2; c2.init(cfg);
            SignalBus b = make_bus(map_kpa, 80.0f);
            b.set(SIG_IAT, 20.0f);
            if (publish_rail) b.set(SIG_FUEL_PRESSURE, 450.0f);
            EngineFrame ff{};
            c2.update(make_pos(3000.0f), b, ff);
            return b.get(SIG_INJ_PRESS_DIFF, 0.0f);
        };
        // SENSOR: measured rail minus manifold, and nothing else comes into it.
        CHECK_NEAR(dp_at(0, 100.0f, true), 350.0, 0.5);      // 450 − 100
        // FIXED REGULATOR: the rail is held at base ABOVE ATMOSPHERE, so the differential falls as the
        // manifold rises — 300 at atmosphere, 100 less under a bar of boost.
        CHECK_NEAR(dp_at(1, 101.3f, false), 300.0, 0.5);
        CHECK_NEAR(dp_at(1, 201.3f, false), 200.0, 0.5);
        // RISING RATE: a 1:1 regulator references the manifold, so the differential is base WHATEVER the
        // manifold does. This is the mode that is exactly right with no pressure sensor of any kind.
        CHECK_NEAR(dp_at(2, 101.3f, false), 300.0, 0.5);
        CHECK_NEAR(dp_at(2, 250.0f, false), 300.0, 0.5);
    }

    SECTION("rising rate: 1:1 holds the differential, a real ratio raises it with boost");
    {
        auto cfg = make_cfg();
        cfg.stage1_fuel_press_mode = 2; cfg.stage1_fuel_press_base_kpa = 3000;
        auto dp_at = [&](uint8_t ratio_x10, float map_kpa) {
            cfg.stage1_fuel_press_ratio = ratio_x10;
            FuelCalculator c2; c2.init(cfg);
            SignalBus b = make_bus(map_kpa, 80.0f); b.set(SIG_IAT, 20.0f);
            EngineFrame ff{}; c2.update(make_pos(3000.0f), b, ff);
            return b.get(SIG_INJ_PRESS_DIFF, 0.0f);
        };
        // 1.0:1 — manifold-referenced. The rail follows the manifold exactly, so the differential is the
        // base pressure at idle, at atmosphere and at two bar of boost alike.
        CHECK_NEAR(dp_at(10,  30.0f), 300.0, 0.5);
        CHECK_NEAR(dp_at(10, 101.3f), 300.0, 0.5);
        CHECK_NEAR(dp_at(10, 301.3f), 300.0, 0.5);
        // 6.0:1 — the rail climbs six per one, so the differential gains FIVE per unit of boost.
        CHECK_NEAR(dp_at(60, 101.3f), 300.0, 0.5);            // no boost, no rise
        CHECK_NEAR(dp_at(60, 201.3f), 800.0, 0.5);            // +100 boost -> +500
        // VACUUM DOES NOT GET AMPLIFIED. The rail itself still tracks the manifold down — that is what
        // a constant differential MEANS, and it is why sucking on the reference hose drops the gauge
        // reading. What must not happen is the RATIO acting on vacuum: 300 + 5 x (30 - 101.3) is a
        // differential of -56 kPa, i.e. fuel flowing backwards up the rail.
        CHECK_NEAR(dp_at(60,  30.0f), 300.0, 0.5);
    }

    SECTION("each stage is on its own rail, with its own regulator");
    {
        // A second set of injectors on a second rail is the ordinary reason to stage at all — port and
        // direct, petrol and methanol, a big secondary set on its own pump. Every stage indexed on the
        // primary's differential before this, which is right only while they share a rail.
        auto cfg = make_cfg();
        cfg.stage1_fuel_press_mode = 1; cfg.stage1_fuel_press_base_kpa = 3000;   // fixed 300 kPa
        cfg.stage2_fuel_press_mode = 2; cfg.stage2_fuel_press_base_kpa = 5000;   // 1:1 rising, 500 kPa
        cfg.stage3_fuel_press_mode = 0; cfg.stage3_fuel_press_src = SIG_AUX_1;   // its own transducer
        cfg.stage4_fuel_press_mode = 1; cfg.stage4_fuel_press_base_kpa = 2000;   // fixed 200 kPa
        FuelCalculator fc; fc.init(cfg);
        EngineFrame f{};
        SignalBus b = make_bus(201.3f, 80.0f);        // a bar of boost, so Fixed and Rising diverge
        b.set(SIG_IAT, 20.0f); b.set(SIG_AUX_1, 600.0f);
        fc.update(make_pos(3000.0f), b, f);
        CHECK_NEAR(b.get(SIG_INJ_PRESS_DIFF,   0.0f), 200.0, 0.5);   // 300 + 101.3 − 201.3, boost eats 100
        CHECK_NEAR(b.get(SIG_INJ_PRESS_DIFF_2, 0.0f), 500.0, 0.5);   // 1:1 — boost changes nothing
        CHECK_NEAR(b.get(SIG_INJ_PRESS_DIFF_3, 0.0f), 398.7, 0.5);   // 600 measured − 201.3 manifold
        CHECK_NEAR(b.get(SIG_INJ_PRESS_DIFF_4, 0.0f), 100.0, 0.5);   // 200 + 101.3 − 201.3
    }

    SECTION("no MAP sensor reads as ATMOSPHERE, not as a hard vacuum");
    {
        // Alpha-N with no manifold sensor at all — the configuration the model exists for. map_src is
        // left pointing at a channel nobody publishes, which is what an unwired MAP looks like.
        auto cfg = make_cfg();
        cfg.fuel_model = 1; cfg.charge_temp_iat_pct = 1000;
        cfg.stage1_inj_flow_table_x_src = SIG_INJ_PRESS_DIFF;
        FuelCalculator fc; fc.init(cfg);
        EngineFrame f{};
        SignalBus b;                                  // nothing published: no MAP, no baro, no rail
        b.set(SIG_CLT, 80.0f); b.set(SIG_IAT, 20.0f); b.set(SIG_TPS, 100.0f); b.set(SIG_BATTERY, 14.0f);
        fc.update(make_pos(3000.0f), b, f);
        // The manifold defaulted to 0 kPa — a hard vacuum — which put the differential a whole bar out
        // and read the flow table in the wrong place entirely. It is atmosphere now, so a fixed
        // regulator's differential is simply its base pressure.
        CHECK_NEAR(b.get(SIG_INJ_PRESS_DIFF, 0.0f), 300.0, 0.5);
        CHECK(f.base_fuel_pw_us > 0.0f);              // and it still makes fuel
        // …and the assumed barometric pressure is PUBLISHED, so the baro correction table (which indexes
        // on this channel) reads the same number the density did instead of nothing at all.
        CHECK_NEAR(b.get(SIG_BARO_KPA, 0.0f), 101.3, 0.2);
    }

    SECTION("speed-density with a FAILED MAP reads atmosphere — never the predicted table");
    {
        // The predicted table is for transients, and only when prediction is on. It was also used as a
        // failover, switched on or not: a disabled MAP sensor fuelled on an untuned 25 kPa and every
        // reading looked plausible. A failed MAP reads the assumed baro (rich at idle: the safe side).
        for (int enabled = 0; enabled <= 1; ++enabled) {
            auto cfg = make_cfg();
            cfg.fuel_model = 0; cfg.map_predict_enabled = static_cast<uint8_t>(enabled);
            for (auto& v : cfg.predicted_map_table) v = 350;     // 35.0 kPa everywhere
            FuelCalculator fc; fc.init(cfg);
            EngineFrame f{};
            SignalBus b;                                          // MAP configured but not publishing
            b.set(SIG_CLT, 80.0f); b.set(SIG_IAT, 20.0f); b.set(SIG_TPS, 2.0f); b.set(SIG_BATTERY, 14.0f);
            fc.update(make_pos(800.0f), b, f);
            fprintf(stdout, "    prediction %s: map used = %.1f kPa, source = %.0f\n", enabled ? "on " : "off",
                    (double)b.get(SIG_MAP_EST, -1.0f), (double)b.get(SIG_MAP_SOURCE, -1.0f));
            CHECK_NEAR(b.get(SIG_MAP_SOURCE, -1.0f), 2.0, 0.01);  // failed
            CHECK_NEAR(b.get(SIG_MAP_EST, 0.0f), 101.3, 0.2);     // atmosphere, not the table's 35
        }
    }

    SECTION("MAF model with a dead MAF falls back to speed-density, not to zero fuel");
    {
        auto sd  = make_cfg(); sd.fuel_model = 0;
        auto maf = make_cfg(); maf.fuel_model = 2; maf.maf_src = SIG_MAF;
        FuelCalculator a; a.init(sd);
        FuelCalculator m; m.init(maf);
        EngineFrame fa{}, fm{};
        { SignalBus b = make_bus(100.0f, 80.0f); b.set(SIG_IAT, 20.0f); a.update(make_pos(3000.0f), b, fa); }
        float failover = 0.0f;
        { SignalBus b = make_bus(100.0f, 80.0f); b.set(SIG_IAT, 20.0f);   // no SIG_MAF published
          m.update(make_pos(3000.0f), b, fm); failover = b.get(SIG_MAF_FAILOVER, 0.0f); }
        fprintf(stdout, "    SD pw=%.0f  MAF-dead pw=%.0f\n", (double)fa.base_fuel_pw_us, (double)fm.base_fuel_pw_us);
        CHECK_NEAR(failover, 1.0, 0.01);
        CHECK_NEAR(fm.base_fuel_pw_us, fa.base_fuel_pw_us, fa.base_fuel_pw_us * 0.001);
    }

    SECTION("a live barometric sensor beats the assumption");
    {
        auto cfg = make_cfg();
        cfg.fuel_model = 1; cfg.charge_temp_iat_pct = 1000;
        cfg.baro_assumed_kpa = 1013;                  // sea level assumed…
        FuelCalculator fc; fc.init(cfg);
        EngineFrame lo{}, hi{};
        { SignalBus b; b.set(SIG_CLT, 80.0f); b.set(SIG_IAT, 20.0f); b.set(SIG_TPS, 100.0f);
          fc.update(make_pos(3000.0f), b, lo); }
        FuelCalculator fc2; fc2.init(cfg);
        { SignalBus b; b.set(SIG_CLT, 80.0f); b.set(SIG_IAT, 20.0f); b.set(SIG_TPS, 100.0f);
          b.set(SIG_BARO_KPA, 80.0f);                 // …but the car is up a mountain and says so
          fc2.update(make_pos(3000.0f), b, hi); }
        // Thinner air, less mass, less fuel. The sensor wins whenever it is publishing.
        CHECK(hi.base_fuel_pw_us < lo.base_fuel_pw_us);
    }

    // Acceleration enrichment moved to its own 1 kHz module (E-2d) — see test_accel_enrich.

    SECTION("E-2b: recomputes every call (decimation is now the per-cycle TASK cadence, not internal)");
    {
        // The Stage-E internal cache/cycle-count gate is gone — FuelCalc now recomputes on every
        // update() because the per-cycle task only calls it once per cycle. So a MAP change is
        // reflected immediately, regardless of cycle_count.
        auto cfg = make_cfg();
        FuelCalculator fc; fc.init(cfg);
        EngineFrame f{};
        auto syncpos = [](uint32_t cyc) { EnginePosition p{}; p.rpm = 3000.0f;
                                          p.is_synchronized = true; p.cycle_count = cyc; return p; };
        { SignalBus b = make_bus(100.0f, 80.0f); b.set(SIG_IAT, 20.0f); fc.update(syncpos(1), b, f); }
        const float pw1 = f.base_fuel_pw_us;
        // SAME cycle_count, doubled MAP -> still recomputes (no internal cache) -> ~2x
        { SignalBus b = make_bus(200.0f, 80.0f); b.set(SIG_IAT, 20.0f); fc.update(syncpos(1), b, f); }
        fprintf(stdout, "    pw @MAP100=%.0f, @MAP200 same cycle_count=%.0f (recomputed, not cached)\n",
                (double)pw1, (double)f.base_fuel_pw_us);
        CHECK(f.base_fuel_pw_us > pw1 * 1.5f);
    }

    SECTION("Stage F Alpha-N: uses TPS+baro, ignores MAP=0; equals SD at same pressure");
    {
        // SD at MAP=101.3 vs Alpha-N at TPS=100% (load picks the same top bin), MAP=0.
        // Same VE (uniform 80%), same density (SD MAP 101.3 == Alpha-N baro 101.3) -> equal air mass.
        auto cfg = make_cfg(); cfg.charge_temp_iat_pct = 1000;
        FuelCalculator fc; fc.init(cfg);
        float air_sd = 0.0f, air_an = 0.0f, pw_an = 0.0f;
        EngineFrame fsd{}, fan{};
        { SignalBus b = make_bus(101.3f, 80.0f); b.set(SIG_IAT, 20.0f);
          fc.update(make_pos(3000.0f), b, fsd); air_sd = b.get(SIG_AIR_MASS, 0.0f); }

        auto cfg2 = make_cfg(); cfg2.charge_temp_iat_pct = 1000; cfg2.fuel_model = 1;
        FuelCalculator fc2; fc2.init(cfg2);
        { SignalBus b = make_bus(0.0f, 80.0f); b.set(SIG_IAT, 20.0f); b.set(SIG_TPS, 100.0f);
          fc2.update(make_pos(3000.0f), b, fan); air_an = b.get(SIG_AIR_MASS, 0.0f); pw_an = fan.base_fuel_pw_us; }

        fprintf(stdout, "    air_mass SD@MAP101=%.2f  Alpha-N@TPS100,MAP0=%.2f  pw_an=%.0f\n",
                (double)air_sd, (double)air_an, (double)pw_an);
        CHECK(air_sd > 0.0f);
        CHECK(pw_an > 0.0f);                       // Alpha-N makes real fuel even with MAP=0
        CHECK_NEAR(air_an, air_sd, air_sd * 0.02); // TPS+baro path matches the MAP path at equal pressure
    }

    SECTION("Stage F MAF: air mass = measured airflow / induction events, ignores MAP");
    {
        // 4-cyl @ 3000 rpm -> events/s = (3000/60)*(4/2) = 100. MAF 20 g/s -> 200 mg/event.
        auto cfg = make_cfg(); cfg.fuel_model = 2;
        FuelCalculator fc; fc.init(cfg);
        EngineFrame f{};
        float air = 0.0f;
        { SignalBus b = make_bus(0.0f, 80.0f); b.set(SIG_MAF, 20.0f);   // MAP=0 to prove it's ignored
          fc.update(make_pos(3000.0f), b, f); air = b.get(SIG_AIR_MASS, 0.0f); }
        fprintf(stdout, "    MAF 20g/s @3000rpm 4cyl -> air_mass=%.2f mg (expect 200), pw=%.0f\n",
                (double)air, (double)f.base_fuel_pw_us);
        CHECK_NEAR(air, 200.0, 1.0);
        CHECK(f.base_fuel_pw_us > 0.0f);
    }

    SECTION("SD air mass is physical + agrees with MAF (no phantom /2)");
    {
        // 2000cc 4-cyl @ 3000rpm, VE=100%, MAP=100kPa, charge 20C: one cylinder ingests 500cc of air at
        // density 100e3/(287.05*293.15) = 1.1884 mg/cc = 594.2 mg/event. Half that (297) was the old /2 bug.
        auto cfg = make_cfg();
        for (unsigned i = 0; i < FUEL_CALCULATOR_VE_TABLE_ALLOC; i++) cfg.ve_table[i] = 1000;   // 100.0% VE
        cfg.charge_temp_iat_pct = 1000;                             // pure-IAT charge temp
        FuelCalculator fc; fc.init(cfg);
        EngineFrame f{}; float air_sd = 0.0f;
        { SignalBus b = make_bus(100.0f, 80.0f); b.set(SIG_IAT, 20.0f);
          fc.update(make_pos(3000.0f), b, f); air_sd = b.get(SIG_AIR_MASS, 0.0f); }
        fprintf(stdout, "    SD air @VE100/MAP100/20C = %.1f mg (physical 594.2; old /2 bug = 297)\n", (double)air_sd);
        CHECK_NEAR(air_sd, 594.2, 3.0);

        // Feed the MAF the airflow that swept volume implies -> both models land on the same per-event mass.
        // events/s = (3000/60)*(4/2) = 100, so maf = 594.2 mg/event * 100 / 1000 = 59.42 g/s.
        auto cfgm = make_cfg(); cfgm.fuel_model = 2;
        FuelCalculator fm; fm.init(cfgm);
        EngineFrame fmf{}; float air_maf = 0.0f;
        { SignalBus b = make_bus(0.0f, 80.0f); b.set(SIG_MAF, 59.42f);
          fm.update(make_pos(3000.0f), b, fmf); air_maf = b.get(SIG_AIR_MASS, 0.0f); }
        fprintf(stdout, "    MAF air @59.42g/s = %.1f mg (SD and MAF now consistent)\n", (double)air_maf);
        CHECK_NEAR(air_maf, air_sd, 2.0);
    }

    SECTION("Blend (fuel_model=3): two maps — Alpha-N air at baro crossfades into MAP air, smoothly");
    {
        // The VE table (80 %, fixture) on MEASURED MAP 50 kPa; the Alpha-N VE table (60 %, RPM x throttle)
        // at BARO 101.3 kPa. Below the band the charge is all Alpha-N, above it all MAP, and the AIR
        // MASSES (not the VE numbers) are mixed between — so the middle is their exact average and the
        // sweep through the band has no step at either end. The Predicted MAP table is not read at all.
        auto cfg = make_cfg(); cfg.charge_temp_iat_pct = 1000;
        cfg.fuel_model = 3; cfg.blend_rpm_lo = 2000; cfg.blend_rpm_hi = 4000;
        cfg.alpha_ve_table_x_src = SIG_RPM; cfg.alpha_ve_table_y_src = SIG_TPS;
        cfg.alpha_ve_table_x_axis_n = 2; cfg.alpha_ve_table_y_axis_n = 2;
        cfg.alpha_ve_table_x_axis[0] = 500;  cfg.alpha_ve_table_x_axis[1] = 8000;
        cfg.alpha_ve_table_y_axis[0] = 0;    cfg.alpha_ve_table_y_axis[1] = 100;
        for (unsigned i = 0; i < FUEL_CALCULATOR_ALPHA_VE_TABLE_ALLOC; i++) cfg.alpha_ve_table[i] = 600;   // 60.0 %
        for (unsigned i = 0; i < FUEL_CALCULATOR_PREDICTED_MAP_TABLE_ALLOC; i++) cfg.predicted_map_table[i] = 3000; // must not matter
        FuelCalculator fc; fc.init(cfg);
        auto at = [&](float rpm){ EngineFrame f{}; SignalBus b = make_bus(50.0f, 80.0f);
            b.set(SIG_IAT, 20.0f); b.set(SIG_TPS, 20.0f); fc.update(make_pos(rpm), b, f);
            return std::make_pair(b.get(SIG_AIR_MASS, 0.0f), b.get(SIG_CHARGE_LOAD, -1.0f)); };
        const auto lo = at(1000.0f), mid = at(3000.0f), hi = at(5000.0f);
        fprintf(stdout, "    air  @1000 %.1f mg (load %.1f%%)  @3000 %.1f  @5000 %.1f mg (load %.1f%%)\n",
                (double)lo.first, (double)lo.second, (double)mid.first, (double)hi.first, (double)hi.second);
        CHECK_NEAR(lo.second, 60.0, 0.5);                           // Alpha-N: 60 % VE at baro
        CHECK_NEAR(hi.second, 80.0 * 50.0 / 101.3, 0.5);            // MAP: 80 % VE at 50 of 101.3 kPa
        CHECK_NEAR(mid.first, (lo.first + hi.first) / 2.0f, lo.first * 0.01f);   // air averaged, not VE
        float worst = 0.0f, prev = at(1500.0f).first;
        for (float r = 1550.0f; r <= 4500.0f; r += 50.0f) {         // no step at 2000 or 4000
            const float a = at(r).first; worst = std::max(worst, std::fabs(a - prev)); prev = a;
        }
        const float per_step = std::fabs(lo.first - hi.first) / 40.0f;   // the band is 40 steps of 50 rpm
        fprintf(stdout, "    largest step through the sweep %.3f mg (a smooth ramp is %.3f)\n", (double)worst, (double)per_step);
        CHECK(worst <= per_step * 1.05f);
    }

    SECTION("charge load: speed-density = VE x MAP / baro");
    {
        auto cfg = make_cfg(); cfg.charge_temp_iat_pct = 1000;
        FuelCalculator fc; fc.init(cfg);
        EngineFrame f{}; SignalBus b = make_bus(70.0f, 80.0f); b.set(SIG_IAT, 20.0f);
        fc.update(make_pos(3000.0f), b, f);
        CHECK_NEAR(b.get(SIG_CHARGE_LOAD, -1.0f), 80.0 * 70.0 / 101.3, 0.5);
        CHECK_NEAR(b.get(SIG_BLEND_ALPHA_SHARE, -1.0f), 0.0, 0.01);   // outside Blend the Alpha-N map has none of it
    }

    SECTION("VE telemetry field matches 80 configured");
    {
        auto cfg = make_cfg();
        FuelCalculator fc;
        fc.init(cfg);
        EngineFrame frame{};
        { SignalBus _b = make_bus(100.0f); fc.update(make_pos(3000.0f), _b, frame); }
        CHECK_NEAR(frame.ve_pct, 80.0, 1.0);
    }

    // NOTE: the CLT-warmup table→fuel_corr_warmup transform (scale + range) moved to FuelTrim — see
    // test_fuel_trim. FuelCalculator just APPLIES whatever warmup is on the bus (covered by the
    // "external bus correction composes" section above, which drives fuel_corr_protection through the
    // same m() aggregation).

    SECTION("wall-film (X-τ): tip-in over-injects to fill the film, settles to steady");
    {
        auto cfg = make_cfg(); cfg.charge_temp_iat_pct = 1000;
        cfg.wallfilm_enabled = 1; cfg.wallfilm_x_pct = 250; cfg.wallfilm_tau_ms = 200;
        FuelCalculator fc; fc.init(cfg);
        EngineFrame f{};
        auto run = [&](float map, uint32_t ms){ g_test_ms = ms; SignalBus b = make_bus(map, 80.0f);
            b.set(SIG_IAT, 20.0f);
            b.set(SIG_ENGINE_STATE, static_cast<float>(EngineRunState::RUNNING));  // wall-film only models while running
            fc.update(make_pos(3000.0f), b, f); return f.base_fuel_pw_us; };
        // settle at MAP=100 (~1.5 s of 20 ms cycles, ~7τ) -> film equilibrium, injected == desired
        float steady100 = 0; for (uint32_t t = 20; t <= 1500; t += 20) steady100 = run(100.0f, t);
        // step to MAP=150: first cycle over-injects (film still sized for 100), then settles
        const float spike = run(150.0f, 1520);
        float steady150 = 0; for (uint32_t t = 1540; t <= 3000; t += 20) steady150 = run(150.0f, t);
        fprintf(stdout, "    steady@100=%.0f  tip-in spike=%.0f  steady@150=%.0f\n",
                (double)steady100, (double)spike, (double)steady150);
        CHECK(spike > steady150 * 1.05f);              // tip-in over-injects vs the new steady
        CHECK(steady150 > steady100 * 1.4f);           // higher load -> more fuel (~1.5x)
    }

    SECTION("wall-film: a fuel cut dries the walls, so fuel returning re-wets them (over-injects)");
    {
        // During a cut nothing is injected; the film only evaporates. It used to go on depositing as if
        // fuel were flowing, so on resume the model thought the walls were wet and added nothing.
        auto cfg = make_cfg(); cfg.charge_temp_iat_pct = 1000;
        cfg.wallfilm_enabled = 1; cfg.wallfilm_x_pct = 250; cfg.wallfilm_tau_ms = 200;
        FuelCalculator fc; fc.init(cfg);
        EngineFrame f{};
        auto run = [&](uint32_t ms, bool cut){ g_test_ms = ms; SignalBus b = make_bus(100.0f, 80.0f);
            b.set(SIG_IAT, 20.0f);
            b.set(SIG_ENGINE_STATE, static_cast<float>(EngineRunState::RUNNING));
            if (cut) b.set_bool(SIG_FUEL_CUT, true, ms, 100);
            fc.update(make_pos(3000.0f), b, f); return f.base_fuel_pw_us; };
        float steady = 0; for (uint32_t t = 20; t <= 1500; t += 20) steady = run(t, false);
        for (uint32_t t = 1520; t <= 3000; t += 20) run(t, true);        // 1.5 s overrun: walls dry
        const float resume = run(3020, false);
        fprintf(stdout, "    steady=%.0f  first pulse after the cut=%.0f\n", (double)steady, (double)resume);
        CHECK(resume > steady * 1.10f);                  // re-wetting the walls, not assuming them wet
    }

    SECTION("post-start enrichment only once the engine has caught, not while cranking");
    {
        auto cfg = make_cfg(); cfg.enable_poststart = 1;
        for (auto& v : cfg.post_start_table) v = 500;    // +50 % everywhere, incl. run_time 0
        FuelCalculator fc; fc.init(cfg);
        EngineFrame f{};
        { SignalBus b = make_bus(100.0f, 20.0f); b.set(SIG_ENGINE_STATE, static_cast<float>(EngineRunState::CRANKING));
          fc.update(make_pos(250.0f), b, f);
          CHECK_NEAR(b.get(SIG_FUEL_CORR_POSTSTART, 0.0f), 1.0, 0.001); }   // cranking: none
        fc.on_engine_start();
        { SignalBus b = make_bus(100.0f, 20.0f); b.set(SIG_ENGINE_STATE, static_cast<float>(EngineRunState::RUNNING));
          fc.update(make_pos(900.0f), b, f);
          CHECK(b.get(SIG_FUEL_CORR_POSTSTART, 0.0f) > 1.3f); }             // caught: it applies
    }

    SECTION("cranking enrichment: gated to CRANKING, absolute-% multiplier, stacks like a normal factor");
    {
        auto cfg = make_cfg();
        for (unsigned i = 0; i < FUEL_CALCULATOR_CRANKING_FUEL_TABLE_ALLOC; i++) cfg.cranking_fuel_table[i] = 2000;   // uniform 200.0% (×2.0)
        FuelCalculator fc; fc.init(cfg);
        EngineFrame f{};
        auto pw = [&](EngineRunState st){ SignalBus b = make_bus(100.0f, 80.0f);
            b.set(SIG_ENGINE_STATE, static_cast<float>(st));
            fc.update(make_pos(300.0f), b, f); return f.base_fuel_pw_us; };
        const float running  = pw(EngineRunState::RUNNING);    // gate off -> cranking factor ×1.0
        const float cranking = pw(EngineRunState::CRANKING);   // gate on  -> ×2.0
        fprintf(stdout, "    running=%.1f  cranking=%.1f (expect ~2x)\n", (double)running, (double)cranking);
        CHECK(running > 0.0f);
        CHECK_NEAR(cranking, running * 2.0, running * 0.02);   // 200% cranking table = double fuel
        // and the published factor matches: ×2.0 cranking, neutral running
        { SignalBus b = make_bus(100.0f, 80.0f); b.set(SIG_ENGINE_STATE, static_cast<float>(EngineRunState::CRANKING));
          fc.update(make_pos(300.0f), b, f); CHECK_NEAR(b.get(SIG_FUEL_CORR_CRANKING, 1.0f), 2.0, 1e-4); }
        { SignalBus b = make_bus(100.0f, 80.0f); b.set(SIG_ENGINE_STATE, static_cast<float>(EngineRunState::RUNNING));
          fc.update(make_pos(300.0f), b, f); CHECK_NEAR(b.get(SIG_FUEL_CORR_CRANKING, 0.0f), 1.0, 1e-4); }
    }

    SECTION("prime pulse: ms mode = cell x1000, fires once per power-up, gated to CRANKING");
    {
        auto cfg = make_cfg();
        cfg.prime_enable = 1; cfg.prime_mode = 0;                       // Injection Time (ms)
        for (unsigned i = 0; i < FUEL_CALCULATOR_PRIME_FUEL_TABLE_ALLOC; i++) cfg.prime_fuel_table[i] = 5;  // uniform 5 ms
        FuelCalculator fc; fc.init(cfg);
        EngineFrame f{};
        // COLD, because a prime is for a cold engine: this fixture used to crank at 80 C, which is a warm
        // restart and the one case that must NOT prime (see the section below).
        auto cyc = [&](EngineRunState st){ SignalBus b = make_bus(100.0f, 20.0f);
            b.set(SIG_ENGINE_STATE, static_cast<float>(st));
            fc.update(make_pos(250.0f), b, f); return f.prime_pw_us; };
        CHECK(cyc(EngineRunState::RUNNING)  == 0u);       // not cranking -> no prime
        CHECK(cyc(EngineRunState::CRANKING) == 5000u);    // 5 ms -> 5000 µs, fires
        CHECK(cyc(EngineRunState::CRANKING) == 0u);       // latched: never re-primes this power cycle
    }

    SECTION("prime pulse: waits for SYNC, so keying on and off does not build fuel");
    {
        // THE RULE: the prime is the first fuel the engine gets when it cranks and the decoder locks. It
        // is not a squirt at key-on — without the sync condition, every key-on that turned the engine even
        // slightly delivered one, and a session of key on/off while tuning put liquid fuel in the ports
        // with nothing burning it off.
        auto cfg = make_cfg();
        cfg.prime_enable = 1; cfg.prime_mode = 0;
        for (unsigned i = 0; i < FUEL_CALCULATOR_PRIME_FUEL_TABLE_ALLOC; i++) cfg.prime_fuel_table[i] = 5;
        FuelCalculator fc; fc.init(cfg);
        EngineFrame f{};
        auto turn = [&](EngineRunState st, bool synced){
            SignalBus b = make_bus(100.0f, 20.0f);
            b.set(SIG_ENGINE_STATE, static_cast<float>(st));
            fc.update(make_pos(250.0f, synced), b, f); return f.prime_pw_us; };

        // Cranking without sync — the starter turning it over before the decoder locks, and every
        // key-on-and-off in between. Nothing is delivered, however many times it happens.
        for (int i = 0; i < 5; ++i) CHECK(turn(EngineRunState::CRANKING, false) == 0u);
        CHECK(turn(EngineRunState::STOPPED, false) == 0u);
        // …and the moment sync arrives while cranking, THAT is the prime.
        CHECK(turn(EngineRunState::CRANKING, true) == 5000u);
        CHECK(turn(EngineRunState::CRANKING, true) == 0u);   // once per power-up, as before
    }

    SECTION("prime pulse: disabled -> never fires");
    {
        auto cfg = make_cfg(); cfg.prime_enable = 0; cfg.prime_mode = 0;
        for (unsigned i = 0; i < FUEL_CALCULATOR_PRIME_FUEL_TABLE_ALLOC; i++) cfg.prime_fuel_table[i] = 5;
        FuelCalculator fc; fc.init(cfg);
        EngineFrame f{}; SignalBus b = make_bus(100.0f, 80.0f);
        b.set(SIG_ENGINE_STATE, static_cast<float>(EngineRunState::CRANKING));
        fc.update(make_pos(250.0f), b, f);
        CHECK(f.prime_pw_us == 0u);
    }

    SECTION("prime pulse: VE mode scales linearly with the table %");
    {
        auto mk = [&](uint16_t pct){ auto cfg = make_cfg(); cfg.prime_enable = 1; cfg.prime_mode = 1;
            for (unsigned i = 0; i < FUEL_CALCULATOR_PRIME_FUEL_TABLE_ALLOC; i++) cfg.prime_fuel_table[i] = pct;
            FuelCalculator fc; fc.init(cfg); EngineFrame f{};
            SignalBus b = make_bus(100.0f, 20.0f);              // cold, so the prime is not gated out
            b.set(SIG_ENGINE_STATE, static_cast<float>(EngineRunState::CRANKING));
            fc.update(make_pos(250.0f), b, f); return f.prime_pw_us; };
        const uint32_t p100 = mk(100), p200 = mk(200);
        fprintf(stdout, "    VE prime: 100%%=%u us  200%%=%u us\n", p100, p200);
        CHECK(p100 > 0u);
        CHECK_NEAR((double)p200, (double)p100 * 2.0, (double)p100 * 0.02);   // linear in table %
    }

    SECTION("injection angle (from the 2D table) propagates to all cylinders");
    {
        auto cfg = make_cfg();
        for (unsigned i = 0; i < FUEL_CALCULATOR_STAGE1_INJ_ANGLE_TABLE_ALLOC; i++) cfg.stage1_inj_angle_table[i] = 3550;   // uniform 355.0° BTDC
        FuelCalculator fc;
        fc.init(cfg);
        EngineFrame frame{};
        { SignalBus _b = make_bus(100.0f); fc.update(make_pos(3000.0f), _b, frame); }
        for (int i = 0; i < 4; i++)
            CHECK(frame.cyl[i].inj_btdc_x10 == 3550);   // table value, same for all cylinders
    }

    SECTION("table — x=rpm(cols) maps right, no transpose");
    {
        // 16 rpm columns × 16 load rows. Cell value encodes (rpm_idx, load_idx)
        // so a transposed read (or swapped axes) returns a different number.
        FuelCalculatorConfig c{};
        for (int i=0;i<28;i++) c.stage1_inj_flow_table[i]=265;
        c.stage1_stoich_x10 = 147;
        c.map_src = SIG_MAP;   c.clt_src = SIG_CLT;
        c.ve_table_x_src = SIG_RPM; c.ve_table_y_src = SIG_FUEL_LOAD; c.ve_table_z_en = 0;
        c.target_lambda_table_x_src = SIG_RPM; c.target_lambda_table_y_src = SIG_FUEL_LOAD;
        c.clt_corr_table_x_src = SIG_CLT; c.clt_corr_table_y_src = SIG_MAP; c.clt_corr_table_y_en = 0;
        c.post_start_table_x_src = SIG_CLT; c.post_start_table_y_src = SIG_RUN_TIME;
        c.iat_corr_table_x_src = SIG_IAT; c.iat_corr_table_y_src = SIG_MAP;
        c.ve_table_x_axis_n=16; c.ve_table_y_axis_n=16; c.target_lambda_table_x_axis_n=16; c.target_lambda_table_y_axis_n=16; c.predicted_map_table_x_axis_n=16;  // base tables own their axes now
    c.clt_axis_n=14; c.clt_map_axis_n=8; c.ps_clt_axis_n=8; c.run_time_axis_n=8; c.iat_axis_n=14; c.iat_map_axis_n=6; c.rl_headroom_axis_n=6; c.rl_map_axis_n=6; c.cyl_load_axis_n=8; c.cyl_rpm_axis_n=8;  // resizable correction-axis live bins
        for (int i = 0; i < 16; i++) c.ve_table_x_axis[i]=c.target_lambda_table_x_axis[i]=c.predicted_map_table_x_axis[i] = static_cast<float>(500 + i * 500);
        for (int i = 0; i < 16; i++) c.ve_table_y_axis[i]=c.target_lambda_table_y_axis[i] = static_cast<float>(10 + i * 10);  // real kPa
        for (int i = 0; i < 16; i++) { c.clt_corr_table[i] = 0; c.clt_axis[i] = static_cast<float>(-40 + i * 10); }
        for (unsigned i = 0; i < FUEL_CALCULATOR_TARGET_LAMBDA_TABLE_ALLOC; i++)
            c.target_lambda_table[i] = 1000;  // 1.000 lambda, over the whole allocation
        for (int li = 0; li < 16; li++)
            for (int ri = 0; ri < 16; ri++)
                c.ve_table[li * FUEL_CALCULATOR_VE_TABLE_X_AXIS_ALLOC + ri] =
                    static_cast<uint16_t>((ri * 10 + li) * 10);   // VE%x10, at the physical stride

        FuelCalculator fc;
        fc.init(c);
        EngineFrame frame{};
        // rpm = rpm_axis[3] = 2000, fuel_load = map = load_axis[5] = 60 kPa (SD: load = MAP, real kPa).
        // Expect exact cell (rpm_idx=3, load_idx=5) = 3*10+5 = 35.
        { SignalBus _b = make_bus(60.0f, 80.0f); fc.update(make_pos(2000.0f), _b, frame); }
        fprintf(stdout, "    ve@(rpm_idx3,load_idx5) = %.1f (expect 35)\n", (double)frame.ve_pct);
        CHECK_NEAR(frame.ve_pct, 35.0, 0.5);
    }
    SECTION("per-cylinder fuel trim: each cylinder uses its OWN table");
    {
        auto cfg = make_cfg();
        // cylinder 4's table (-> frame.cyl[3]) uniform +50%; all others stay 0 (neutral).
        for (unsigned i = 0; i < FUEL_CALCULATOR_CYL4_FUEL_CORR_TABLE_ALLOC; i++) cfg.cyl4_fuel_corr_table[i] = 500;   // +50.0%
        FuelCalculator fc; fc.init(cfg);
        EngineFrame frame{};
        { SignalBus _b = make_bus(100.0f); fc.update(make_pos(3000.0f), _b, frame); }
        const double pw0 = frame.cyl[0].inj_pw_us;   // cyl 1 -> table default 0 -> base
        const double pw3 = frame.cyl[3].inj_pw_us;   // cyl 4 -> +50%
        fprintf(stdout, "    cyl1 pw=%g  cyl4 pw=%g  ratio=%.3f (expect 1.5)\n", pw0, pw3, pw3 / pw0);
        CHECK_NEAR(pw3 / pw0, 1.5, 0.02);
        CHECK(frame.cyl[1].inj_pw_us == frame.cyl[0].inj_pw_us);   // only cyl 4 moved
    }
    SECTION("per-cylinder trim scales the FUEL, not the injector dead time");
    {
        // +10 % on cylinder 4 with 1000 µs of dead time: the dead time is the injector opening, the same
        // for every cylinder. It used to be multiplied too, so the trim delivered more than it said.
        auto cfg = make_cfg();
        for (unsigned i = 0; i < FUEL_CALCULATOR_STAGE1_DEAD_TIME_TABLE_ALLOC; i++) cfg.stage1_dead_time_table[i] = 1000;
        for (unsigned i = 0; i < FUEL_CALCULATOR_CYL4_FUEL_CORR_TABLE_ALLOC; i++) cfg.cyl4_fuel_corr_table[i] = 100;   // +10.0%
        FuelCalculator fc; fc.init(cfg);
        EngineFrame frame{};
        double dead = 0.0;
        { SignalBus b = make_bus(100.0f); fc.update(make_pos(3000.0f), b, frame);
          dead = b.get(SIG_PW_ADD_DEADTIME, 0.0f); }
        const double fuel0 = frame.cyl[0].inj_pw_us - dead;   // untrimmed cylinder's fuel part
        const double fuel3 = frame.cyl[3].inj_pw_us - dead;   // +10 % cylinder's fuel part
        fprintf(stdout, "    dead=%g fuel cyl1=%g cyl4=%g ratio=%.4f (expect 1.10)\n", dead, fuel0, fuel3, fuel3 / fuel0);
        CHECK_NEAR(dead, 1000.0, 1.0);
        CHECK_NEAR(fuel3 / fuel0, 1.10, 0.005);                // exactly the trim, on the fuel alone
    }
    SECTION("flood clear: cranking with the throttle open cuts fuel and holds the prime");
    {
        // The operator's way of saying "stop giving it fuel and turn it over" — nothing else asks for
        // wide-open throttle at cranking speed. It cuts through the shared wk::fuel_cut, so the check
        // is on the cut, not on the pulse width: the commanded pw deliberately still reports what the
        // tables asked for while a cut is in force (see EngineTask).
        auto cfg = make_cfg();
        cfg.flood_clear_enabled = 1;
        cfg.flood_clear_tps_pct = 90;
        cfg.prime_enable = 1; cfg.prime_mode = 0;
        for (unsigned i = 0; i < FUEL_CALCULATOR_PRIME_FUEL_TABLE_ALLOC; i++) cfg.prime_fuel_table[i] = 5;
        EngineFrame f{};

        auto run = [&](FuelCalculator& fc, EngineRunState st, float tps, bool sync) {
            SignalBus b = make_bus(100.0f, 80.0f);
            b.set(SIG_ENGINE_STATE, static_cast<float>(st));
            b.set(SIG_TPS, tps);
            b.invalidate(SIG_FUEL_CUT);
            EnginePosition p = make_pos(250.0f);
            p.is_synchronized = sync;
            fc.update(p, b, f);
            return std::pair<bool, bool>{ b.valid(SIG_FUEL_CUT), b.get(SIG_FLOOD_CLEAR, -1.0f) > 0.5f };
        };

        { FuelCalculator fc; fc.init(cfg);
          auto [cut, flag] = run(fc, EngineRunState::CRANKING, 95.0f, true);
          fprintf(stdout, "    cranking, tps 95%%: fuel_cut=%d flood_clear=%d (expect 1 1)\n", cut, flag);
          CHECK(cut); CHECK(flag);
          CHECK(f.prime_pw_us == 0);            // and the prime does not squirt into an engine being cleared
        }
        { FuelCalculator fc; fc.init(cfg);      // …the prime is only HELD, not spent: throttle closed, it fires
          run(fc, EngineRunState::CRANKING, 95.0f, true);
          run(fc, EngineRunState::CRANKING, 5.0f, true);
          fprintf(stdout, "    then throttle closed: prime_pw=%u (expect 5000)\n", f.prime_pw_us);
          CHECK(f.prime_pw_us == 5000u);
        }
        { FuelCalculator fc; fc.init(cfg);      // below the threshold: nothing happens
          auto [cut, flag] = run(fc, EngineRunState::CRANKING, 50.0f, true);
          CHECK(!cut); CHECK(!flag);
        }
        { FuelCalculator fc; fc.init(cfg);      // a RUNNING engine is never asking for this
          auto [cut, flag] = run(fc, EngineRunState::RUNNING, 95.0f, true);
          CHECK(!cut); CHECK(!flag);
        }
        { FuelCalculator fc; auto off = cfg; off.flood_clear_enabled = 0; fc.init(off);
          auto [cut, flag] = run(fc, EngineRunState::CRANKING, 95.0f, true);
          CHECK(!cut); CHECK(!flag);
        }
        {   // A DEAD THROTTLE NEVER CUTS. The reading is invalid, not low — an unassigned or failed TPS
            // must not be able to stop an engine from starting.
            FuelCalculator fc; fc.init(cfg);
            SignalBus b = make_bus(100.0f, 80.0f);
            b.set(SIG_ENGINE_STATE, static_cast<float>(EngineRunState::CRANKING));
            b.invalidate(SIG_TPS); b.invalidate(SIG_FUEL_CUT);
            fc.update(make_pos(250.0f), b, f);
            fprintf(stdout, "    invalid TPS: fuel_cut=%d (expect 0)\n", (int)b.valid(SIG_FUEL_CUT));
            CHECK(!b.valid(SIG_FUEL_CUT));
        }
    }

    SECTION("a correction's enable short-circuits it: no evaluation, exactly 1.000");
    {
        // "Off" used to mean a table tuned to 0%. The flag says it outright — and says it about the
        // SIGNAL too, so a page reading 1.000 means the correction is out of the calculation rather
        // than merely neutral today.
        auto cfg = make_cfg();
        for (unsigned i = 0; i < FUEL_CALCULATOR_MAP_CORR_TABLE_ALLOC; i++) cfg.map_corr_table[i] = 500;  // +50%
        EngineFrame f{};
        auto corr = [&](uint8_t on) {
            auto c = cfg; c.enable_map = on;
            FuelCalculator fc; fc.init(c);
            SignalBus b = make_bus(100.0f, 80.0f);
            fc.update(make_pos(2000.0f), b, f);
            return std::pair<float, float>{ b.get(SIG_FUEL_CORR_MAP, -1.0f), f.base_fuel_pw_us };
        };
        auto [on_mult, on_pw]   = corr(1);
        auto [off_mult, off_pw] = corr(0);
        fprintf(stdout, "    map corr on=%.3f off=%.3f  pw %.1f -> %.1f\n",
                (double)on_mult, (double)off_mult, (double)on_pw, (double)off_pw);
        CHECK_NEAR(on_mult,  1.5, 1e-4);
        CHECK_NEAR(off_mult, 1.0, 1e-4);
        CHECK_NEAR(off_pw, on_pw / 1.5, on_pw * 0.02);

        // The global multiplier answers to its own flag, and turning it off does not wipe the number.
        auto ov = cfg; ov.overall_corr_pct = 200; ov.enable_overall = 0;   // +20%, disabled
        FuelCalculator fc; fc.init(ov);
        SignalBus b = make_bus(100.0f, 80.0f);
        fc.update(make_pos(2000.0f), b, f);
        CHECK_NEAR(b.get(SIG_FUEL_CORR_OVERALL, -1.0f), 1.0, 1e-4);
    }

    SECTION("run time is ENGINE run time, not uptime");
    {
        // The anchor is set at the cranking->running edge. Before that edge it is 0, so publishing
        // now-anchor meant publishing SECONDS SINCE BOOT under a name that says otherwise: a stationary
        // engine showed a climbing Run Time, the post-start table decayed with nothing having started,
        // and idle's LTT gate (run_time >= ltt_min_runtime_s) came free with enough uptime.
        auto cfg = make_cfg();
        FuelCalculator fc; fc.init(cfg);
        EngineFrame frame{};
        g_test_ms = 300000;                                   // ECU has been powered for five minutes
        SignalBus b1 = make_bus(); fc.update(make_pos(0.0f), b1, frame);
        fprintf(stdout, "    never started, 300s uptime: run_time = %.1f (expect 0)\n",
                (double)b1.get(SIG_RUN_TIME, -1.0f));
        CHECK_NEAR(b1.get(SIG_RUN_TIME, -1.0f), 0.0, 0.001);

        fc.on_engine_start();                                 // caught, at t = 300 s
        g_test_ms = 307000;
        SignalBus b2 = make_bus(); fc.update(make_pos(1500.0f), b2, frame);
        fprintf(stdout, "    7s after catching:          run_time = %.1f (expect 7)\n",
                (double)b2.get(SIG_RUN_TIME, -1.0f));
        CHECK_NEAR(b2.get(SIG_RUN_TIME, -1.0f), 7.0, 0.001);

        fc.on_engine_stop();                                  // and a stopped engine has run for 0 s
        g_test_ms = 320000;
        SignalBus b3 = make_bus(); fc.update(make_pos(0.0f), b3, frame);
        fprintf(stdout, "    after stopping:             run_time = %.1f (expect 0)\n",
                (double)b3.get(SIG_RUN_TIME, -1.0f));
        CHECK_NEAR(b3.get(SIG_RUN_TIME, -1.0f), 0.0, 0.001);
        g_test_ms = 0;
    }

    SECTION("MAP prediction: a fast throttle stands the predicted table in for a late sensor");
    {
        // The point is the SENSOR being late, so measured MAP stays at 40 kPa — what an idling manifold
        // reads — through a throttle movement, with the predicted table at 100. Stepped at 5 ms, the
        // rate the module actually sees: a derivative sampled at 100 ms intervals is a different signal.
        auto cfg = make_cfg();
        cfg.map_predict_enabled = 1;
        cfg.map_predict_hold_ms = 200;
        cfg.map_predict_scale_table_x_src = SIG_RPM; cfg.map_predict_scale_table_y_en = 0;
        cfg.mps_rpm_axis_n = 8;
        for (int i = 0; i < 8; i++) { cfg.mps_rpm_axis[i] = static_cast<float>(500 + i * 500);
                                      cfg.map_predict_scale_table[i] = 100; }        // fully in at 100 %/s
        for (unsigned k = 0; k < FUEL_CALCULATOR_PREDICTED_MAP_TABLE_ALLOC; k++)
            cfg.predicted_map_table[k] = 1000;                                        // 100.0 kPa flat
        FuelCalculator fc; fc.init(cfg); EngineFrame frame{};

        uint32_t t = 1000;
        float    tps = 5.0f;
        SignalBus last{};
        auto step = [&](float new_tps, uint32_t ms) {
            for (uint32_t k = 0; k < ms; k += 5) {
                t += 5; tps = new_tps;
                SignalBus b = make_bus(40.0f, 80.0f);
                b.set(SIG_TPS, tps);
                g_test_ms = t;
                fc.update(make_pos(1000.0f), b, frame);
                last = b;
            }
            return last;
        };

        SignalBus quiet = step(5.0f, 200);                       // foot still
        CHECK(quiet.get(SIG_MAP_SOURCE, -1.0f) == 0.0f);         // measured
        CHECK_NEAR(quiet.get(SIG_MAP_EST, 0.0f), 40.0f, 0.5f);

        // One 5 ms sample carrying 45% of throttle = 9000 %/s, far past the threshold.
        SignalBus stab = step(50.0f, 5);
        fprintf(stdout, "    stab: rate=%.0f %%/s  map_est=%.1f  source=%.0f\n",
                (double)stab.get(SIG_TPS_RATE, 0.0f), (double)stab.get(SIG_MAP_EST, 0.0f),
                (double)stab.get(SIG_MAP_SOURCE, -1.0f));
        CHECK(stab.get(SIG_MAP_SOURCE, -1.0f) == 1.0f);          // predicted
        CHECK_NEAR(stab.get(SIG_MAP_EST, 0.0f), 100.0f, 0.5f);

        SignalBus held = step(50.0f, 100);                       // on the throttle, no longer moving
        CHECK(held.get(SIG_MAP_SOURCE, -1.0f) == 1.0f);          // still inside the 200 ms hold
        SignalBus after = step(50.0f, 200);                      // hold expired
        fprintf(stdout, "    after hold: rate=%.1f %%/s  map_est=%.1f  source=%.0f\n",
                (double)after.get(SIG_TPS_RATE, 0.0f), (double)after.get(SIG_MAP_EST, 0.0f),
                (double)after.get(SIG_MAP_SOURCE, -1.0f));
        CHECK(after.get(SIG_MAP_SOURCE, -1.0f) == 0.0f);         // the sensor again
        CHECK_NEAR(after.get(SIG_MAP_EST, 0.0f), 40.0f, 0.5f);
    }

    SECTION("MAP prediction never predicts DOWNWARD, and is inert when disabled");
    {
        // Predicted (100) below measured (150, on boost): taking the lower would invent a lean hole,
        // which is the opposite of the job.
        auto cfg = make_cfg();
        cfg.map_predict_enabled = 1; cfg.map_predict_hold_ms = 200;
        cfg.map_predict_scale_table_x_src = SIG_RPM; cfg.map_predict_scale_table_y_en = 0;
        cfg.mps_rpm_axis_n = 8;
        for (int i = 0; i < 8; i++) { cfg.mps_rpm_axis[i] = static_cast<float>(500 + i * 500);
                                      cfg.map_predict_scale_table[i] = 100; }
        for (unsigned k = 0; k < FUEL_CALCULATOR_PREDICTED_MAP_TABLE_ALLOC; k++)
            cfg.predicted_map_table[k] = 1000;                    // 100.0 kPa flat
        FuelCalculator fc; fc.init(cfg); EngineFrame frame{};
        uint32_t t = 5000;
        auto drive = [&](FuelCalculator& f, EngineFrame& fr, float map, float tps, uint32_t ms) {
            SignalBus b{};
            for (uint32_t k = 0; k < ms; k += 5) {
                t += 5; g_test_ms = t;
                b = make_bus(map, 80.0f); b.set(SIG_TPS, tps);
                f.update(make_pos(3000.0f), b, fr);
            }
            return b;
        };
        drive(fc, frame, 150.0f, 5.0f, 100);
        SignalBus boosted = drive(fc, frame, 150.0f, 50.0f, 5);
        CHECK_NEAR(boosted.get(SIG_MAP_EST, 0.0f), 150.0f, 0.5f);  // measured wins

        auto off = make_cfg();                                     // the shipped default: prediction off
        FuelCalculator fo; fo.init(off); EngineFrame f2{};
        drive(fo, f2, 40.0f, 5.0f, 100);
        SignalBus disabled = drive(fo, f2, 40.0f, 50.0f, 5);
        CHECK(disabled.get(SIG_MAP_SOURCE, -1.0f) == 0.0f);
        CHECK_NEAR(disabled.get(SIG_MAP_EST, 0.0f), 40.0f, 0.5f);
        g_test_ms = 0;
    }

    // (outputs are published to the bus + control frame; on_engine_stop no longer zeroes
    //  telemetry fields — they decay/refresh via the normal publish path)

    SECTION("sequential at CRANK-only sync gets half a pulse per squirt (it squirts every revolution)");
    {
        // Audit T3: without the cam every event fires once per revolution, so a sequential injector
        // squirts twice a cycle. The pulse was not divided for it: double fuel until cam sync.
        auto cfg = make_cfg();
        const uint8_t saved = g_config.engine.inj_stage[0].mode;
        g_config.engine.inj_stage[0].mode = static_cast<uint8_t>(InjectionMode::SEQUENTIAL);
        FuelCalculator fc; fc.init(cfg);
        EngineFrame frame{};
        EnginePosition ph = make_pos(3000.0f), cr = make_pos(3000.0f);
        cr.sync_level = SyncLevel::CRANK;
        SignalBus b1 = make_bus(100.0f); fc.update(ph, b1, frame);
        SignalBus b2 = make_bus(100.0f); fc.update(cr, b2, frame);
        const float dead = b1.get(wk::pw_add_deadtime, 0.0f) / 1000.0f;
        const float pw_phase = b1.get(SIG_INJ_PW, -1.0f) - dead;
        const float pw_crank = b2.get(SIG_INJ_PW, -1.0f) - dead;
        fprintf(stdout, "    fuel pw: phase %.3f ms, crank %.3f ms\n", (double)pw_phase, (double)pw_crank);
        CHECK(pw_phase > 0.0f);
        CHECK_NEAR(pw_crank, pw_phase / 2.0f, 0.05f);
        CHECK(injection_deliveries_per_cycle(static_cast<uint8_t>(InjectionMode::SEQUENTIAL_ANY_SYNC),
                                             g_config.engine.cycle_type, 1, false) == 1);
        CHECK(injection_deliveries_per_cycle(static_cast<uint8_t>(InjectionMode::SEMI_SEQUENTIAL),
                                             g_config.engine.cycle_type, 1, false) == 2);
        g_config.engine.inj_stage[0].mode = saved;
    }

    SECTION("injector duty — the squirt WINDOW, not the crank revolution");
    {
        // Duty is the commanded opening over the time one squirt has. Checking it against the PW the
        // same frame published is what makes this test able to fail: get the window wrong — divide by
        // the crank revolution instead of the engine cycle, or forget that the mode splits the charge
        // across several squirts — and the two channels stop agreeing, even though each looks sane on
        // its own. Both come off the bus, so nothing here re-implements the calculation under test.
        auto cfg = make_cfg();
        FuelCalculator fc;
        fc.init(cfg);
        EngineFrame frame{};
        const uint8_t events = injection_events_per_cycle(g_config.engine.inj_stage[0].mode,
                                                          g_config.engine.cycle_type,
                                                          g_config.engine.inj_stage[0].injections_per_cycle);
        CHECK(events > 0);

        for (const float rpm : { 1500.0f, 3000.0f, 6000.0f }) {
            SignalBus bus = make_bus(100.0f);
            fc.update(make_pos(rpm), bus, frame);
            const float pw_ms = bus.get(SIG_INJ_PW, -1.0f);
            const float duty  = bus.get(SIG_INJ_DUTY, -1.0f);
            CHECK(pw_ms > 0.0f);
            CHECK(duty  > 0.0f);
            // One squirt's window, in ms: the 4-stroke cycle (120000/rpm ms) shared between events.
            const float window_ms = 120000.0f / (rpm * (float)events);
            CHECK_NEAR(duty, pw_ms * 100.0f / window_ms, 0.5f);
            fprintf(stdout, "    %5.0f rpm: pw %.3f ms, window %.3f ms -> duty %.1f%%\n",
                    (double)rpm, (double)pw_ms, (double)window_ms, (double)duty);
        }

        // A stage that is not carrying fuel publishes NOTHING, so its channel expires rather than
        // reading a confident 0% — "there is no stage 3" and "stage 3 is shut" are different answers.
        SignalBus bus = make_bus(100.0f);
        fc.update(make_pos(3000.0f), bus, frame);
        CHECK(!bus.valid(SIG_INJ_DUTY_3));
        CHECK(!bus.valid(SIG_INJ_DUTY_4));
    }

    return test_summary();
}
