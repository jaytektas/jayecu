// Host test for Nitrous — single-stage activation window + solenoid/retard outputs.
#include "test_helpers.h"
#include "../firmware/Engine/Modules/Nitrous.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "well_known_signals.h"
#include "../generated/signal_ids.h"

static NitrousConfig make_cfg() {
    NitrousConfig c{};
    c.enabled=1; c.arm_sig=-1; c.min_rpm=3000; c.max_rpm=7000; c.min_tps=800;  // 80%
    c.pressure_src=-1; c.min_pressure_kpa=5500; c.retard_deg=30;   // whole kPa: ~800 psi   // 3.0 deg
    return c;
}
static bool run(Nitrous& n, SignalBus& bus, float rpm, float tps) {
    EnginePosition p{}; p.rpm=rpm; EngineFrame f{}; bus.set(wk::tps,tps);
    n.update(p,bus,f); return n.active();
}
int main() {
    fprintf(stdout, "=== Nitrous ===\n");
    SECTION("armed + WOT + in RPM window -> active + retard");
    { auto c=make_cfg(); Nitrous n; n.init(c); SignalBus b{};
      CHECK(run(n,b,5000,90.0f)==true);
      CHECK(b.get_bool(SIG_NITROUS_ACTIVE));
      CHECK_NEAR(b.get(SIG_NITROUS_RETARD,-1.0f), 3.0f, 0.05f); }
    SECTION("part throttle -> inactive");
    { auto c=make_cfg(); Nitrous n; n.init(c); SignalBus b{};
      CHECK(run(n,b,5000,50.0f)==false);
      CHECK_NEAR(b.get(SIG_NITROUS_RETARD,-1.0f), 0.0f, 0.05f); }
    SECTION("below min rpm -> inactive");
    { auto c=make_cfg(); Nitrous n; n.init(c); SignalBus b{};
      CHECK(run(n,b,2500,90.0f)==false); }
    SECTION("above max rpm -> inactive");
    { auto c=make_cfg(); Nitrous n; n.init(c); SignalBus b{};
      CHECK(run(n,b,7500,90.0f)==false); }
    SECTION("low bottle pressure -> inactive (safety)");
    { auto c=make_cfg(); c.pressure_src=SIG_NITROUS_PRESSURE_1; Nitrous n; n.init(c); SignalBus b{};
      b.set(SIG_NITROUS_PRESSURE_1, 4800.0f);  // ~700 psi, below the 5500 kPa minimum
      CHECK(run(n,b,5000,90.0f)==false);
      b.set(SIG_NITROUS_PRESSURE_1, 6500.0f);  // ~940 psi: a bottle at working pressure
      CHECK(run(n,b,5000,90.0f)==true); }
    SECTION("arm signal gates");
    { auto c=make_cfg(); c.arm_sig=SIG_CLUTCH_SW; Nitrous n; n.init(c); SignalBus b{};
      b.set_bool(SIG_CLUTCH_SW,false); CHECK(run(n,b,5000,90.0f)==false);
      b.set_bool(SIG_CLUTCH_SW,true);  CHECK(run(n,b,5000,90.0f)==true); }
    SECTION("disabled -> inactive");
    { auto c=make_cfg(); c.enabled=0; Nitrous n; n.init(c); SignalBus b{};
      CHECK(run(n,b,5000,90.0f)==false); }
    return test_summary();
}
