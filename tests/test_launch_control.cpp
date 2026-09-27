// Host test for Launch — the two-step, and the calibration the engine runs on while it holds.
//
// The config comes from g_config.launch (the shipped defaults) rather than a hand-built struct,
// because the limit and both maps are TABLES: a zeroed grid makes every table_eval return an empty
// first cell, an End RPM of 0 cuts at every RPM, and a test over that passes while proving nothing.
#include "test_helpers.h"
#include "../firmware/Engine/Modules/Launch.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Signal/Expr.h"
#include "../firmware/Engine/EngineFrame.h"
#include "../firmware/Diagnostics/DtcManager.h"
#include "well_known_signals.h"   // wk:: cut/prot signal roles
#include "../generated/signal_ids.h"
#include "../generated/ecu_config.h"
#include "../generated/module_dtc.h"
#include <cstring>

static uint32_t g_ms = 1000;
extern "C" uint32_t platform_get_tick_ms() { return g_ms; }
volatile uint32_t g_config_generation = 0;
// The written byte range, which on target the comms layer records (see CommsManager). A host test
// writes g_config directly, so it records none.
volatile uint32_t g_config_dirty_lo = 0xFFFFFFFFu;
volatile uint32_t g_config_dirty_hi = 0;

// Arming is an EXPRESSION, so a test hands the module bytecode. `<signal> != 0` is what the studio
// compiles a plain switch to, and it is what the real car will carry.
static void switch_prog(uint8_t* dst, size_t cap, SignalId s) {
    const uint16_t sel = (uint16_t)(s + 1);        // options_from:signals — 0 is None, else id+1
    const uint8_t prog[] = { (uint8_t)expr::OP_PUSH_SIG, (uint8_t)(sel & 0xFF), (uint8_t)(sel >> 8),
                             (uint8_t)expr::OP_PUSH_I8, 0,
                             (uint8_t)expr::OP_NE, (uint8_t)expr::OP_END };
    memset(dst, 0, cap);
    memcpy(dst, prog, sizeof prog);
}

static LaunchConfig make_cfg(uint8_t cut_type = Launch::HardCut,
                             uint8_t cut_method = Launch::CutFuel) {
    LaunchConfig c = g_config.launch;     // the shipped tables, so the maps and the limit are real
    c.enabled     = 1;
    c.cut_type    = cut_type;
    c.cut_method  = cut_method;
    c.timeout_s   = 0;                    // the timeout has its own section; off everywhere else
    switch_prog(c.arm_expr, sizeof c.arm_expr, SIG_LAUNCH_SW);
    return c;
}

static EnginePosition make_pos(float rpm) { EnginePosition p{}; p.rpm = rpm; return p; }

// A bus with the arm switch set and the two channels the launch maps are read against. RPM is on the
// bus as well as in EnginePosition because a table axis can only read the bus — the module compares
// pos.rpm against the limit and the maps index on the channel, and a test that sets only one of them
// moves half the module.
static SignalBus make_bus(bool arm, float rpm, float load = 100.0f) {
    SignalBus b{};
    b.set_bool(SIG_LAUNCH_SW, arm, g_ms, 0);
    b.set(wk::rpm, rpm, true, g_ms, 0);
    b.set(wk::fuel_load, load, true, g_ms, 0);
    return b;
}

// One frame. The cut signals are invalidated first because a cut is released by its signal EXPIRING —
// validity is the OR — so without clearing them a test would read last frame's cut as this frame's.
static void run(Launch& lc, SignalBus& bus, float rpm, EngineFrame& f, uint32_t dt = 5) {
    g_ms += dt;
    bus.invalidate(wk::fuel_cut);
    bus.invalidate(wk::ign_cut);
    bus.set(wk::rpm, rpm, true, g_ms, 0);
    lc.update(make_pos(rpm), bus, f);
}

// How many of `frames` frames actually cut, which is the only way to read a DUTY: a soft cut is spread
// across frames, so one frame says nothing about the percentage being delivered.
static int cut_frames(Launch& lc, SignalBus& bus, float rpm, SignalId which, int frames) {
    int n = 0;
    for (int i = 0; i < frames; ++i) {
        EngineFrame f{};
        run(lc, bus, rpm, f);
        if (bus.valid(which)) ++n;
    }
    return n;
}

