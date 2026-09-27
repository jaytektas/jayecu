// Host test for Stepper — bipolar stepper position controller. Reads a 0..100% demand signal,
// integrates to a microstep position (rate-limited), computes cos/sin coil duties, and publishes
// them as SIG_STEP_DEMAND_A/B + SIG_STEP_EN_A/B on the bus. No hardware required.
//   build: tests/CMakeLists.txt -> ctest -R stepper
#include "test_helpers.h"
#include "../firmware/Engine/Modules/Stepper.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "../generated/signal_ids.h"
#include <cmath>

extern "C" uint32_t g_stub_tick_ms;   // host-controllable ms clock (platform_hal_stub) — drives the step-rate limit

static EnginePosition POS{};
static EngineFrame    FR{};

// Advance the clock past any step interval, then update — so the step-rate limit (step_period_ms)
// never blocks a step the test expects to happen.
static void step_update(Stepper& s, SignalBus& bus) { g_stub_tick_ms += 50; s.update(POS, bus, FR); }

// Finish the power-up homing (drive to the closed stop from "fully open + margin"), so a section starts
// from a KNOWN position 0 — which is exactly what the firmware now guarantees before trusting its count.
static void home(Stepper& s, SignalBus& bus) {
    const float demand = bus.get(SIG_IDLE_DUTY, 0.0f);
    bus.set(SIG_IDLE_DUTY, 0.0f, true);                       // home against no demand...
    for (int i = 0; i < 100000 && s.homing(); ++i) step_update(s, bus);
    step_update(s, bus); step_update(s, bus);                 // ...settle (Step/Dir pulse back low)
    bus.set(SIG_IDLE_DUTY, demand, true);                     // then hand the demand back
}

static StepperConfig base_cfg() {
    StepperConfig c{};
    c.enabled           = 1;
    c.input_sig         = SIG_IDLE_DUTY;
    c.microstep         = 1;
    c.range_steps       = 100;
    c.max_step_per_update = 100;
    c.move_current_pct = 800;
    c.hold_current_pct = 300;
    return c;
}

