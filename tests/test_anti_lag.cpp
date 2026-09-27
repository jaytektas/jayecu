// Host test for AntiLag — off-throttle ignition retard (ALS), time-limited.
#include "test_helpers.h"
#include "../firmware/Engine/Modules/AntiLag.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "well_known_signals.h"
#include "../generated/signal_ids.h"

static uint32_t g_ms = 1000;
extern "C" uint32_t platform_get_tick_ms() { return g_ms; }

static AntiLagConfig make_cfg() {
    AntiLagConfig c{};
    c.enabled=1; c.arm_sig=-1; c.min_rpm=3500; c.max_tps=100;  // <10% = off-throttle
    c.retard_deg=250; c.max_time_ms=5000;   // 25.0 deg, 5 s
    return c;
}
static float retard(AntiLag& a, SignalBus& bus, uint32_t dt, float rpm, float tps) {
    g_ms += dt; EnginePosition p{}; p.rpm=rpm; EngineFrame f{}; bus.set(wk::tps,tps);
    a.update(p,bus,f); return bus.get(SIG_ANTILAG_RETARD,-1.0f);
}
int main() {
    fprintf(stdout, "=== AntiLag ===\n");
    SECTION("off-throttle + high rpm -> retard");
    { auto c=make_cfg(); AntiLag a; a.init(c); SignalBus b{};
      retard(a,b,10,5000,2.0f); CHECK_NEAR(retard(a,b,10,5000,2.0f), 25.0f, 0.05f); CHECK(a.active()); }
    SECTION("throttle open -> no retard");
    { auto c=make_cfg(); AntiLag a; a.init(c); SignalBus b{};
      CHECK_NEAR(retard(a,b,10,5000,50.0f), 0.0f, 0.05f); CHECK(!a.active()); }
    SECTION("below rpm floor -> no retard");
    { auto c=make_cfg(); AntiLag a; a.init(c); SignalBus b{};
      CHECK_NEAR(retard(a,b,10,2000,2.0f), 0.0f, 0.05f); }
    SECTION("releases after max_time_ms");
    { auto c=make_cfg(); AntiLag a; a.init(c); SignalBus b{};
      retard(a,b,0,5000,2.0f);
      for(int i=0;i<499;i++) retard(a,b,10,5000,2.0f);   // ~4990ms < 5000 -> active
      CHECK(a.active());
      for(int i=0;i<3;i++) retard(a,b,10,5000,2.0f);      // past 5000ms
      CHECK(!a.active()); }
    SECTION("disabled -> no retard");
    { auto c=make_cfg(); c.enabled=0; AntiLag a; a.init(c); SignalBus b{};
      CHECK_NEAR(retard(a,b,10,5000,2.0f), 0.0f, 0.05f); }
    return test_summary();
}
