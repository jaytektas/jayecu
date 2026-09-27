// Host test for Dfco — deceleration (overrun) fuel cut with RPM hysteresis.
#include "test_helpers.h"
#include "../firmware/Engine/Modules/Dfco.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "well_known_signals.h"
#include "../generated/signal_ids.h"

static DfcoConfig make_cfg() {
    DfcoConfig c{};
    c.enabled  = 1;
    c.max_tps  = 20;    // 2.0% (scale 0.1) — "closed throttle"
    c.min_clt  = 60;    // degC — warm
    c.rpm_high = 1600;  // enter cut above
    c.rpm_low  = 1200;  // resume below
    return c;
}

// Run one frame; returns the accumulated bus.valid(wk::fuel_cut). prior_cut seeds a cut from another module.
static bool run(Dfco& d, SignalBus& bus, float rpm, float tps, float clt, bool prior_cut = false) {
    EnginePosition p{}; p.rpm = rpm;
    if (prior_cut) bus.set_bool(wk::fuel_cut, true);
    EngineFrame f{};
    bus.set(wk::tps, tps); bus.set(wk::clt, clt);
    d.update(p, bus, f);
    return bus.valid(wk::fuel_cut);
}

int main() {
    fprintf(stdout, "=== Dfco ===\n");

    SECTION("overrun (closed throttle, warm, high rpm) -> fuel cut");
    {
        auto cfg = make_cfg(); Dfco d; d.init(cfg);
        SignalBus bus{};
        CHECK(run(d, bus, 2000, 0.0f, 80.0f) == true);   // 2000>1600, tps 0<2%, clt 80>60
        CHECK(d.active());
        CHECK(bus.get_bool(SIG_DFCO_ACTIVE) == true);
    }

    SECTION("throttle open -> no cut");
    {
        auto cfg = make_cfg(); Dfco d; d.init(cfg);
        SignalBus bus{};
        CHECK(run(d, bus, 2000, 30.0f, 80.0f) == false);  // tps 30% >> 2%
        CHECK(!d.active());
    }

    SECTION("cold engine -> no cut");
    {
        auto cfg = make_cfg(); Dfco d; d.init(cfg);
        SignalBus bus{};
        CHECK(run(d, bus, 2000, 0.0f, 20.0f) == false);   // clt 20 < 60
    }

    SECTION("RPM hysteresis: enter above high, hold in band, resume below low");
    {
        auto cfg = make_cfg(); Dfco d; d.init(cfg);
        SignalBus bus{};
        run(d, bus, 2000, 0.0f, 80.0f); CHECK(d.active());   // enter (>1600)
        run(d, bus, 1400, 0.0f, 80.0f); CHECK(d.active());   // 1400 in [1200,1600] -> hold
        run(d, bus, 1100, 0.0f, 80.0f); CHECK(!d.active());  // <1200 -> resume
        run(d, bus, 1500, 0.0f, 80.0f); CHECK(!d.active());  // in band but was off -> stays off
    }

    SECTION("ORs into the cut — never clears another module's cut");
    {
        auto cfg = make_cfg(); Dfco d; d.init(cfg);
        SignalBus bus{};
        // DFCO inactive (throttle open) but a prior module already cut -> stays cut
        CHECK(run(d, bus, 2000, 50.0f, 80.0f, /*prior_cut=*/true) == true);
        CHECK(!d.active());
    }

    SECTION("disabled -> never cuts");
    {
        auto cfg = make_cfg(); cfg.enabled = 0; Dfco d; d.init(cfg);
        SignalBus bus{};
        CHECK(run(d, bus, 2000, 0.0f, 80.0f) == false);
        CHECK(!d.active());
    }

    return test_summary();
}