int main() {
    fprintf(stdout, "=== Stepper ===\n");

    SECTION("enable signals are 1 (enabled) while module is enabled, 0 while disabled");
    {
        StepperConfig cfg = base_cfg();
        Stepper s; s.init(cfg);
        SignalBus bus{}; bus.set(SIG_IDLE_DUTY, 0.0f, true);
        s.update(POS, bus, FR);
        CHECK_NEAR(bus.get(SIG_STEP_EN_A, -1.0f), 1.0f, 0.01f);
        CHECK_NEAR(bus.get(SIG_STEP_EN_B, -1.0f), 1.0f, 0.01f);

        cfg.enabled = 0; s.on_config_change(cfg);
        s.update(POS, bus, FR);
        CHECK_NEAR(bus.get(SIG_STEP_EN_A, -1.0f), 0.0f, 0.01f);
        CHECK_NEAR(bus.get(SIG_STEP_EN_B, -1.0f), 0.0f, 0.01f);
    }

    SECTION("100% demand walks to range_steps at max_step_per_update rate");
    {
        StepperConfig cfg = base_cfg();
        cfg.range_steps = 4; cfg.max_step_per_update = 1;
        Stepper s; s.init(cfg);
        SignalBus bus{}; bus.set(SIG_IDLE_DUTY, 100.0f, true);
        home(s, bus);
        CHECK(s.step_position() == 0);
        for (int i = 0; i < 4; ++i) step_update(s, bus);
        CHECK(s.step_position() == 4);
        step_update(s, bus);
        CHECK(s.step_position() == 4);   // holds at target
    }

    SECTION("at position 0 (theta=0): coil A = cos(0)*current, coil B = sin(0)*current");
    {
        StepperConfig cfg = base_cfg();
        Stepper s; s.init(cfg);
        SignalBus bus{}; bus.set(SIG_IDLE_DUTY, 0.0f, true);
        home(s, bus);
        s.update(POS, bus, FR);   // stays at pos 0, held (demand=0 -> target=0)
        // hold current: at pos 0 and target 0 → not moving → hold
        float ca = bus.get(SIG_STEP_DEMAND_A, -999.0f);
        float cb = bus.get(SIG_STEP_DEMAND_B, -999.0f);
        CHECK_NEAR(ca, std::cos(0.0f) * 30.0f, 0.5f);   // cos(0)*hold = 30
        CHECK_NEAR(cb, std::sin(0.0f) * 30.0f, 0.5f);   // sin(0)*hold =  0
    }

    SECTION("move current used while in motion, hold current when at target");
    {
        StepperConfig cfg = base_cfg();
        cfg.range_steps = 4; cfg.max_step_per_update = 1;
        cfg.move_current_pct = 800; cfg.hold_current_pct = 300;
        Stepper s; s.init(cfg);
        SignalBus bus{}; bus.set(SIG_IDLE_DUTY, 100.0f, true);
        home(s, bus);

        step_update(s, bus);   // moving: pos 0->1
        float ca_move = std::fabs(bus.get(SIG_STEP_DEMAND_A, 0.0f));
        float cb_move = std::fabs(bus.get(SIG_STEP_DEMAND_B, 0.0f));
        // magnitude should be scaled by move_current (80), not hold (30)
        CHECK(ca_move > 30.0f || cb_move > 30.0f);

        for (int i = 0; i < 3; ++i) step_update(s, bus);
        step_update(s, bus);   // now at target (pos 4), holding
        float ca_hold = std::fabs(bus.get(SIG_STEP_DEMAND_A, 0.0f));
        float cb_hold = std::fabs(bus.get(SIG_STEP_DEMAND_B, 0.0f));
        CHECK(ca_hold <= 30.0f + 0.5f && cb_hold <= 30.0f + 0.5f);
    }

    SECTION("dir_invert reverses rotation (sin phase flips, cos unchanged)");
    {
        // 25% demand, range_steps=4, microstep=1: target=1. One update → pos=1.
        // er=4, phase=1, theta=π/2 → sin=+move_current (fwd); theta=-π/2 → sin=-move_current (rev).
        StepperConfig cfg_fwd = base_cfg();
        cfg_fwd.range_steps = 4; cfg_fwd.max_step_per_update = 4; cfg_fwd.microstep = 1;
        StepperConfig cfg_rev = cfg_fwd;
        cfg_rev.dir_invert = 1;

        Stepper fwd; fwd.init(cfg_fwd);
        Stepper rev; rev.init(cfg_rev);

        SignalBus bf{}, br{};
        bf.set(SIG_IDLE_DUTY, 25.0f, true);   // target = 25% * 4 = 1 step
        br.set(SIG_IDLE_DUTY, 25.0f, true);
        home(fwd, bf); home(rev, br);
        step_update(fwd, bf);
        step_update(rev, br);

        // Coil B (sin): sin(π/2) = +current, sin(-π/2) = -current — opposite signs.
        CHECK(bf.get(SIG_STEP_DEMAND_B, 0.0f) > 0.0f);
        CHECK(br.get(SIG_STEP_DEMAND_B, 0.0f) < 0.0f);
    }

    SECTION("power-up homes to the closed stop; an engine stop keeps the (true) position");
    {
        // A stop does not move the valve, so the count must not pretend it did — it used to zero it,
        // and every stop added the valve's opening to an untracked offset.
        StepperConfig cfg = base_cfg();
        cfg.range_steps = 100; cfg.max_step_per_update = 100;
        Stepper s; s.init(cfg);
        CHECK(s.homing());
        CHECK(s.step_position() > 100);                  // "fully open + margin" before homing
        SignalBus bus{}; bus.set(SIG_IDLE_DUTY, 100.0f, true);
        home(s, bus);
        CHECK(!s.homing());
        step_update(s, bus);
        CHECK(s.step_position() == 100);
        s.on_engine_stop();
        CHECK(s.step_position() == 100);                 // unchanged: the valve did not move
    }

    SECTION("Step/Direction mode: one STEP pulse per step, DIR/ENABLE levels, coils off");
    {
        StepperConfig cfg = base_cfg();
        cfg.driver_mode = 1;                 // Step/Direction
        cfg.range_steps = 10; cfg.step_period_ms = 5;
        Stepper s; s.init(cfg);
        SignalBus bus{}; bus.set(SIG_IDLE_DUTY, 100.0f, true);   // target = 10 steps
        home(s, bus);

        // Every step is an EDGE: the pulse is high for one service and low for at least the next, so
        // 10 steps take 20 services. (It used to sit high whenever a step was due every service.)
        int pulses = 0; bool prev = false, rose_twice = false;
        for (int i = 0; i < 20; ++i) {
            step_update(s, bus);
            CHECK_NEAR(bus.get(SIG_STEP_ENABLE, -1.0f), 1.0f, 0.01f);   // driver enabled
            const bool hi = bus.get(SIG_STEP_PULSE, 0.0f) > 0.5f;
            if (hi) { pulses++; CHECK_NEAR(bus.get(SIG_STEP_DIR, -1.0f), 1.0f, 0.01f); }   // forward
            if (hi && prev) rose_twice = true;
            prev = hi;
            CHECK_NEAR(bus.get(SIG_STEP_EN_A, 1.0f), 0.0f, 0.01f);     // H-bridge coils off in this mode
        }
        CHECK(s.step_position() == 10);      // reached target
        CHECK(pulses == 10);                 // exactly one pulse per step
        CHECK(!rose_twice);                  // never high two services running
    }

    SECTION("step_period_ms rate-limits stepping");
    {
        StepperConfig cfg = base_cfg();
        cfg.driver_mode = 1; cfg.range_steps = 100; cfg.step_period_ms = 20;   // 20 ms/step
        Stepper s; s.init(cfg);
        SignalBus bus{}; bus.set(SIG_IDLE_DUTY, 100.0f, true);
        home(s, bus);

        g_stub_tick_ms += 1000; s.update(POS, bus, FR);   // period elapsed → first step
        const int32_t p0 = s.step_position();
        g_stub_tick_ms += 5;    s.update(POS, bus, FR);   // < 20 ms later → must NOT step
        CHECK(s.step_position() == p0);
        g_stub_tick_ms += 20;   s.update(POS, bus, FR);   // period elapsed → steps
        CHECK(s.step_position() == p0 + 1);
    }

    return test_summary();
}
