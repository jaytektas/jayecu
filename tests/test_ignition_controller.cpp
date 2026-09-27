#include "../firmware/Engine/EngineStateMachine.h"   // EngineRunState — the cranking base is state-gated
#include "test_helpers.h"
#include "../firmware/Engine/Modules/Ignition.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "../generated/signal_ids.h"
#include "well_known_signals.h"
#include "../generated/ecu_config.h"   // g_config.engine.cylinder_count

static IgnitionConfig make_cfg(int8_t fill_adv = 25) {
    IgnitionConfig c = g_config.ignition;   // SHIPPED defaults, not zero: a zeroed axis has no live bin count
    // The corrections each have a switch now, and a zero-initialised struct has them all OFF — a real
    // tune ships them on (the tables are neutral until given values), so a test that wants a correction
    // evaluated has to say so, exactly as the fuel tests do.
    c.enable_clt = c.enable_iat = c.enable_fuelcomp = c.enable_gear = c.enable_poststart = 1;
    c.enable_generic1 = c.enable_generic2 = c.enable_generic3 = c.enable_generic4 = 1;
    c.enable_revlimit = 1;
    // Dwell is a table now. Axes off = one cell = the fixed 3 ms this used to set as a scalar.
    c.dwell_table[0] = 3000; c.dwell_table_x_en = 0; c.dwell_table_y_en = 0;
    c.max_adv_deg = 500;
    c.min_adv_deg = -100;
    // Table-engine axis channels: ign X=RPM / Y=MAP (raw); clt-advance X=CLT, Y off (1D curve).
    c.ign_table_x_src = SIG_RPM; c.ign_table_y_src = SIG_MAP;
    c.clt_ign_corr_table_x_src = SIG_CLT; c.clt_ign_corr_table_y_src = SIG_MAP; c.clt_ign_corr_table_y_en = 0;

    c.ign_rpm_axis_n = 16; c.ign_load_axis_n = 16;   // live bins = dense stride (clt_axis is now fixed 16)
    c.clt_axis_n=14; c.clt_adv_map_axis_n=8;  // resizable correction-axis live bins

    for (int r = 0; r < 16; r++)          // 16x16 LIVE, laid out at the PHYSICAL stride; cell = deg x10
        for (int cc = 0; cc < 16; cc++)
            c.ign_table[r * IGNITION_IGN_RPM_AXIS_ALLOC + cc] = static_cast<int16_t>(fill_adv * 10);

    for (int i = 0; i < 16; i++) {
        c.ign_rpm_axis[i]  = static_cast<float>(500 + i * 500);
        c.ign_load_axis[i] = static_cast<float>(10 + i * 6);    // real kPa breakpoints
    }

    for (int i = 0; i < 16; i++) {
        c.clt_ign_corr_table[i] = 0;
        c.clt_axis[i]          = static_cast<float>(-40 + i * 10);
    }

    // Cranking advance: a 16-bin CLT curve, off by default (as it ships).
    c.cranking_ign_table_x_src = SIG_CLT; c.cranking_ign_table_y_src = SIG_BATTERY;
    c.cranking_ign_table_y_en  = 0;
    for (int i = 0; i < 16; i++) {
        c.cranking_ign_table[i] = 40;                              // 4.0 deg everywhere
        c.ci_x_axis[i] = static_cast<float>(-40 + i * 10);
    }

    return c;
}

static EnginePosition make_pos(float rpm = 3000.0f) {
    EnginePosition p{};
    p.rpm = rpm;
    return p;
}

static SignalBus make_bus(float map_kpa = 100.0f, float clt_c = 80.0f) {
    SignalBus bus{};
    bus.set(SIG_MAP, map_kpa);
    bus.set(SIG_CLT,   clt_c);
    return bus;
}

