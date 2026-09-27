// Host test for TractionControl — wheel slip, and the three ways it pulls engine output back.
//
// The config comes from g_config.traction_control (the shipped defaults) rather than a hand-built
// struct, because the target, retard and cut are CURVES: a zeroed axis makes every table_eval return
// the first cell, and a test over a degenerate table passes while proving nothing.
#include "test_helpers.h"
#include "../firmware/Engine/Modules/TractionControl.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "well_known_signals.h"
#include "../generated/signal_ids.h"
#include "../generated/ecu_config.h"
#include "../generated/module_dtc.h"

static uint32_t g_ms = 1000;
extern "C" uint32_t platform_get_tick_ms() { return g_ms; }
volatile uint32_t g_config_generation = 0;
// The written byte range, which on target the comms layer records (see CommsManager). A host test
// writes g_config directly, so it records none — and Sensors then falls back to the full sweep, which
// is exactly the behaviour an unknown range is supposed to get.
volatile uint32_t g_config_dirty_lo = 0xFFFFFFFFu;
volatile uint32_t g_config_dirty_hi = 0;

static TractionControlConfig make_cfg() {
    TractionControlConfig c = g_config.traction_control;   // the shipped curves, so the tables are real
    c.enabled       = 1;
    c.driven_axle   = 1;                                   // rear
    c.min_speed_kph = 5;
    return c;
}

// One frame at the given wheel speeds. dt defaults to the module's 5 ms cadence.
//
// `spd` is the ROAD SPEED — what Vehicle Speed's Main Source publishes, which is the module's reference
// AND the axis its slip target is scheduled on. It defaults to the front average because these tests are
// mostly a rear-drive car, where the front axle is what a sensible Main Source names; a front-drive test
// has to say so, exactly as a front-drive car's tune would.
static void run(TractionControl& t, SignalBus& bus,
                float fl, float fr, float rl, float rr, uint32_t dt = 5, float spd = -1.0f) {
    g_ms += dt;
    EnginePosition p{}; EngineFrame f{};
    bus.set(SIG_WHEEL_FL, fl, true, g_ms, 0); bus.set(SIG_WHEEL_FR, fr, true, g_ms, 0);
    bus.set(SIG_WHEEL_RL, rl, true, g_ms, 0); bus.set(SIG_WHEEL_RR, rr, true, g_ms, 0);
    bus.set(wk::vehicle_spd, spd >= 0.0f ? spd : (fl + fr) * 0.5f, true, g_ms, 0);
    t.update(p, bus, f);
}

// Hold a condition for a while, so the integral and the DTC timers have something to work with.
static void hold(TractionControl& t, SignalBus& bus,
                 float fl, float fr, float rl, float rr, int frames) {
    for (int i = 0; i < frames; ++i) run(t, bus, fl, fr, rl, rr);
}

