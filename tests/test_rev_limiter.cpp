#include "test_helpers.h"
#include "../firmware/Engine/Modules/RevLimiter.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "well_known_signals.h"   // wk:: cut/prot signal roles

static RevLimiterConfig make_cfg(bool enabled = true,
                                  uint16_t hard = 7000,
                                  uint16_t soft = 6500,
                                  uint8_t  method = 0,
                                  uint16_t resume = 150)
{
    RevLimiterConfig c{};
    c.enabled        = enabled ? 1 : 0;
    c.hard_limit_rpm = hard;
    c.soft_limit_rpm = soft;
    c.cut_method     = method;
    c.resume_band_rpm = resume;
    return c;
}

static EnginePosition make_pos(float rpm) {
    EnginePosition p{};
    p.rpm = rpm;
    return p;
}

int main() {
    fprintf(stdout, "=== RevLimiter ===\n");

    SECTION("disabled — no cut at any RPM");
    {
        auto cfg = make_cfg(false);
        RevLimiter rl;
        rl.init(cfg);
        EngineFrame frame{};
        SignalBus bus{};
        rl.update(make_pos(10000.0f), bus, frame);
        CHECK(!bus.valid(wk::fuel_cut));
        CHECK(!bus.valid(wk::ign_cut));
        CHECK(!bus.valid(SIG_FUEL_CUT));
        CHECK(!bus.valid(SIG_IGN_CUT));
    }

    SECTION("below soft limit — no cut");
    {
        auto cfg = make_cfg();
        RevLimiter rl;
        rl.init(cfg);
        EngineFrame frame{};
        SignalBus bus{};
        rl.update(make_pos(5000.0f), bus, frame);
        CHECK(!bus.valid(wk::fuel_cut));
        CHECK(!bus.valid(wk::ign_cut));
        CHECK(bus.get(SIG_SOFT_CUT_PCT) == 0.0f);
    }

    SECTION("at hard limit — hard fuel cut");
    {
        auto cfg = make_cfg();
        RevLimiter rl;
        rl.init(cfg);
        EngineFrame frame{};
        SignalBus bus{};
        rl.update(make_pos(7000.0f), bus, frame);
        CHECK(bus.valid(wk::fuel_cut));
        CHECK(!bus.valid(wk::ign_cut));
        CHECK(bus.valid(SIG_FUEL_CUT));
        CHECK(!bus.valid(SIG_IGN_CUT));
        CHECK(bus.get(SIG_SOFT_CUT_PCT) == 100.0f);
    }

    SECTION("ignition-only cut method");
    {
        auto cfg = make_cfg(true, 7000, 6500, 1);
        RevLimiter rl;
        rl.init(cfg);
        EngineFrame frame{};
        SignalBus bus{};
        rl.update(make_pos(7000.0f), bus, frame);
        CHECK(!bus.valid(wk::fuel_cut));
        CHECK(bus.valid(wk::ign_cut));
        CHECK(bus.valid(SIG_IGN_CUT));
    }

    SECTION("both-cut method");
    {
        auto cfg = make_cfg(true, 7000, 6500, 2);
        RevLimiter rl;
        rl.init(cfg);
        EngineFrame frame{};
        SignalBus bus{};
        rl.update(make_pos(7000.0f), bus, frame);
        CHECK(bus.valid(wk::fuel_cut));
        CHECK(bus.valid(wk::ign_cut));
    }

    SECTION("hysteresis — cut stays active until resume threshold");
    {
        auto cfg = make_cfg(true, 7000, 6500, 0, 150);
        RevLimiter rl;
        rl.init(cfg);
        SignalBus bus{};

        {
            EngineFrame frame{};
            rl.update(make_pos(7100.0f), bus, frame);
            CHECK(bus.valid(wk::fuel_cut));
        }

        {
            EngineFrame frame{};
            rl.update(make_pos(6900.0f), bus, frame);
            CHECK(bus.valid(wk::fuel_cut));
        }

        {
            EngineFrame frame{};
            bus.invalidate(wk::fuel_cut); bus.invalidate(wk::ign_cut);
            rl.update(make_pos(5000.0f), bus, frame);
            CHECK(!bus.valid(wk::fuel_cut));
        }
    }

    SECTION("soft cut in band — partial duty");
    {
        auto cfg = make_cfg();
        RevLimiter rl;
        rl.init(cfg);
        EngineFrame frame{};
        SignalBus bus{};
        rl.update(make_pos(6750.0f), bus, frame);
        const float pct = bus.get(SIG_SOFT_CUT_PCT);
        CHECK(pct > 0.0f && pct < 100.0f);
    }
    // (rpm is the decoder's channel now — published/rounded by EngineTask, not RevLimiter)

    SECTION("on_engine_stop clears state");
    {
        auto cfg = make_cfg();
        RevLimiter rl;
        rl.init(cfg);
        SignalBus bus{};

        {
            EngineFrame frame{};
            rl.update(make_pos(8000.0f), bus, frame);
            CHECK(bus.valid(wk::fuel_cut));
        }

        rl.on_engine_stop();   // clears hysteresis; cut outputs re-published on next update

        bus.invalidate(SIG_FUEL_CUT); bus.invalidate(SIG_IGN_CUT);
        EngineFrame frame2{};
        rl.update(make_pos(0.0f), bus, frame2);
        CHECK(!bus.valid(SIG_FUEL_CUT));
        CHECK(!bus.valid(SIG_FUEL_CUT));
        CHECK(bus.get(SIG_SOFT_CUT_PCT) == 0.0f);
    }

    return test_summary();
}
