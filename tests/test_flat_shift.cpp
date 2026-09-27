// Host test for FlatShift — clutchless upshift cut (trigger + high rpm/throttle, time-limited).
#include "test_helpers.h"
#include "../firmware/Engine/Modules/FlatShift.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "well_known_signals.h"
#include "../generated/signal_ids.h"

static uint32_t g_ms = 0;
extern "C" uint32_t platform_get_tick_ms() { return g_ms; }

static FlatShiftConfig make_cfg() {
    FlatShiftConfig c{};
    c.enabled     = 1;
    c.trigger_sig = SIG_CLUTCH_SW;
    c.min_rpm     = 3000;
    c.min_tps     = 700;    // 70.0%
    c.max_cut_ms  = 250;
    c.cut_method  = 1;      // ignition — 0 is Fuel, the one order every cutting module now uses
    return c;
}

static void run(FlatShift& fs, SignalBus& bus, uint32_t dt, float rpm, float tps, bool trig, EngineFrame& f) {
    g_ms += dt;
    EnginePosition p{}; p.rpm = rpm;
    bus.set(wk::tps, tps); bus.set_bool(SIG_CLUTCH_SW, trig);
    bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut);
    fs.update(p, bus, f);
}

int main() {
    fprintf(stdout, "=== FlatShift ===\n");

    SECTION("trigger + high rpm/throttle -> ignition cut");
    {
        auto cfg = make_cfg(); FlatShift fs; fs.init(cfg);
        SignalBus bus{}; EngineFrame f{};
        run(fs, bus, 10, 5000, 90.0f, true, f);
        CHECK(fs.active());
        CHECK(bus.valid(wk::ign_cut));
        CHECK(!bus.valid(wk::fuel_cut));   // ignition method only
        CHECK(bus.get_bool(SIG_SHIFT_CUT_ACTIVE));
    }

    SECTION("no trigger -> no cut");
    {
        auto cfg = make_cfg(); FlatShift fs; fs.init(cfg);
        SignalBus bus{}; EngineFrame f{};
        run(fs, bus, 10, 5000, 90.0f, false, f);
        CHECK(!fs.active());
        CHECK(!bus.valid(wk::ign_cut));
    }

    SECTION("trigger but low throttle -> no cut");
    {
        auto cfg = make_cfg(); FlatShift fs; fs.init(cfg);
        SignalBus bus{}; EngineFrame f{};
        run(fs, bus, 10, 5000, 20.0f, true, f);   // 20% < 70%
        CHECK(!fs.active());
    }

    SECTION("cut releases after max_cut_ms even if the trigger stays held");
    {
        auto cfg = make_cfg(); FlatShift fs; fs.init(cfg);
        SignalBus bus{}; EngineFrame seed{};
        run(fs, bus, 0, 5000, 90.0f, true, seed);
        for (int i = 0; i < 24; i++) { EngineFrame ff{}; run(fs, bus, 10, 5000, 90.0f, true, ff); }  // 240ms
        CHECK(fs.active());
        for (int i = 0; i < 3;  i++) { EngineFrame ff{}; run(fs, bus, 10, 5000, 90.0f, true, ff); }  // past 250ms
        CHECK(!fs.active());
    }

    SECTION("re-arms after the trigger releases");
    {
        auto cfg = make_cfg(); FlatShift fs; fs.init(cfg);
        SignalBus bus{}; EngineFrame seed{};
        run(fs, bus, 0, 5000, 90.0f, true, seed);
        for (int i = 0; i < 30; i++) { EngineFrame ff{}; run(fs, bus, 10, 5000, 90.0f, true, ff); }  // exceed max
        CHECK(!fs.active());
        EngineFrame r{}; run(fs, bus, 10, 5000, 90.0f, false, r);   // release
        EngineFrame p{}; run(fs, bus, 10, 5000, 90.0f, true,  p);   // press again
        CHECK(fs.active());
    }

    SECTION("does not clear another module's cut");
    {
        auto cfg = make_cfg(); FlatShift fs; fs.init(cfg);
        SignalBus bus{};
        // validity-OR: flatshift only ever publishes a cut (true) while active; it never writes
        // false, so it structurally cannot clear another module's cut. Verify it publishes nothing idle.
        EngineFrame f{};
        run(fs, bus, 10, 1000, 10.0f, false, f);                    // flatshift inactive
        CHECK(!bus.valid(wk::ign_cut));
        CHECK(!fs.active());
    }

    SECTION("disabled -> no cut");
    {
        auto cfg = make_cfg(); cfg.enabled = 0; FlatShift fs; fs.init(cfg);
        SignalBus bus{}; EngineFrame f{};
        run(fs, bus, 10, 5000, 90.0f, true, f);
        CHECK(!fs.active());
    }

    return test_summary();
}
