// Host test for EgtProtect — two-stage EGT over-temp: enrichment ramp then hard cut with hysteresis.
#include "test_helpers.h"
#include "../firmware/Engine/Modules/EgtProtect.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "well_known_signals.h"
#include "../generated/signal_ids.h"
#include "../generated/module_dtc.h"
#include "../firmware/Diagnostics/DtcManager.h"

static EgtProtectConfig make_cfg() {
    EgtProtectConfig c{};
    c.enabled=1; c.enrich_c=850; c.cut_c=950; c.max_enrich_pct=20;
    return c;
}
struct R { float mult; bool cut; };
static R step(EgtProtect& e, SignalBus& b, float egt) {
    EnginePosition p{}; EngineFrame f{}; b.set(SIG_EGT_1,egt);
    b.invalidate(wk::fuel_cut);
    e.update(p,b,f);
    return { b.get(SIG_FUEL_CORR_EGT,-1.0f), b.valid(wk::fuel_cut) };
}
int main() {
    fprintf(stdout, "=== EgtProtect ===\n");
    SECTION("below enrich threshold -> neutral, no cut");
    { auto c=make_cfg(); EgtProtect e; e.init(c); SignalBus b{};
      R r=step(e,b,800); CHECK_NEAR(r.mult,1.0f,0.001f); CHECK(!r.cut); }
    SECTION("midway -> half enrichment");
    { auto c=make_cfg(); EgtProtect e; e.init(c); SignalBus b{};
      R r=step(e,b,900); CHECK_NEAR(r.mult,1.10f,0.001f); CHECK(!r.cut); }  // 50% of 20% = +10%
    SECTION("at cut -> full enrichment + cut");
    { auto c=make_cfg(); EgtProtect e; e.init(c); SignalBus b{};
      R r=step(e,b,950); CHECK_NEAR(r.mult,1.20f,0.001f); CHECK(r.cut); CHECK(e.cutting()); }
    SECTION("cut latches until below enrich_c");
    { auto c=make_cfg(); EgtProtect e; e.init(c); SignalBus b{};
      step(e,b,960);                         // cut
      CHECK(step(e,b,900).cut);              // still above enrich -> latched
      CHECK(!step(e,b,840).cut); }           // below enrich -> release
    // WHICH probe cooked. The protection acts on the hottest and does not care which; the person holding
    // the spanner does, and a stored code is what they have when nothing was being logged.
    SECTION("the cut raises the code for the HOTTEST probe, not probe 1");
    { auto c=make_cfg(); EgtProtect e; e.init(c); DtcManager d; e.set_dtc(&d); d.set_active(true);
      SignalBus b{}; EnginePosition p{}; EngineFrame f{};
      b.set(SIG_EGT_1, 400.0f); b.set(SIG_EGT_7, 1000.0f);      // probe 7 is the hot one
      e.update(p,b,f);
      CHECK(e.cutting());
      CHECK(d.code_severity(ModuleDtc::EGT_OVERTEMP_7) != 0);
      CHECK(d.code_severity(ModuleDtc::EGT_OVERTEMP_1) == 0); }
    SECTION("release heals that code, and it stays in history");
    { auto c=make_cfg(); EgtProtect e; e.init(c); DtcManager d; e.set_dtc(&d); d.set_active(true);
      SignalBus b{}; EnginePosition p{}; EngineFrame f{};
      b.set(SIG_EGT_3, 1000.0f); e.update(p,b,f);
      CHECK(d.code_severity(ModuleDtc::EGT_OVERTEMP_3) != 0);
      b.set(SIG_EGT_3, 400.0f);  e.update(p,b,f);               // below enrich -> release
      CHECK(!e.cutting());
      CHECK(d.code_severity(ModuleDtc::EGT_OVERTEMP_3) == 0);   // no longer ACTIVE…
      CHECK(d.clear(ModuleDtc::EGT_OVERTEMP_3)); }               // …but still STORED (clear finds it)
    SECTION("disabled -> neutral, no cut");
    { auto c=make_cfg(); c.enabled=0; EgtProtect e; e.init(c); SignalBus b{};
      R r=step(e,b,1200); CHECK_NEAR(r.mult,1.0f,0.001f); CHECK(!r.cut); }
    return test_summary();
}