int main() {
    fprintf(stdout, "=== TractionControl ===\n");
    DtcManager dtc; dtc.set_active(true);   // runtime codes are suppressed on the bench otherwise
    auto raised = [&](uint16_t code) { return dtc.code_severity(code) != 0; };

    SECTION("no slip -> no cap, no retard, no cut");
    {
        auto cfg = make_cfg(); TractionControl t; t.init(cfg); t.set_dtc(&dtc);
        SignalBus bus{};
        hold(t, bus, 100, 100, 100, 100, 20);
        CHECK_NEAR(t.cap(), 100.0f, 0.1f);
        CHECK_NEAR(t.retard(), 0.0f, 0.01f);
        CHECK_NEAR(t.cut_pct(), 0.0f, 0.01f);
    }

    SECTION("slip is measured off the FASTEST driven wheel, not the pair's average");
    {
        // One rear wheel spinning at 130 and the other gripping at 100, fronts honest at 100. The
        // fastest is 30% slip; averaging the pair would report 15% — half of what is happening, which is
        // exactly the case an open diff produces. Red if driven ever becomes (RL+RR)/2.
        auto cfg = make_cfg(); TractionControl t; t.init(cfg); t.set_dtc(&dtc);
        SignalBus bus{};
        run(t, bus, 100, 100, 100, 130);
        CHECK_NEAR(t.slip(), 30.0f, 0.1f);
    }

    SECTION("a gearbox pickup and ONE undriven wheel is a complete install");
    {
        // The common cheap setup on a rear-drive car: a drive train sensor, a front-left pickup, and no
        // driven-wheel pickups at all. Red before Driven Speed From existed — the module looked for rear
        // wheels, found neither, and raised TRACTION_NO_DRIVEN on a car that can measure slip perfectly
        // well.
        auto cfg = make_cfg(); cfg.driven_src = TractionControl::DriveTrain;
        TractionControl t; t.init(cfg); t.set_dtc(&dtc);
        SignalBus bus{};
        g_ms += 5; EnginePosition p{}; EngineFrame f{};
        bus.invalidate(SIG_WHEEL_RL); bus.invalidate(SIG_WHEEL_RR);   // no driven-wheel pickups
        bus.invalidate(SIG_WHEEL_FR);                                 // and only ONE undriven wheel
        bus.set(SIG_WHEEL_FL,  100.0f, true, g_ms, 0);
        bus.set(SIG_SHAFT_SPD, 120.0f, true, g_ms, 0);                // the gearbox, through the diff
        bus.set(wk::vehicle_spd, 100.0f, true, g_ms, 0);              // Main Source = Front Left
        t.update(p, bus, f);
        CHECK_NEAR(t.slip(), 20.0f, 0.1f);
        CHECK(!raised(ModuleDtc::TRACTION_NO_DRIVEN));
    }

    SECTION("...and a drive train pickup that stops reading still says so");
    {
        auto cfg = make_cfg(); cfg.driven_src = TractionControl::DriveTrain;
        TractionControl t; t.init(cfg); t.set_dtc(&dtc);
        SignalBus bus{};
        g_ms += 5; EnginePosition p{}; EngineFrame f{};
        bus.invalidate(SIG_SHAFT_SPD);
        bus.set(SIG_WHEEL_FL, 100.0f, true, g_ms, 0);
        bus.set(wk::vehicle_spd, 100.0f, true, g_ms, 0);
        t.update(p, bus, f);
        CHECK_NEAR(t.cap(), 100.0f, 0.1f);
        CHECK(raised(ModuleDtc::TRACTION_NO_DRIVEN));
    }

    SECTION("the reference is the ROAD SPEED, whatever Vehicle Speed says it is");
    {
        // No undriven pickups at all — the two-pickup car, and the four-wheel-drive car reading GPS.
        // Red the moment this module goes back to working the reference out for itself: it would need
        // the front wheels, find neither, and refuse.
        auto cfg = make_cfg(); TractionControl t; t.init(cfg); t.set_dtc(&dtc);
        SignalBus bus{};
        g_ms += 5; EnginePosition p{}; EngineFrame f{};
        bus.invalidate(SIG_WHEEL_FL); bus.invalidate(SIG_WHEEL_FR);
        bus.set(SIG_WHEEL_RL, 110.0f, true, g_ms, 0); bus.set(SIG_WHEEL_RR, 110.0f, true, g_ms, 0);
        bus.set(wk::vehicle_spd, 100.0f, true, g_ms, 0);
        t.update(p, bus, f);
        CHECK_NEAR(t.slip(), 10.0f, 0.1f);
    }

    SECTION("no road speed at all -> no cap, and a code that says so");
    {
        auto cfg = make_cfg(); TractionControl t; t.init(cfg); t.set_dtc(&dtc);
        SignalBus bus{};
        g_ms += 5; EnginePosition p{}; EngineFrame f{};
        bus.invalidate(wk::vehicle_spd);
        bus.set(SIG_WHEEL_RL, 200.0f, true, g_ms, 0); bus.set(SIG_WHEEL_RR, 200.0f, true, g_ms, 0);
        t.update(p, bus, f);
        CHECK_NEAR(t.cap(), 100.0f, 0.1f);
        // "Not active" is also what a below-minimum speed gives, so only the code tells them apart.
        CHECK(raised(ModuleDtc::TRACTION_NO_REF));
        hold(t, bus, 100, 100, 100, 100, 3);
        CHECK(!raised(ModuleDtc::TRACTION_NO_REF));   // and it heals
    }

    SECTION("over target the throttle ceiling comes down, and the integral keeps pulling");
    {
        auto cfg = make_cfg(); TractionControl t; t.init(cfg); t.set_dtc(&dtc);
        SignalBus bus{};
        // 10% slip against a ~5.5% target here: small enough that the proportional term does NOT
        // saturate, which is the only way the integral's contribution is visible at all. (At 40% slip
        // the P term alone pins the cap to its floor on the first frame and there is nothing to see.)
        run(t, bus, 100, 100, 110, 110);
        const float first = t.cap();
        CHECK(first < 100.0f);                              // proportional term, immediately
        hold(t, bus, 100, 100, 110, 110, 200);
        // Red with a P-only controller: the cap would sit exactly where the first frame put it.
        CHECK(t.cap() < first);
    }

    SECTION("the ceiling is GIVEN BACK at a rate, not snapped back");
    {
        auto cfg = make_cfg(); cfg.release_pct_s = 100;     // 100 %/s = 0.5% per 5 ms frame
        TractionControl t; t.init(cfg); t.set_dtc(&dtc);
        SignalBus bus{};
        hold(t, bus, 100, 100, 140, 140, 50);
        const float capped = t.cap();
        CHECK(capped < 90.0f);
        run(t, bus, 100, 100, 100, 100);                    // slip gone, this frame
        CHECK(t.cap() > capped);                            // …giving back
        CHECK(t.cap() < 100.0f);                            // …but NOT all of it: red if it snaps
        hold(t, bus, 100, 100, 100, 100, 500);
        CHECK_NEAR(t.cap(), 100.0f, 0.1f);                  // and it does get all the way back
    }

    SECTION("the ceiling never goes below its floor");
    {
        auto cfg = make_cfg(); TractionControl t; t.init(cfg); t.set_dtc(&dtc);
        SignalBus bus{};
        hold(t, bus, 100, 100, 400, 400, 200);              // 300% slip, held
        CHECK_NEAR(t.cap(), static_cast<float>(cfg.cap_min_pct) * 0.1f, 0.5f);
    }

    SECTION("retard and cut come off curves read on slip ERROR, not slip");
    {
        auto cfg = make_cfg(); TractionControl t; t.init(cfg); t.set_dtc(&dtc);
        SignalBus bus{};
        run(t, bus, 100, 100, 105, 105);                    // 5% slip: under target at this speed
        CHECK_NEAR(t.retard(), 0.0f, 0.01f);
        CHECK_NEAR(t.cut_pct(), 0.0f, 0.01f);
        run(t, bus, 100, 100, 160, 160);                    // 60% slip: well over
        CHECK(t.retard() > 0.0f);
        CHECK(t.cut_pct() > 0.0f);
    }

    SECTION("a cut percentage is spread across frames, not held on");
    {
        // 50% duty must cut about half the frames. Red for a threshold ("cut while over X"), which is
        // what the rev limiter's soft cut used to do: nothing, then everything.
        CutDuty d;
        int cuts = 0;
        for (int i = 0; i < 100; ++i) cuts += d.step(50.0f) ? 1 : 0;
        CHECK(cuts >= 49 && cuts <= 51);
        CutDuty e; cuts = 0;
        for (int i = 0; i < 100; ++i) cuts += e.step(25.0f) ? 1 : 0;
        CHECK(cuts >= 24 && cuts <= 26);
        CutDuty f; cuts = 0;
        for (int i = 0; i < 100; ++i) cuts += f.step(0.0f) ? 1 : 0;
        CHECK(cuts == 0);
        CutDuty g; cuts = 0;
        for (int i = 0; i < 100; ++i) cuts += g.step(100.0f) ? 1 : 0;
        CHECK(cuts == 100);
    }

    SECTION("below the speed floor it does nothing");
    {
        auto cfg = make_cfg(); TractionControl t; t.init(cfg); t.set_dtc(&dtc);
        SignalBus bus{};
        hold(t, bus, 2, 2, 20, 20, 10);                     // reference 2 kph < 5
        CHECK_NEAR(t.cap(), 100.0f, 0.1f);
    }

    SECTION("front-driven measures it the other way round");
    {
        // …and its Main Source names the REAR axle, which is this car's undriven one. Passing the road
        // speed explicitly is the test saying that out loud rather than inheriting a rear-drive default.
        auto cfg = make_cfg(); cfg.driven_axle = 0;
        TractionControl t; t.init(cfg); t.set_dtc(&dtc);
        SignalBus bus{};
        run(t, bus, 130, 130, 100, 100, 5, 100.0f);
        CHECK_NEAR(t.slip(), 30.0f, 0.1f);
    }

    SECTION("disabled -> no cap, and its codes heal");
    {
        auto cfg = make_cfg(); TractionControl t; t.init(cfg); t.set_dtc(&dtc);
        SignalBus bus{};
        hold(t, bus, 100, 100, 200, 200, 10);
        CHECK(t.cap() < 100.0f);
        cfg.enabled = 0; t.on_config_change(cfg);
        run(t, bus, 100, 100, 200, 200);
        CHECK_NEAR(t.cap(), 100.0f, 0.1f);
    }

    SECTION("steady slip at a cruise, with nothing being pulled, is a CALIBRATION code");
    {
        // 3% slip at 100 kph for over ten seconds while the module is doing nothing about it. That is
        // not wheelspin — it is the two speed sources disagreeing, and it biases every measurement made
        // above. Red if the check ever fires while the module is actually intervening.
        auto cfg = make_cfg(); TractionControl t; t.init(cfg); t.set_dtc(&dtc);
        SignalBus bus{};
        hold(t, bus, 100, 100, 103, 103, 2600);             // 2600 x 5 ms = 13 s
        CHECK(raised(ModuleDtc::TRACTION_AXLE_CAL));
        hold(t, bus, 100, 100, 100, 100, 2000);
        CHECK(!raised(ModuleDtc::TRACTION_AXLE_CAL));
    }

    fprintf(stdout, "=== TractionControl: all good ===\n");
    return test_summary();
}
