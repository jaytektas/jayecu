// Host test for VehicleSpeed — the pickups are frequencies; a road speed is what this makes of them.
//
// The bug this module exists for: `vehicle_spd` used to be the raw Hz off the pickup, while launch
// control, the pit limiter, cruise and gear detection all compare it against thresholds named in kph.
// Nothing converted it, so all four were reading Hz as though it were km/h. So the first thing worth
// asserting is the arithmetic — pulses/km is the whole calibration — and the second is that an
// uncalibrated or implausible source publishes NOTHING rather than a confident wrong number.
#include "test_helpers.h"
#include "../firmware/Engine/Modules/VehicleSpeed.h"
#include "../firmware/Signal/EnginePosition.h"
#include "../firmware/Signal/SignalBus.h"
#include "../firmware/Engine/EngineFrame.h"
#include "well_known_signals.h"
#include "../generated/signal_ids.h"

static VehicleSpeedConfig make_cfg() {
    VehicleSpeedConfig c{};
    c.enabled       = 1;
    c.main_source   = VehicleSpeed::Shaft;
    c.max_kph       = 4000;         // 400.0 kph (x0.1)
    for (auto& s : c.source) s.pulses_per_km = 2450;   // default driveshaft figure
    return c;
}

// One pass with the four wheels, the shaft and the GPS module set to the given frequencies.
static void run(VehicleSpeed& v, SignalBus& bus, float shaft, float fl, float fr, float rl, float rr,
                float gps = 0.0f) {
    EnginePosition p{}; EngineFrame f{};
    bus.set(SIG_SHAFT_HZ, shaft);
    bus.set(SIG_WHEEL_HZ_FL, fl); bus.set(SIG_WHEEL_HZ_FR, fr);
    bus.set(SIG_WHEEL_HZ_RL, rl); bus.set(SIG_WHEEL_HZ_RR, rr);
    bus.set(SIG_GPS_HZ, gps);
    v.update(p, bus, f);
}

