#include "test_helpers.h"
#include "../firmware/Engine/Modules/Alternator.h"
#include "../firmware/Engine/EngineStateMachine.h"   // EngineRunState
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "../generated/signal_ids.h"

static uint32_t g_ms = 0;
extern "C" uint32_t platform_get_tick_ms() { return g_ms; }

static AlternatorConfig make_cfg(uint8_t soft_start_s = 0) {
    AlternatorConfig c{};
    c.enabled        = 1;
    c.target_voltage = 1440;          // 14.40 V (scale 0.01)
    c.min_rpm        = 400;
    c.kp             = 30000;         // 30 %/V
    c.ki             = 15000;         // 15 %/V/s
    c.max_duty_pct = 1000;
    c.off_above_tps = 0;             // always on
    c.soft_start_s   = soft_start_s;  // 0 = instant (no ramp) unless a test sets it
    return c;
}

static SignalBus make_bus(float vbat, float rpm, EngineRunState st, float tps = 0.0f) {
    SignalBus b{};
    b.set(SIG_BATTERY, vbat);
    b.set(SIG_RPM, rpm);
    b.set(SIG_ENGINE_STATE, static_cast<float>(st));
    b.set(SIG_TPS, tps);
    return b;
}

static EnginePosition make_pos(float rpm) { EnginePosition p{}; p.rpm = rpm; return p; }

// Step the loop n times at 10 ms cadence so the PI integrates; return the final published duty.
static float run(Alternator& a, float vbat, float rpm, EngineRunState st, int n, float tps = 0.0f) {
    EngineFrame f{};
    float duty = -1.0f;
    for (int i = 0; i < n; ++i) {
        g_ms += 10;
        SignalBus b = make_bus(vbat, rpm, st, tps);
        a.update(make_pos(rpm), b, f);
        duty = b.get(SIG_ALTERNATOR_DUTY, -1.0f);
    }
    return duty;
}

int main() {
    fprintf(stdout, "=== Alternator ===\n");

    SECTION("disabled — publishes nothing");
    {
        auto cfg = make_cfg(); cfg.enabled = 0;
        Alternator a; a.init(cfg);
        CHECK(run(a, 12.0f, 2000.0f, EngineRunState::RUNNING, 5) < 0.0f);   // default -> never published
    }

    SECTION("engine stopped — disengaged, no output");
    {
        auto cfg = make_cfg(); Alternator a; a.init(cfg);
        CHECK(run(a, 12.0f, 0.0f, EngineRunState::STOPPED, 5) < 0.0f);
    }

    SECTION("below min RPM — disengaged");
    {
        auto cfg = make_cfg(); Alternator a; a.init(cfg);
        CHECK(run(a, 12.0f, 200.0f, EngineRunState::RUNNING, 5) < 0.0f);   // 200 < min_rpm 400
    }

    SECTION("no battery reading — field released, NOT driven to full");
    {
        // Read as 0 V it was the whole target as error: full field into an unmeasured battery.
        auto cfg = make_cfg(); Alternator a; a.init(cfg);
        EngineFrame f{}; float duty = 0.0f;
        for (int i = 0; i < 20; ++i) {
            g_ms += 10;
            SignalBus b = make_bus(12.0f, 2000.0f, EngineRunState::RUNNING, 0.0f);
            b.invalidate(SIG_BATTERY);
            a.update(make_pos(2000.0f), b, f);
            duty = b.get(SIG_ALTERNATOR_DUTY, -1.0f);
        }
        CHECK(duty < 0.0f);                                   // nothing published -> output failsafes off
    }

    SECTION("well over target — field cut outright");
    {
        auto cfg = make_cfg(); Alternator a; a.init(cfg);
        run(a, 12.0f, 2000.0f, EngineRunState::RUNNING, 20);   // wind the field up
        CHECK_NEAR(run(a, 16.5f, 2000.0f, EngineRunState::RUNNING, 1), 0.0, 0.01);
    }

    SECTION("low battery — field drives up toward max");
    {
        auto cfg = make_cfg(); Alternator a; a.init(cfg);
        const float duty = run(a, 12.0f, 2000.0f, EngineRunState::RUNNING, 20);   // 2.4 V under target
        CHECK(duty > 50.0f);
    }

    SECTION("battery above target — field backs off to zero");
    {
        auto cfg = make_cfg(); Alternator a; a.init(cfg);
        const float duty = run(a, 15.0f, 2000.0f, EngineRunState::RUNNING, 20);   // 0.6 V over target
        CHECK(duty == 0.0f);
    }

    SECTION("load-shed above TPS — disengages");
    {
        auto cfg = make_cfg(); cfg.off_above_tps = 800;
        Alternator a; a.init(cfg);
        // First charge normally, then go to WOT: the field must drop out (publishes nothing).
        run(a, 12.0f, 3000.0f, EngineRunState::RUNNING, 10, 10.0f);
        CHECK(run(a, 12.0f, 3000.0f, EngineRunState::RUNNING, 3, 90.0f) < 0.0f);
    }

    SECTION("soft-start ramps the duty ceiling");
    {
        auto cfg = make_cfg(/*soft_start_s=*/50);   // 5.0 s ramp
        Alternator a; a.init(cfg);
        // ~1 s in (ramp ~0.2 -> ceiling ~20%): low battery wants max but is capped low.
        const float early = run(a, 11.0f, 2000.0f, EngineRunState::RUNNING, 100);
        // ~6 s total (ramp saturated at 1.0 -> ceiling 100%).
        const float late  = run(a, 11.0f, 2000.0f, EngineRunState::RUNNING, 500);
        CHECK(early < 60.0f);
        CHECK(late > early);
    }

    return test_summary();
}