int main() {
    fprintf(stdout, "=== Launch ===\n");
    DtcManager dtc; dtc.set_active(true);   // runtime codes are suppressed on the bench otherwise

    SECTION("disabled — no cut at any RPM, and the launch chain rests neutral");
    {
        auto cfg = make_cfg(); cfg.enabled = 0;
        Launch lc; lc.init(cfg); lc.set_dtc(&dtc);
        EngineFrame f{};
        SignalBus bus = make_bus(true, 10000.0f);
        run(lc, bus, 10000.0f, f);
        CHECK(!bus.valid(wk::fuel_cut));
        CHECK(!bus.valid(wk::ign_cut));
        CHECK(!bus.get_bool(wk::launch_active));
        CHECK_NEAR(bus.get(wk::fuel_corr_launch), 1.0f, 0.001f);   // 1.0x = no fuel correction
        CHECK_NEAR(bus.get(wk::launch_ign_adv), 0.0f, 0.001f);
    }

    SECTION("arm condition false — no cut above the End RPM");
    {
        auto cfg = make_cfg();
        Launch lc; lc.init(cfg); lc.set_dtc(&dtc);
        EngineFrame f{};
        SignalBus bus = make_bus(/*arm*/false, 6000.0f);
        run(lc, bus, 6000.0f, f);                     // well above the shipped 5000
        CHECK(!bus.valid(wk::fuel_cut));
        CHECK(!bus.get_bool(wk::launch_active));
    }

    SECTION("an EMPTY arm condition is the built-in rule: armed only STANDING STILL WITH THE THROTTLE OPEN");
    {
        // "Always armed" put the launch maps in charge for the first half-minute of every drive; "standing
        // still" alone put them in charge of the idle at every stop, with the idle ignition correction off.
        auto cfg = make_cfg();
        memset(cfg.arm_expr, 0, sizeof cfg.arm_expr);     // empty
        EngineFrame f{};
        {
            Launch idle; idle.init(cfg); idle.set_dtc(&dtc);
            SignalBus bi = make_bus(false, 900.0f);
            bi.set(wk::vehicle_spd, 0.0f, true, g_ms, 0);  // stopped at the lights...
            bi.set(wk::tps, 1.0f, true, g_ms, 0);          // ...with the throttle closed
            run(idle, bi, 900.0f, f);
            CHECK(!bi.get_bool(wk::launch_active));        // idling is not a launch
        }
        {
            Launch dbw; dbw.init(cfg); dbw.set_dtc(&dtc);   // drive-by-wire: the PEDAL is the driver
            SignalBus bd = make_bus(false, 3000.0f);
            bd.set(wk::vehicle_spd, 0.0f, true, g_ms, 0);
            bd.set(wk::tps, 8.0f, true, g_ms, 0);           // the plate the ECU is holding
            bd.set(SIG_PEDAL_DEMAND, 100.0f, true, g_ms, 0);
            run(dbw, bd, 3000.0f, f);
            CHECK(bd.get_bool(wk::launch_active));
        }
        Launch lc; lc.init(cfg); lc.set_dtc(&dtc);
        SignalBus bus = make_bus(/*arm*/false, 3000.0f);
        bus.set(wk::vehicle_spd, 0.0f, true, g_ms, 0);     // stationary
        bus.set(wk::tps, 80.0f, true, g_ms, 0);            // throttle open on the line
        run(lc, bus, 3000.0f, f);
        CHECK(bus.get_bool(wk::launch_active));
        CHECK(!bus.valid(wk::fuel_cut));                  // armed, but under the limit
        Launch moving; moving.init(cfg); moving.set_dtc(&dtc);
        SignalBus b2 = make_bus(false, 3000.0f);
        b2.set(wk::vehicle_spd, 60.0f, true, g_ms, 0);     // driving
        b2.set(wk::tps, 80.0f, true, g_ms, 0);
        run(moving, b2, 3000.0f, f);
        CHECK(!b2.get_bool(wk::launch_active));
        Launch nospeed; nospeed.init(cfg); nospeed.set_dtc(&dtc);
        SignalBus b3 = make_bus(false, 3000.0f);           // no road speed at all
        run(nospeed, b3, 3000.0f, f);
        CHECK(!b3.get_bool(wk::launch_active));
    }

    SECTION("an arm condition that will not compile arms NOTHING and raises P1740");
    {
        auto cfg = make_cfg();
        memset(cfg.arm_expr, 0, sizeof cfg.arm_expr);
        cfg.arm_expr[0] = 0x7F;                           // not an opcode
        g_config_generation++;                            // the module revalidates on the counter
        Launch lc; lc.init(cfg); lc.set_dtc(&dtc);
        EngineFrame f{};
        SignalBus bus = make_bus(true, 6000.0f);
        run(lc, bus, 6000.0f, f);
        CHECK(dtc.code_severity(ModuleDtc::LAUNCH_EXPR) != 0);
        CHECK(!bus.get_bool(wk::launch_active));
        CHECK(!bus.valid(wk::fuel_cut));                  // NOT "the driver is not launching"
    }
    g_config_generation++;                                // …and the next config is a fresh one

    SECTION("armed below the End RPM — active, but nothing is cut");
    {
        auto cfg = make_cfg();
        Launch lc; lc.init(cfg); lc.set_dtc(&dtc);
        EngineFrame f{};
        SignalBus bus = make_bus(true, 3000.0f);
        run(lc, bus, 3000.0f, f);
        CHECK(bus.get_bool(wk::launch_active));
        CHECK(!bus.valid(wk::fuel_cut));
        CHECK_NEAR(bus.get(wk::launch_end_rpm), 5000.0f, 0.5f);   // the shipped single cell
    }

    SECTION("hard cut: everything at the End RPM, released a resume band below it");
    {
        auto cfg = make_cfg(Launch::HardCut, Launch::CutFuel);
        cfg.resume_band_rpm = 200;
        Launch lc; lc.init(cfg); lc.set_dtc(&dtc);
        SignalBus bus = make_bus(true, 5100.0f);
        EngineFrame f{};
        run(lc, bus, 5100.0f, f);  CHECK(bus.valid(wk::fuel_cut));
        CHECK(!bus.valid(wk::ign_cut));                       // fuel method: spark is untouched
        CHECK_NEAR(bus.get(wk::launch_cut_pct), 100.0f, 0.1f);
        CHECK_NEAR(f.effective_rpm_limit, 5000.0f, 0.5f);
        // 4900 is under the limit but above resume (5000-200) -> still cut.
        run(lc, bus, 4900.0f, f);  CHECK(bus.valid(wk::fuel_cut));
        // 4700 is below resume -> releases.
        run(lc, bus, 4700.0f, f);  CHECK(!bus.valid(wk::fuel_cut));
    }

    SECTION("soft cut is a DUTY across the range, not a second hard cut at its midpoint");
    {
        auto cfg = make_cfg(Launch::SoftCut, Launch::CutFuel);
        cfg.cut_range_rpm = 500;                       // 4500..5000
        Launch lc; lc.init(cfg); lc.set_dtc(&dtc);
        SignalBus bus = make_bus(true, 4600.0f);
        // A fifth of the way in: about 20% of frames cut. A threshold implementation cuts NONE here
        // and ALL of them at 4800 — which is what this pair of counts exists to tell apart.
        const int low = cut_frames(lc, bus, 4600.0f, wk::fuel_cut, 100);
        CHECK(low > 10 && low < 30);
        const int high = cut_frames(lc, bus, 4900.0f, wk::fuel_cut, 100);
        CHECK(high > 70 && high < 90);                 // four fifths in
        const int under = cut_frames(lc, bus, 4400.0f, wk::fuel_cut, 50);
        CHECK(under == 0);                             // below the range: nothing at all
        const int over = cut_frames(lc, bus, 5200.0f, wk::fuel_cut, 50);
        CHECK(over == 50);                             // past the End RPM: every frame
    }

    SECTION("cut method picks the channel");
    {
        {   auto cfg = make_cfg(Launch::HardCut, Launch::CutIgnition);
            Launch lc; lc.init(cfg); lc.set_dtc(&dtc);
            EngineFrame f{}; SignalBus bus = make_bus(true, 5200.0f);
            run(lc, bus, 5200.0f, f);
            CHECK(!bus.valid(wk::fuel_cut));
            CHECK(bus.valid(wk::ign_cut)); }
        {   auto cfg = make_cfg(Launch::HardCut, Launch::CutBoth);
            Launch lc; lc.init(cfg); lc.set_dtc(&dtc);
            EngineFrame f{}; SignalBus bus = make_bus(true, 5200.0f);
            run(lc, bus, 5200.0f, f);
            CHECK(bus.valid(wk::fuel_cut));
            CHECK(bus.valid(wk::ign_cut)); }
    }

    SECTION("the adder staggers the second cut above the leading one");
    {
        auto cfg = make_cfg(Launch::HardCut, Launch::CutBoth);
        cfg.cut_adder_rpm = 200;
        cfg.cut_lead      = Launch::LeadIgnition;      // ignition at 5000, fuel at 5200
        Launch lc; lc.init(cfg); lc.set_dtc(&dtc);
        SignalBus bus = make_bus(true, 5100.0f);
        EngineFrame f{};
        run(lc, bus, 5100.0f, f);
        CHECK(bus.valid(wk::ign_cut));
        CHECK(!bus.valid(wk::fuel_cut));               // still below the follower's threshold
        run(lc, bus, 5300.0f, f);
        CHECK(bus.valid(wk::ign_cut));
        CHECK(bus.valid(wk::fuel_cut));                // now both
    }

    SECTION("the leading cut is a setting, not a convention");
    {
        auto cfg = make_cfg(Launch::HardCut, Launch::CutBoth);
        cfg.cut_adder_rpm = 200;
        cfg.cut_lead      = Launch::LeadFuel;          // fuel at 5000, ignition at 5200
        Launch lc; lc.init(cfg); lc.set_dtc(&dtc);
        EngineFrame f{}; SignalBus bus = make_bus(true, 5100.0f);
        run(lc, bus, 5100.0f, f);
        CHECK(bus.valid(wk::fuel_cut));
        CHECK(!bus.valid(wk::ign_cut));
    }

    SECTION("the ignition map is published as an ABSOLUTE advance, read at rpm x load");
    {
        auto cfg = make_cfg();
        Launch lc; lc.init(cfg); lc.set_dtc(&dtc);
        EngineFrame f{};
        // 5000 rpm x 100 kPa is a shipped cell: 22.0 deg. Off boost at the same RPM it is 30.0 —
        // a NORMAL advance, which is the whole point of the table being absolute.
        SignalBus bus = make_bus(true, 5000.0f, /*load*/100.0f);
        run(lc, bus, 5000.0f, f);
        CHECK_NEAR(bus.get(wk::launch_ign_adv), 22.0f, 0.05f);
        bus.set(wk::fuel_load, 0.0f, true, g_ms, 0);
        run(lc, bus, 5000.0f, f);
        CHECK_NEAR(bus.get(wk::launch_ign_adv), 30.0f, 0.05f);
        // …and into boost it is retarded past TDC, which a retard-style table could not express.
        bus.set(wk::fuel_load, 250.0f, true, g_ms, 0);
        run(lc, bus, 6000.0f, f);
        CHECK_NEAR(bus.get(wk::launch_ign_adv), -12.0f, 0.05f);
    }

    SECTION("the fuel map is a percentage, published as a multiplier");
    {
        auto cfg = make_cfg();
        // Row 0 (0 kPa), every RPM column: +10 %.
        for (int i = 0; i < 8; ++i) cfg.fuel_corr_table[i] = 100;
        Launch lc; lc.init(cfg); lc.set_dtc(&dtc);
        EngineFrame f{};
        SignalBus bus = make_bus(true, 3000.0f, /*load*/0.0f);
        run(lc, bus, 3000.0f, f);
        CHECK_NEAR(bus.get(wk::fuel_corr_launch), 1.10f, 0.001f);
        // The shipped table is zero everywhere, which has to read as 1.0x and not as 0.
        auto plain = make_cfg();
        Launch lc2; lc2.init(plain); lc2.set_dtc(&dtc);
        EngineFrame f2{}; SignalBus b2 = make_bus(true, 3000.0f, 100.0f);
        run(lc2, b2, 3000.0f, f2);
        CHECK_NEAR(b2.get(wk::fuel_corr_launch), 1.0f, 0.001f);
    }

    SECTION("the End RPM table is the limit: switch the load axis on and the limit moves with boost");
    {
        auto cfg = make_cfg();
        cfg.end_rpm_table_x_en = 1;                    // x = fuel_load, bins 0,50,75,100,150,200,250,300
        cfg.end_rpm_table[0] = 3000;                   //   0 kPa -> 3000
        cfg.end_rpm_table[3] = 5000;                   // 100 kPa -> 5000
        Launch lc; lc.init(cfg); lc.set_dtc(&dtc);
        EngineFrame f{};
        SignalBus bus = make_bus(true, 3500.0f, /*load*/0.0f);
        run(lc, bus, 3500.0f, f);
        CHECK_NEAR(bus.get(wk::launch_end_rpm), 3000.0f, 0.5f);
        CHECK(bus.valid(wk::fuel_cut));                // 3500 is over the limit down here
        bus.set(wk::fuel_load, 100.0f, true, g_ms, 0);
        run(lc, bus, 3500.0f, f);
        CHECK_NEAR(bus.get(wk::launch_end_rpm), 5000.0f, 0.5f);
        CHECK(!bus.valid(wk::fuel_cut));               // …and not, up here
    }

    SECTION("the timeout ends a launch, and only re-arms when the condition cycles");
    {
        auto cfg = make_cfg();
        cfg.timeout_s = 2;
        Launch lc; lc.init(cfg); lc.set_dtc(&dtc);
        EngineFrame f{};
        SignalBus bus = make_bus(true, 5200.0f);
        run(lc, bus, 5200.0f, f);
        CHECK(bus.get_bool(wk::launch_active));
        g_ms += 2500;                                  // hold it past the timeout
        run(lc, bus, 5200.0f, f);
        CHECK(!bus.get_bool(wk::launch_active));
        CHECK(!bus.valid(wk::fuel_cut));
        // Still held: it must NOT come back on its own, however long it is left.
        g_ms += 5000;
        run(lc, bus, 5200.0f, f);
        CHECK(!bus.get_bool(wk::launch_active));
        // Release and press again: a new launch.
        bus.set_bool(SIG_LAUNCH_SW, false, g_ms, 0);
        run(lc, bus, 5200.0f, f);
        bus.set_bool(SIG_LAUNCH_SW, true, g_ms, 0);
        run(lc, bus, 5200.0f, f);
        CHECK(bus.get_bool(wk::launch_active));
        CHECK(bus.valid(wk::fuel_cut));
    }

    SECTION("disarming while cut releases the cut and the maps");
    {
        auto cfg = make_cfg();
        Launch lc; lc.init(cfg); lc.set_dtc(&dtc);
        EngineFrame f{};
        SignalBus bus = make_bus(true, 5200.0f);
        run(lc, bus, 5200.0f, f);
        CHECK(bus.valid(wk::fuel_cut));
        bus.set_bool(SIG_LAUNCH_SW, false, g_ms, 0);
        run(lc, bus, 5200.0f, f);
        CHECK(!bus.valid(wk::fuel_cut));
        CHECK(!bus.get_bool(wk::launch_active));
        CHECK_NEAR(bus.get(wk::fuel_corr_launch), 1.0f, 0.001f);
    }

    SECTION("on_engine_stop clears the latch");
    {
        auto cfg = make_cfg();
        Launch lc; lc.init(cfg); lc.set_dtc(&dtc);
        EngineFrame f{};
        SignalBus bus = make_bus(true, 5200.0f);
        run(lc, bus, 5200.0f, f);
        CHECK(bus.valid(wk::fuel_cut));
        lc.on_engine_stop();
        // Below the resume band now; the latch must not survive the stop.
        run(lc, bus, 4000.0f, f);
        CHECK(!bus.valid(wk::fuel_cut));
    }

    return test_summary();
}