int main() {
    fprintf(stdout, "=== VehicleSpeed ===\n");

    // 2450 pulses/km: one pulse per 0.408 m, so 40.83 Hz is 60 km/h — the calibration speed.
    const float kHzAt60 = 60.0f * 2450.0f / 3600.0f;

    SECTION("pulses/km turns a frequency into a road speed");
    {
        auto cfg = make_cfg(); VehicleSpeed v; v.init(cfg);
        SignalBus bus{};
        run(v, bus, kHzAt60, 0, 0, 0, 0);
        CHECK_NEAR(bus.get(SIG_VEHICLE_SPD, 0.0f), 60.0f, 0.01f);
        run(v, bus, kHzAt60 * 2.0f, 0, 0, 0, 0);
        CHECK_NEAR(bus.get(SIG_VEHICLE_SPD, 0.0f), 120.0f, 0.01f);
    }

    SECTION("an uncalibrated source publishes nothing at all");
    {
        auto cfg = make_cfg();
        for (auto& s : cfg.source) s.pulses_per_km = 0;      // never calibrated
        VehicleSpeed v; v.init(cfg);
        SignalBus bus{};
        bus.set(SIG_VEHICLE_SPD, 42.0f);                     // something else's answer
        run(v, bus, kHzAt60, 0, 0, 0, 0);
        CHECK_NEAR(bus.get(SIG_VEHICLE_SPD, 0.0f), 42.0f, 0.001f);   // left alone, not zeroed
    }

    SECTION("disabled, it leaves a speed from CAN or Lua alone");
    {
        auto cfg = make_cfg(); cfg.enabled = 0;
        VehicleSpeed v; v.init(cfg);
        SignalBus bus{};
        bus.set(SIG_VEHICLE_SPD, 88.0f);
        run(v, bus, kHzAt60, 0, 0, 0, 0);
        CHECK_NEAR(bus.get(SIG_VEHICLE_SPD, 0.0f), 88.0f, 0.001f);
    }

    SECTION("an implausible reading is dropped, not published");
    {
        auto cfg = make_cfg(); VehicleSpeed v; v.init(cfg);
        SignalBus bus{};
        run(v, bus, kHzAt60, 0, 0, 0, 0);
        CHECK_NEAR(bus.get(SIG_VEHICLE_SPD, 0.0f), 60.0f, 0.01f);
        run(v, bus, 500000.0f, 0, 0, 0, 0);                  // a floating input reads megahertz
        CHECK_NEAR(bus.get(SIG_VEHICLE_SPD, 0.0f), 60.0f, 0.01f);   // the last good answer stands
    }

    SECTION("each wheel is published in km/h, whatever the main source is");
    {
        // Red if the per-corner publish is ever made conditional on the source: traction control reads
        // these four directly and needs all of them, however the road speed is being derived.
        auto cfg = make_cfg(); VehicleSpeed v; v.init(cfg);
        SignalBus bus{};
        // fronts at 60, rears spinning at 80 — the shape of wheelspin.
        run(v, bus, kHzAt60, kHzAt60, kHzAt60, kHzAt60 * 80.f / 60.f, kHzAt60 * 80.f / 60.f);
        CHECK_NEAR(bus.get(SIG_WHEEL_FL, 0.0f), 60.0f, 0.01f);
        CHECK_NEAR(bus.get(SIG_WHEEL_FR, 0.0f), 60.0f, 0.01f);
        CHECK_NEAR(bus.get(SIG_WHEEL_RL, 0.0f), 80.0f, 0.01f);
        CHECK_NEAR(bus.get(SIG_WHEEL_RR, 0.0f), 80.0f, 0.01f);
    }

    SECTION("a source NAMES its wheels — front axle, rear axle or all four");
    {
        // The whole point of the list: no second setting decides which wheels "Rear Axle" meant.
        // Fronts 60 and 70, rears 80 and 90.
        struct Case { uint8_t src; float want; const char* why; };
        const Case cases[] = {
            { VehicleSpeed::FrontAxle, 65.0f, "middle of {60, 70}" },
            { VehicleSpeed::RearAxle,  85.0f, "middle of {80, 90}" },
            { VehicleSpeed::AllWheels, 75.0f, "middle two of {60, 70, 80, 90}" },
        };
        for (const Case& c : cases) {
            auto cfg = make_cfg(); cfg.main_source = c.src;
            VehicleSpeed v; v.init(cfg);
            SignalBus bus{};
            run(v, bus, 0, kHzAt60, kHzAt60 * 70.f / 60.f,
                        kHzAt60 * 80.f / 60.f, kHzAt60 * 90.f / 60.f);
            CHECK_NEAR(bus.get(SIG_VEHICLE_SPD, 0.0f), c.want, 0.01f);
        }
    }

    SECTION("the MIDDLE value: one wheel spinning does not move the road speed");
    {
        // Three wheels honest at 60, the right rear spinning at 120. The median is 60; a mean would be
        // 75 — fifteen km/h of wheelspin handed to cruise control as though the car had accelerated.
        // Red the moment median_ is replaced by an average.
        auto cfg = make_cfg(); cfg.main_source = VehicleSpeed::AllWheels;
        VehicleSpeed v; v.init(cfg);
        SignalBus bus{};
        run(v, bus, 0, kHzAt60, kHzAt60, kHzAt60, kHzAt60 * 120.f / 60.f);
        CHECK_NEAR(bus.get(SIG_VEHICLE_SPD, 0.0f), 60.0f, 0.01f);
    }

    SECTION("...and one wheel LOCKED does not either");
    {
        // The other end of the same list, and the reason "slowest" was never the answer: three wheels at
        // 60 and a locked front left reading 0. The median is 60; the slowest would be 0, and a mean 45.
        auto cfg = make_cfg(); cfg.main_source = VehicleSpeed::AllWheels;
        VehicleSpeed v; v.init(cfg);
        SignalBus bus{};
        run(v, bus, 0, 0.001f, kHzAt60, kHzAt60, kHzAt60);
        CHECK_NEAR(bus.get(SIG_VEHICLE_SPD, 0.0f), 60.0f, 0.01f);
    }

    SECTION("one wheel fitted is still an answer for its set");
    {
        auto cfg = make_cfg(); cfg.main_source = VehicleSpeed::RearAxle;
        cfg.source[VehicleSpeed::RR].pulses_per_km = 0;       // only one rear sensor on the car
        VehicleSpeed v; v.init(cfg);
        SignalBus bus{};
        run(v, bus, 0, kHzAt60, kHzAt60, kHzAt60 * 80.f / 60.f, 0);
        // 80, not 40: a wheel that is not reading is not in the sort at all. Red the moment a missing
        // corner is folded in as a zero.
        CHECK_NEAR(bus.get(SIG_VEHICLE_SPD, 0.0f), 80.0f, 0.01f);
    }

    SECTION("three wheels reading takes the middle ONE, not a mean of a pair");
    {
        auto cfg = make_cfg(); cfg.main_source = VehicleSpeed::AllWheels;
        cfg.source[VehicleSpeed::RR].pulses_per_km = 0;          // three wheels fitted
        VehicleSpeed v; v.init(cfg);
        SignalBus bus{};
        run(v, bus, 0, kHzAt60, kHzAt60 * 70.f / 60.f, kHzAt60 * 90.f / 60.f, 0);
        CHECK_NEAR(bus.get(SIG_VEHICLE_SPD, 0.0f), 70.0f, 0.01f);
    }

    SECTION("a set with nothing reading publishes NOTHING, not zero");
    {
        auto cfg = make_cfg(); cfg.main_source = VehicleSpeed::FrontAxle;
        cfg.source[VehicleSpeed::FL].pulses_per_km = 0;
        cfg.source[VehicleSpeed::FR].pulses_per_km = 0;          // rear-wheel pickups only
        VehicleSpeed v; v.init(cfg);
        SignalBus bus{};
        run(v, bus, kHzAt60, kHzAt60, kHzAt60, kHzAt60, kHzAt60);
        CHECK(!bus.valid(SIG_VEHICLE_SPD));
    }

    SECTION("GPS is a pickup like any other — a pulse train with its own pulses/km");
    {
        // Red if GPS is ever special-cased back into reading somebody else's kph channel: this GPS
        // module puts out HALF the pulses per km the shaft does, and only the per-source calibration
        // turns that into the same 60 kph. Nothing here publishes a speed, only a frequency.
        auto cfg = make_cfg(); cfg.main_source = VehicleSpeed::Gps;
        cfg.source[VehicleSpeed::Gps].pulses_per_km = 1225;
        VehicleSpeed v; v.init(cfg);
        SignalBus bus{};
        run(v, bus, kHzAt60, kHzAt60, kHzAt60, kHzAt60, kHzAt60, kHzAt60 / 2.0f);
        CHECK_NEAR(bus.get(SIG_VEHICLE_SPD, 0.0f), 60.0f, 0.01f);
    }

    SECTION("an uncalibrated GPS publishes nothing, exactly like an uncalibrated wheel");
    {
        auto cfg = make_cfg(); cfg.main_source = VehicleSpeed::Gps;
        cfg.source[VehicleSpeed::Gps].pulses_per_km = 0;
        VehicleSpeed v; v.init(cfg);
        SignalBus bus{};
        run(v, bus, kHzAt60, kHzAt60, kHzAt60, kHzAt60, kHzAt60, kHzAt60);
        CHECK(!bus.valid(SIG_VEHICLE_SPD));
    }

    fprintf(stdout, "=== VehicleSpeed: all good ===\n");
    return test_summary();
}
