// Host test for PitLimiter — vehicle-speed limiter with hysteresis.
#include "test_helpers.h"
#include "../firmware/Engine/Modules/PitLimiter.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "well_known_signals.h"
#include "../generated/signal_ids.h"

static PitLimiterConfig make_cfg() {
    PitLimiterConfig c{};
    c.enabled = 1; c.arm_sig = -1; c.speed_kph = 60; c.hyst_kph = 3; c.cut_method = 2;  // both
    return c;
}
static bool run(PitLimiter& p, SignalBus& bus, float vss, bool prior=false) {
    EnginePosition pos{}; if (prior) { bus.set_bool(wk::fuel_cut, true); bus.set_bool(wk::ign_cut, true); }
    EngineFrame f{};
    bus.set(wk::vehicle_spd, vss);
    p.update(pos, bus, f);
    return bus.valid(wk::fuel_cut) && bus.valid(wk::ign_cut);
}
int main() {
    fprintf(stdout, "=== PitLimiter ===\n");
    SECTION("over limit -> cut");
    { auto c=make_cfg(); PitLimiter p; p.init(c); SignalBus b{};
      CHECK(run(p,b,65.0f)==true); CHECK(p.active()); CHECK(b.get_bool(SIG_PIT_LIMIT_ACTIVE)); }
    SECTION("under limit -> no cut");
    { auto c=make_cfg(); PitLimiter p; p.init(c); SignalBus b{};
      CHECK(run(p,b,50.0f)==false); CHECK(!p.active()); }
    SECTION("hysteresis: holds cut in band, resumes below limit-hyst");
    { auto c=make_cfg(); PitLimiter p; p.init(c); SignalBus b{};
      run(p,b,65.0f); CHECK(p.active());
      run(p,b,58.0f); CHECK(p.active());   // in [57,60] band -> hold
      run(p,b,56.0f); CHECK(!p.active()); }// < 57 -> resume
    SECTION("arm signal gates engagement");
    { auto c=make_cfg(); c.arm_sig=SIG_CLUTCH_SW; PitLimiter p; p.init(c); SignalBus b{};
      b.set_bool(SIG_CLUTCH_SW,false); CHECK(run(p,b,65.0f)==false);   // not engaged
      b.set_bool(SIG_CLUTCH_SW,true);  CHECK(run(p,b,65.0f)==true); }  // engaged
    SECTION("disabled -> no cut");
    { auto c=make_cfg(); c.enabled=0; PitLimiter p; p.init(c); SignalBus b{};
      CHECK(run(p,b,65.0f)==false); }
    return test_summary();
}
