// Host test for Wmi — water/methanol injection duty ramp.
#include "test_helpers.h"
#include "../firmware/Engine/Modules/Wmi.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "well_known_signals.h"
#include "../generated/signal_ids.h"

static WmiConfig make_cfg() {
    WmiConfig c{};
    c.enabled=1; c.min_map_kpa=1200; c.full_map_kpa=2000; c.min_tps=600; c.min_duty_pct=200;  // 120..200kPa, 20%..100%
    return c;
}
static float run(Wmi& w, SignalBus& bus, float map, float tps) {
    EnginePosition p{}; EngineFrame f{}; bus.set(wk::map,map); bus.set(wk::tps,tps);
    w.update(p,bus,f); return w.duty();
}
int main() {
    fprintf(stdout, "=== Wmi ===\n");
    SECTION("below boost threshold -> off");
    { auto c=make_cfg(); Wmi w; w.init(c); SignalBus b{};
      CHECK_NEAR(run(w,b,100.0f,90.0f), 0.0f, 0.1f); CHECK(!b.get_bool(SIG_WMI_ACTIVE)); }
    SECTION("at onset -> min duty");
    { auto c=make_cfg(); Wmi w; w.init(c); SignalBus b{};
      CHECK_NEAR(run(w,b,120.0f,90.0f), 20.0f, 0.5f); CHECK(b.get_bool(SIG_WMI_ACTIVE)); }  // frac 0 -> 20%
    SECTION("midway -> ramped");
    { auto c=make_cfg(); Wmi w; w.init(c); SignalBus b{};
      CHECK_NEAR(run(w,b,160.0f,90.0f), 60.0f, 0.5f); }  // frac 0.5 -> 20 + 0.5*80 = 60
    SECTION("at/above full -> 100%");
    { auto c=make_cfg(); Wmi w; w.init(c); SignalBus b{};
      CHECK_NEAR(run(w,b,200.0f,90.0f), 100.0f, 0.5f);
      CHECK_NEAR(run(w,b,250.0f,90.0f), 100.0f, 0.5f); }
    SECTION("part throttle -> off");
    { auto c=make_cfg(); Wmi w; w.init(c); SignalBus b{};
      CHECK_NEAR(run(w,b,180.0f,30.0f), 0.0f, 0.1f); }
    SECTION("disabled -> off");
    { auto c=make_cfg(); c.enabled=0; Wmi w; w.init(c); SignalBus b{};
      CHECK_NEAR(run(w,b,180.0f,90.0f), 0.0f, 0.1f); }
    return test_summary();
}
