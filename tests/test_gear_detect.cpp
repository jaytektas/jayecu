// Host test for GearDetect — derive gear from the rpm/speed ratio.
#include "test_helpers.h"
#include "../firmware/Engine/Modules/GearDetect.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "well_known_signals.h"
#include "../generated/signal_ids.h"

static GearDetectConfig make_cfg() {
    GearDetectConfig c{};
    c.enabled    = 1;
    c.gear_count = 5;
    c.tol_pct    = 12;
    c.min_rpm    = 500;
    c.min_vss    = 3;
    const uint16_t r[5] = {1000, 600, 400, 300, 240};   // rpm/kph x10: 100, 60, 40, 30, 24
    for (int i = 0; i < 5; i++) c.gear_ratio[i].rpm_per_kph = r[i];
    return c;
}

static uint8_t detect(GearDetect& g, SignalBus& bus, float rpm, float vss) {
    EnginePosition p{}; p.rpm = rpm; EngineFrame f{};
    bus.set(wk::vehicle_spd, vss);
    g.update(p, bus, f);
    return g.gear();
}

int main() {
    fprintf(stdout, "=== GearDetect ===\n");

    SECTION("matches each gear from its ratio (measured = rpm/vss)");
    {
        auto cfg = make_cfg(); GearDetect g; g.init(cfg);
        SignalBus bus{};
        CHECK(detect(g, bus, 3000, 30.0f)  == 1);   // 100 rpm/kph
        CHECK(detect(g, bus, 3000, 50.0f)  == 2);   // 60
        CHECK(detect(g, bus, 3000, 75.0f)  == 3);   // 40
        CHECK(detect(g, bus, 3000, 100.0f) == 4);   // 30
        CHECK(detect(g, bus, 3000, 125.0f) == 5);   // 24
    }

    SECTION("ratio outside tolerance -> 0 (unknown)");
    {
        auto cfg = make_cfg(); GearDetect g; g.init(cfg);
        SignalBus bus{};
        CHECK(detect(g, bus, 3000, 60.0f) == 0);   // measured 50: >12% from both gear2(60) and gear3(40)
    }

    SECTION("below speed floor -> 0");
    {
        auto cfg = make_cfg(); GearDetect g; g.init(cfg);
        SignalBus bus{};
        CHECK(detect(g, bus, 3000, 1.0f) == 0);   // vss 1 < 3
    }

    SECTION("below rpm floor -> 0");
    {
        auto cfg = make_cfg(); GearDetect g; g.init(cfg);
        SignalBus bus{};
        CHECK(detect(g, bus, 300, 50.0f) == 0);   // rpm 300 < 500
    }

    SECTION("publishes SIG_GEAR");
    {
        auto cfg = make_cfg(); GearDetect g; g.init(cfg);
        SignalBus bus{};
        detect(g, bus, 3000, 100.0f);
        CHECK_NEAR(bus.get(SIG_GEAR, -1.0f), 4.0f, 0.01f);
    }

    SECTION("disabled -> does not derive");
    {
        auto cfg = make_cfg(); cfg.enabled = 0; GearDetect g; g.init(cfg);
        SignalBus bus{};
        CHECK(detect(g, bus, 3000, 30.0f) == 0);
    }

    return test_summary();
}
