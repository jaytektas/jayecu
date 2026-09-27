// Host test for LambdaProtect — lean-protection fuel cut (too lean under load past a timeout).
#include "test_helpers.h"
#include "../firmware/Engine/Modules/LambdaProtect.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "well_known_signals.h"
#include "../generated/signal_ids.h"

static uint32_t g_ms = 0;
extern "C" uint32_t platform_get_tick_ms() { return g_ms; }

static LambdaProtectConfig make_cfg() {
    LambdaProtectConfig c{};
    c.enabled    = 1;
    c.max_lambda = 1100;   // 1.100 lean limit
    c.min_tps    = 600;    // 60.0%
    c.min_rpm    = 2000;
    c.timeout_ms = 500;
    return c;
}

static bool run(LambdaProtect& lp, SignalBus& bus, uint32_t dt,
                float rpm, float tps, float lambda, bool prior = false) {
    g_ms += dt;
    EnginePosition p{}; p.rpm = rpm;
    if (prior) bus.set_bool(wk::fuel_cut, true);
    EngineFrame f{};
    bus.set(wk::tps, tps); bus.set(SIG_LAMBDA_1, lambda);
    lp.update(p, bus, f);
    return bus.valid(wk::fuel_cut);
}

int main() {
    fprintf(stdout, "=== LambdaProtect ===\n");

    SECTION("lean under load past timeout -> cut");
    {
        auto cfg = make_cfg(); LambdaProtect lp; lp.init(cfg);
        SignalBus bus{};
        run(lp, bus, 0, 4000, 80.0f, 1.20f);   // seed last_ms_ (dt=0)
        CHECK(!lp.cut());
        for (int i = 0; i < 60; i++) run(lp, bus, 10, 4000, 80.0f, 1.20f);  // 600 ms lean > 500
        CHECK(lp.cut());
        CHECK(bus.get_bool(SIG_LAMBDA_PROTECT_ACTIVE));
    }

    SECTION("rich under load -> no cut");
    {
        auto cfg = make_cfg(); LambdaProtect lp; lp.init(cfg);
        SignalBus bus{};
        run(lp, bus, 0, 4000, 80.0f, 0.90f);
        for (int i = 0; i < 60; i++) run(lp, bus, 10, 4000, 80.0f, 0.90f);
        CHECK(!lp.cut());
    }

    SECTION("lean off-throttle -> not monitored, no cut");
    {
        auto cfg = make_cfg(); LambdaProtect lp; lp.init(cfg);
        SignalBus bus{};
        run(lp, bus, 0, 4000, 10.0f, 1.30f);
        for (int i = 0; i < 60; i++) run(lp, bus, 10, 4000, 10.0f, 1.30f);  // tps 10% < 60%
        CHECK(!lp.cut());
    }

    SECTION("lean below timeout -> no cut yet");
    {
        auto cfg = make_cfg(); LambdaProtect lp; lp.init(cfg);
        SignalBus bus{};
        run(lp, bus, 0, 4000, 80.0f, 1.20f);
        for (int i = 0; i < 30; i++) run(lp, bus, 10, 4000, 80.0f, 1.20f);  // 300 ms < 500
        CHECK(!lp.cut());
    }

    SECTION("cut latches until lift (load drops out of the window)");
    {
        auto cfg = make_cfg(); LambdaProtect lp; lp.init(cfg);
        SignalBus bus{};
        run(lp, bus, 0, 4000, 80.0f, 1.20f);
        for (int i = 0; i < 60; i++) run(lp, bus, 10, 4000, 80.0f, 1.20f);
        CHECK(lp.cut());
        run(lp, bus, 10, 4000, 10.0f, 1.20f);   // lift -> not monitored -> release
        CHECK(!lp.cut());
    }

    SECTION("ORs into the cut — never clears another module's cut");
    {
        auto cfg = make_cfg(); LambdaProtect lp; lp.init(cfg);
        SignalBus bus{};
        CHECK(run(lp, bus, 10, 4000, 80.0f, 0.90f, /*prior=*/true) == true);  // rich, but prior cut holds
        CHECK(!lp.cut());
    }

    SECTION("bank 2 lean is watched (it read only SIG_LAMBDA_1)");
    {
        auto cfg = make_cfg(); LambdaProtect lp; lp.init(cfg);
        SignalBus bus{};
        auto step = [&](uint32_t dt) {
            g_ms += dt; EnginePosition p{}; p.rpm = 4000; EngineFrame f{};
            bus.set(wk::tps, 80.0f);
            bus.set(SIG_LAMBDA_BANK_1, 0.90f); bus.set(SIG_LAMBDA_BANK_2, 1.25f);   // bank 2 is starving
            lp.update(p, bus, f);
        };
        step(0);
        for (int i = 0; i < 60; i++) step(10);
        CHECK(lp.cut());
    }

    SECTION("lean against the TARGET: 0.78 target, 0.95 measured cuts under the fixed 1.10 limit");
    {
        auto cfg = make_cfg(); cfg.lean_margin_pct = 100;   // 10 %
        LambdaProtect lp; lp.init(cfg);
        SignalBus bus{};
        auto step = [&](uint32_t dt, float meas) {
            g_ms += dt; EnginePosition p{}; p.rpm = 4000; EngineFrame f{};
            bus.set(wk::tps, 80.0f); bus.set(wk::lambda_target, 0.78f); bus.set(SIG_LAMBDA_OVERALL, meas);
            lp.update(p, bus, f);
        };
        step(0, 0.80f);
        for (int i = 0; i < 60; i++) step(10, 0.80f);         // on target: fine
        CHECK(!lp.cut());
        for (int i = 0; i < 60; i++) step(10, 0.95f);         // 22 % lean of target
        CHECK(lp.cut());
    }

    SECTION("no lambda reading at all -> not judged lean (and never as 'exactly stoich')");
    {
        auto cfg = make_cfg(); LambdaProtect lp; lp.init(cfg);
        SignalBus bus{};
        for (int i = 0; i < 60; i++) { g_ms += 10; EnginePosition p{}; p.rpm = 4000; EngineFrame f{};
                                       bus.set(wk::tps, 80.0f); lp.update(p, bus, f); }
        CHECK(!lp.cut());
    }

    SECTION("disabled -> no cut");
    {
        auto cfg = make_cfg(); cfg.enabled = 0; LambdaProtect lp; lp.init(cfg);
        SignalBus bus{};
        for (int i = 0; i < 60; i++) run(lp, bus, 10, 4000, 80.0f, 1.30f);
        CHECK(!lp.cut());
    }

    return test_summary();
}
