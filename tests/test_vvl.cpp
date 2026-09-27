// Host test for Vvl — VTEC-style high-lift cam engagement with RPM hysteresis.
#include "test_helpers.h"
#include "../firmware/Engine/Modules/Vvl.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "well_known_signals.h"
#include "../generated/signal_ids.h"

static VVLConfig make_cfg() {
    VVLConfig c{};
    c.enabled=1; c.on_rpm=4500; c.off_rpm=4200; c.min_load_kpa=400; c.min_clt_c=400; // 40kPa, 40C
    return c;
}
static bool step(Vvl& v, SignalBus& b, float rpm, float map, float clt) {
    EnginePosition p{}; p.rpm=rpm; EngineFrame f{};
    b.set(wk::map,map); b.set(wk::clt,clt);
    v.update(p,b,f); return b.get_bool(SIG_VVL_ACTIVE);
}
int main() {
    fprintf(stdout, "=== Vvl ===\n");
    SECTION("engages above on_rpm when warm + loaded");
    { auto c=make_cfg(); Vvl v; v.init(c); SignalBus b{};
      CHECK(!step(v,b,4400,80,90)); CHECK(step(v,b,4600,80,90)); CHECK(v.engaged()); }
    SECTION("hysteresis: holds between off_rpm and on_rpm");
    { auto c=make_cfg(); Vvl v; v.init(c); SignalBus b{};
      step(v,b,4600,80,90);                 // engage
      CHECK(step(v,b,4300,80,90));          // 4300 < on but > off -> still engaged
      CHECK(!step(v,b,4100,80,90)); }       // below off -> drop
    SECTION("cold engine stays on low cam");
    { auto c=make_cfg(); Vvl v; v.init(c); SignalBus b{};
      CHECK(!step(v,b,5000,80,20)); }
    SECTION("low load stays on low cam");
    { auto c=make_cfg(); Vvl v; v.init(c); SignalBus b{};
      CHECK(!step(v,b,5000,30,90)); }
    SECTION("disabled -> never engages");
    { auto c=make_cfg(); c.enabled=0; Vvl v; v.init(c); SignalBus b{};
      CHECK(!step(v,b,6000,100,90)); }
    return test_summary();
}