int main() {
    fprintf(stdout, "=== Ignition ===\n");

    SECTION("dwell follows battery voltage");
    {
        // THE COIL CHARGES TO A CURRENT, and the current follows the voltage across it — so one fixed
        // dwell is right at exactly one battery voltage. The table's default curve is the shape a coil
        // datasheet gives: roughly double the time at half the volts.
        auto c = make_cfg();
        c.dwell_table_x_en = 1;                       // voltage axis on (the shipped default)
        c.dwell_table_x_src = SIG_BATTERY;
        c.dwell_volt_axis_n = 6;
        const float volts[6] = {6, 8, 10, 12, 14, 16};
        const uint16_t ms_x1000[6] = {5000, 4500, 4000, 3500, 3000, 2500};
        for (int i = 0; i < 6; i++) { c.dwell_volt_axis[i] = volts[i]; c.dwell_table[i] = ms_x1000[i]; }

        Ignition ig; ig.init(c);
        EngineFrame frame{};
        auto dwell_at = [&](float v) {
            SignalBus b = make_bus();
            b.set(SIG_BATTERY, v);
            ig.update(make_pos(3000.0f), b, frame);
            return frame.dwell_us;
        };
        const uint32_t crank = dwell_at(8.0f), running = dwell_at(14.0f), charging = dwell_at(16.0f);
        fprintf(stdout, "    dwell 8V=%u us  14V=%u us  16V=%u us\n",
                (unsigned)crank, (unsigned)running, (unsigned)charging);
        CHECK_NEAR(crank,    4500.0, 2.0);            // a cranking battery gets a LONGER charge
        CHECK_NEAR(running,  3000.0, 2.0);
        CHECK_NEAR(charging, 2500.0, 2.0);
        CHECK(crank > running && running > charging); // …monotonic, which is the whole point
        // Between breakpoints it interpolates rather than stepping.
        CHECK_NEAR(dwell_at(13.0f), 3250.0, 60.0);

        // Axes off = one cell = the fixed dwell this used to be, unchanged.
        auto flat = make_cfg();                       // make_cfg leaves both axes off, cell 0 = 3000
        Ignition ig2; ig2.init(flat);
        EngineFrame f2{};
        { SignalBus b = make_bus(); b.set(SIG_BATTERY, 8.0f); ig2.update(make_pos(3000.0f), b, f2); }
        CHECK_NEAR(f2.dwell_us, 3000.0, 2.0);         // …and voltage cannot move it
    }
    SECTION("basic advance from flat table");
    {
        auto cfg = make_cfg(25);
        Ignition ic;
        ic.init(cfg);
        EngineFrame frame{};
        { SignalBus _b = make_bus(); ic.update(make_pos(), _b, frame); }
        CHECK_NEAR(frame.ign_advance_deg, 25.0, 0.5);
    }

    SECTION("advance clamped to max_adv");
    {
        auto cfg = make_cfg(60);
        cfg.max_adv_deg = 400;
        Ignition ic;
        ic.init(cfg);
        EngineFrame frame{};
        { SignalBus _b = make_bus(); ic.update(make_pos(), _b, frame); }
        CHECK(frame.ign_advance_deg <= 40.0f);
        CHECK_NEAR(frame.ign_advance_deg, 40.0, 0.5);
    }

    SECTION("per-cylinder trim cannot take a cylinder past Max/Min Advance");
    {
        // Max Advance is "every table and correction summed" — the per-cylinder trim included. It used
        // to be added after the clamp, so a +10° cell on a 38° base put cylinder 1 at 48° against a 40°
        // limit (and the table allows ±60°).
        auto cfg = make_cfg(38);
        cfg.max_adv_deg = 400;
        for (auto& v : cfg.cyl1_ign_corr_table) v = 100;     // +10.0° everywhere
        for (auto& v : cfg.cyl2_ign_corr_table) v = -600;    // -60.0° everywhere
        cfg.min_adv_deg = -100;
        Ignition ic; ic.init(cfg);
        EngineFrame frame{};
        { SignalBus _b = make_bus(); ic.update(make_pos(), _b, frame); }
        fprintf(stdout, "    cyl1=%.1f cyl2=%.1f\n", frame.cyl[0].spark_btdc_x10 / 10.0, frame.cyl[1].spark_btdc_x10 / 10.0);
        CHECK(frame.cyl[0].spark_btdc_x10 <= 400);           // held at Max Advance
        CHECK(frame.cyl[1].spark_btdc_x10 >= -100);          // held at Min Advance
        CHECK_NEAR(frame.cyl[2].spark_btdc_x10, 380.0, 1.0); // an untrimmed cylinder is untouched
    }

    SECTION("launch map is absolute — but the CLT/IAT protection RETARDS still apply");
    {
        auto cfg = make_cfg(25);
        Ignition ic; ic.init(cfg);
        EngineFrame frame{};
        auto adv_with = [&](float iat_corr, float clt_corr) {
            SignalBus b = make_bus();
            b.set_bool(wk::launch_active, true); b.set(wk::launch_ign_adv, 10.0f);
            b.set(SIG_IGN_CORR_IAT, iat_corr); b.set(SIG_IGN_CORR_CLT, clt_corr);
            b.set(wk::ign_advance_trim, iat_corr + clt_corr);
            ic.update(make_pos(), b, frame);
            return frame.ign_advance_deg;
        };
        CHECK_NEAR(adv_with(-3.0f, 0.0f), 7.0, 0.1);     // heat-soaked: retarded off the launch angle
        CHECK_NEAR(adv_with(+2.0f, +1.0f), 10.0, 0.1);   // an advancing trim does not touch the launch map
        // …and the torque model hears how far below the main map (25°) the launch angle really is.
        SignalBus b = make_bus();
        b.set_bool(wk::launch_active, true); b.set(wk::launch_ign_adv, 10.0f);
        ic.update(make_pos(), b, frame);
        CHECK_NEAR(b.get(SIG_SPARK_BELOW_MAP, -1.0f), 15.0, 0.1);
    }

    SECTION("per-cylinder knock retard: only the knocking cylinder's spark moves");
    {
        auto cfg = make_cfg(25);
        Ignition ic; ic.init(cfg);
        float per[MAX_CYLINDERS] = {};
        per[2] = 4.0f;                                    // cylinder 3 has earned 4° of retard
        ic.apply_knock_retard(per, MAX_CYLINDERS);
        EngineFrame frame{};
        { SignalBus b = make_bus(); ic.update(make_pos(), b, frame); }
        CHECK_NEAR(frame.cyl[2].spark_btdc_x10, 210.0, 1.0);   // 25 - 4
        CHECK_NEAR(frame.cyl[0].spark_btdc_x10, 250.0, 1.0);   // untouched
        CHECK_NEAR(frame.ign_advance_deg, 21.0, 0.1);           // the global figure is the worst cylinder
    }

    SECTION("advance clamped to min_adv");
    {
        auto cfg = make_cfg(-15);
        Ignition ic;
        ic.init(cfg);
        EngineFrame frame{};
        { SignalBus _b = make_bus(); ic.update(make_pos(), _b, frame); }
        CHECK(frame.ign_advance_deg >= -10.0f);
    }

    SECTION("ign_cut leaves the commanded advance alone");
    {
        // A cut is an OUTPUT decision — EnginePositionHal::set_output_cuts clears the coil's
        // execution bit so the dwell never starts. Ignition does not see it at all, and the advance
        // keeps reporting what the tables asked for.
        //
        // This asserted 0 for as long as it existed, which made "no spark" and "a spark at TDC"
        // look the same in a test. They are not the same on an engine: nothing ever suppressed the
        // coil, so every ign-cut requester — rev limiter, launch, flat shift, pit limiter,
        // overboost, EngineProtection — dumped a spark at TDC on every plug instead of cutting.
        auto cfg = make_cfg(25);
        Ignition ic;
        ic.init(cfg);
        EngineFrame cut{}, run{};
        { SignalBus _b = make_bus(); _b.set_bool(SIG_IGN_CUT, true);
          ic.update(make_pos(), _b, cut); }
        { SignalBus _b = make_bus(); ic.update(make_pos(), _b, run); }
        CHECK_NEAR(cut.ign_advance_deg, run.ign_advance_deg, 0.001);
        CHECK_NEAR(cut.ign_advance_deg, 25.0, 0.5);
    }

    // -------------------------------------------------------------------------------------------
    // Fixed timing — the base-timing procedure. Hold one advance so a timing light reads the number
    // the tuner chose, then wind trigger_offset_btdc until the light agrees. It is worth pinning
    // because a correction that keeps moving underneath is exactly what makes a light unreadable,
    // and because a diagnostic mode must never outrank a safety cut.
    // -------------------------------------------------------------------------------------------
    SECTION("fixed timing holds its value against every trim and retard");
    {
        auto cfg = make_cfg(25);
        cfg.fixed_timing_enable = 1;
        cfg.fixed_timing_deg    = 100;              // 10.0 deg
        cfg.overall_adv_trim    = 150;              // 15 deg of trim that must NOT show up
        Ignition ic;
        ic.init(cfg);
        ic.apply_knock_retard(8.0f);                // nor 8 deg of knock retard
        EngineFrame frame{};
        { SignalBus _b = make_bus();
          _b.set(SIG_IGN_ADVANCE_TRIM, 12.0f);        // nor the slow trims
          _b.set(SIG_ANTILAG_RETARD,    6.0f);        // nor a retard off the bus
          ic.update(make_pos(), _b, frame); }
        CHECK_NEAR(frame.ign_advance_deg, 10.0, 0.01);
        // Every cylinder reads the same number, or the light shows one of them instead of the value.
        for (int i = 0; i < g_config.engine.cylinder_count; i++)
            CHECK(frame.cyl[i].spark_btdc_x10 == 100);
    }

    SECTION("an ignition cut does NOT touch the timing — it stops the coil");
    {
        // A cut suppresses the DWELL at the output (EnginePositionHal::set_output_cuts); it must not
        // move the advance. This used to assert 0, which is not "no spark" — it is a spark at TDC,
        // on every plug, every time the rev limiter came in.
        auto cfg = make_cfg(25);
        cfg.fixed_timing_enable = 1;
        cfg.fixed_timing_deg    = 100;
        Ignition ic;
        ic.init(cfg);
        EngineFrame frame{};
        { SignalBus _b = make_bus(); _b.set_bool(SIG_IGN_CUT, true);
          ic.update(make_pos(), _b, frame); }
        CHECK_NEAR(frame.ign_advance_deg, 10.0, 0.01);
    }

    SECTION("disabled, fixed timing changes nothing");
    {
        auto cfg = make_cfg(25);
        cfg.fixed_timing_deg = 100;                 // set, but the toggle is off
        Ignition ic;
        ic.init(cfg);
        EngineFrame frame{}, ref{};
        { SignalBus _b = make_bus(); ic.update(make_pos(), _b, ref); }
        cfg.fixed_timing_enable = 0;
        Ignition ic2; ic2.init(cfg);
        { SignalBus _b = make_bus(); ic2.update(make_pos(), _b, frame); }
        CHECK_NEAR(frame.ign_advance_deg, ref.ign_advance_deg, 0.001);
    }

    SECTION("dwell propagates to telemetry and frame");
    {
        auto cfg = make_cfg(25);
        Ignition ic;
        ic.init(cfg);
        EngineFrame frame{};
        { SignalBus _b = make_bus(); ic.update(make_pos(), _b, frame); }
        CHECK(frame.dwell_us == 3000u);
    }

    SECTION("per-cylinder spark angle set");
    {
        auto cfg = make_cfg(25);
        Ignition ic;
        ic.init(cfg);
        EngineFrame frame{};
        { SignalBus _b = make_bus(); ic.update(make_pos(), _b, frame); }
        // Only the cylinders the engine has are written (g_config default = 4).
        for (int i = 0; i < g_config.engine.cylinder_count; i++)
            CHECK(frame.cyl[i].spark_btdc_x10 == static_cast<int16_t>(frame.ign_advance_deg * 10.0f));
    }

    SECTION("reads ign_advance_trim off the bus (slow corrections produced by IgnitionTrim)");
    {
        // CLT/IAT/etc are no longer computed here — they arrive pre-summed as wk::ign_advance_trim.
        // Ignition just adds that one number additively.
        auto cfg = make_cfg(20);
        Ignition ic;
        ic.init(cfg);
        EngineFrame base{}; { SignalBus _b = make_bus(); ic.update(make_pos(3000.0f), _b, base); }
        EngineFrame trimmed{};
        { SignalBus _b = make_bus(); _b.set(SIG_IGN_ADVANCE_TRIM, -5.0f); ic.update(make_pos(3000.0f), _b, trimmed); }
        CHECK_NEAR(trimmed.ign_advance_deg, base.ign_advance_deg - 5.0f, 0.05f);
    }

    SECTION("cranking advance REPLACES the map and its corrections, and says which base it used");
    {
        // The map has nothing useful to say at cranking speed, and the coolant advance correction is
        // indexed on the same coolant the cranking table is — so the table replaces both, or the
        // coolant term gets counted twice.
        auto cfg = make_cfg(25);          // map: 25 deg everywhere
        cfg.cranking_ign_enable = 1;
        cfg.overall_adv_trim    = 15;     // +1.5, which must NOT reach a cranking engine
        Ignition ic;
        ic.init(cfg);
        EngineFrame f{};

        SignalBus b = make_bus();
        b.set(SIG_IGN_ADVANCE_TRIM, -5.0f);                         // slow corrections say -5
        b.set(SIG_ENGINE_STATE, static_cast<float>(static_cast<int>(EngineRunState::CRANKING)));
        ic.update(make_pos(200.0f), b, f);
        CHECK_NEAR(f.ign_advance_deg, 4.0f, 0.05f);                 // the cell, and only the cell
        CHECK_NEAR(b.get(SIG_IGN_BASE_ADV), 4.0f, 0.05f);
        CHECK(b.get(SIG_IGN_BASE_KIND) == 1.0f);                    // "Cranking Table"

        // Caught: the map is the base again, and every correction is back with it.
        b.set(SIG_ENGINE_STATE, static_cast<float>(static_cast<int>(EngineRunState::RUNNING)));
        ic.update(make_pos(3000.0f), b, f);
        CHECK_NEAR(f.ign_advance_deg, 25.0f - 5.0f + 1.5f, 0.05f);
        CHECK(b.get(SIG_IGN_BASE_KIND) == 0.0f);                    // "Advance Map"
    }

    SECTION("disabled, cranking fires on the map exactly as before");
    {
        auto cfg = make_cfg(25);          // cranking_ign_enable stays 0 — the shipped default
        Ignition ic;
        ic.init(cfg);
        EngineFrame f{};
        SignalBus b = make_bus();
        b.set(SIG_ENGINE_STATE, static_cast<float>(static_cast<int>(EngineRunState::CRANKING)));
        ic.update(make_pos(200.0f), b, f);
        CHECK_NEAR(f.ign_advance_deg, 25.0f, 0.05f);
        CHECK(b.get(SIG_IGN_BASE_KIND) == 0.0f);
    }

    SECTION("the chain adds up: base + corrections - retards == the commanded advance");
    {
        // What a diagnostics page reads down the screen has to BE the arithmetic, or it is decoration.
        // Base and rev-limit are published here; the slow nine arrive pre-summed as ign_advance_trim;
        // every retard has its own channel and spark_retard_total is their sum.
        auto cfg = make_cfg(20);
        cfg.overall_adv_trim = 15;                       // +1.5 deg, a scalar with no channel of its own
        Ignition ic;
        ic.init(cfg);
        EngineFrame f{};
        SignalBus b = make_bus();
        b.set(SIG_IGN_ADVANCE_TRIM, -5.0f);
        b.set(SIG_IGN_CORR_TRANSIENT, 1.0f);
        b.set(SIG_ANTILAG_RETARD, 6.0f);
        ic.update(make_pos(3000.0f), b, f);

        const float chain = b.get(SIG_IGN_BASE_ADV) + b.get(SIG_IGN_CORR_REVLIMIT)
                          + b.get(SIG_IGN_ADVANCE_TRIM) + b.get(SIG_IGN_CORR_TRANSIENT)
                          + b.get(SIG_IDLE_IGN_CORR) + cfg.overall_adv_trim * 0.1f
                          - b.get(SIG_SPARK_RETARD_TOTAL);
        fprintf(stdout, "    base=%g rev=%g trim=%g trans=%g retard=%g -> chain=%g adv=%g\n",
                (double)b.get(SIG_IGN_BASE_ADV), (double)b.get(SIG_IGN_CORR_REVLIMIT),
                (double)b.get(SIG_IGN_ADVANCE_TRIM), (double)b.get(SIG_IGN_CORR_TRANSIENT),
                (double)b.get(SIG_SPARK_RETARD_TOTAL), (double)chain, (double)f.ign_advance_deg);
        CHECK_NEAR(chain, f.ign_advance_deg, 0.05f);
        CHECK_NEAR(b.get(SIG_SPARK_RETARD_TOTAL), 6.0f, 0.05f);
    }

    SECTION("subtracts a retard published on the bus (anti-lag / nitrous / traction)");
    {
        auto cfg = make_cfg(20);
        Ignition ic;
        ic.init(cfg);
        EngineFrame base{}; { SignalBus _b = make_bus(); ic.update(make_pos(3000.0f), _b, base); }
        EngineFrame retarded{};
        { SignalBus _b = make_bus(); _b.set(SIG_ANTILAG_RETARD, 6.0f); ic.update(make_pos(3000.0f), _b, retarded); }
        CHECK_NEAR(retarded.ign_advance_deg, base.ign_advance_deg - 6.0f, 0.05f);
    }

    SECTION("while LAUNCH is active the launch map IS the advance, not a correction on the map");
    {
        // Launch publishes an absolute angle, so Ignition swaps the base table out for it — the
        // cranking arrangement, and for the same reason: two absolute answers cannot be added. The
        // additive trims go with the map they belonged to; the retards do not.
        auto cfg = make_cfg(20);
        Ignition ic;
        ic.init(cfg);
        EngineFrame base{}; { SignalBus _b = make_bus(); ic.update(make_pos(3000.0f), _b, base); }
        EngineFrame launched{};
        { SignalBus _b = make_bus();
          _b.set(SIG_IGN_ADVANCE_TRIM, 5.0f);                 // must NOT reach the launch angle
          _b.set_bool(SIG_LAUNCH_ACTIVE, true);
          _b.set(SIG_LAUNCH_IGN_ADV, -8.0f);                  // past TDC, which no retard could produce
          ic.update(make_pos(3000.0f), _b, launched);
          CHECK_NEAR(_b.get(SIG_IGN_BASE_ADV), -8.0f, 0.05f);
          CHECK_NEAR(_b.get(SIG_IGN_BASE_KIND), 2.0f, 0.01f); }
        CHECK_NEAR(launched.ign_advance_deg, -8.0f, 0.05f);
        CHECK(launched.ign_advance_deg != base.ign_advance_deg);
        // …and a retard still subtracts from it, because protection does not stop mattering.
        EngineFrame pulled{};
        { SignalBus _b = make_bus();
          _b.set_bool(SIG_LAUNCH_ACTIVE, true);
          _b.set(SIG_LAUNCH_IGN_ADV, 10.0f);
          _b.set(SIG_TRACTION_RETARD, 4.0f);
          ic.update(make_pos(3000.0f), _b, pulled); }
        CHECK_NEAR(pulled.ign_advance_deg, 6.0f, 0.05f);
    }

    SECTION("table — x=rpm(cols) maps right, no transpose");
    {
        // 16 rpm cols × 16 load rows; cell encodes (rpm_idx, load_idx) within the
        // advance clamp range. A transposed read returns a different number.
        IgnitionConfig c{};
        c.dwell_table[0] = 3000; c.dwell_table_x_en = 0; c.dwell_table_y_en = 0;
        c.max_adv_deg = 500;  c.min_adv_deg = -100;
        c.ign_table_x_src = SIG_RPM; c.ign_table_y_src = SIG_MAP;
        c.clt_ign_corr_table_x_src = SIG_CLT; c.clt_ign_corr_table_y_src = SIG_MAP; c.clt_ign_corr_table_y_en = 0;
        c.ign_rpm_axis_n = 16; c.ign_load_axis_n = 16;
    c.clt_axis_n=14; c.clt_adv_map_axis_n=8;  // resizable correction-axis live bins
        for (int i = 0; i < 16; i++) c.ign_rpm_axis[i]  = static_cast<float>(500 + i * 500);
        for (int i = 0; i < 16; i++) c.ign_load_axis[i] = static_cast<float>(10 + i * 10);  // real kPa
        for (int i = 0; i < 16; i++) { c.clt_ign_corr_table[i] = 0; c.clt_axis[i] = static_cast<float>(-40 + i * 10); }
        for (int li = 0; li < 16; li++)
            for (int ri = 0; ri < 16; ri++)
                c.ign_table[li * IGNITION_IGN_RPM_AXIS_ALLOC + ri] =
                    static_cast<int16_t>((ri * 4 + li) * 10);   // deg x10, at the physical stride

        Ignition ic;
        ic.init(c);
        EngineFrame frame{};
        // rpm = rpm_axis[3] = 2000, load = map = load_axis[5] = 60 kPa → cell(3,5) = 3*4+5 = 17°.
        { SignalBus _b = make_bus(60.0f, 80.0f); ic.update(make_pos(2000.0f), _b, frame); }
        fprintf(stdout, "    adv@(rpm_idx3,load_idx5) = %g deg (expect 17)\n",
                (double)frame.ign_advance_deg);
        CHECK_NEAR(frame.ign_advance_deg, 17.0, 0.05);
    }
    // (advance/dwell are published to the bus + control frame each update; on_engine_stop
    //  no longer stores/zeroes them — only resets the latched knock retard)

    return test_summary();
}
