// The scheduler's ASYNC INJECTION TRAIN — the transient-enrichment burst.
//
// TransientThrottle's half of this is tested (it produces async_inj_pw_us/pulses on a tip-in edge);
// the half that actually drives injectors had no test at all, which is how a four-stroke assumption
// sat in it unnoticed. The burst is spread across ONE ENGINE CYCLE by counting virtual teeth, and
// "teeth per cycle" is not "teeth per revolution times two" unless the engine happens to be a
// four-stroke.
//
// This drives EventScheduler through its real ISR entry point (on_virtual_tooth) with mock hardware
// and watches which teeth actually fire injectors — so it tests delivery, not intent.
//
//   cmake --build build --target test_async_injection && ./build/test_async_injection
#include "test_helpers.h"
#include "Scheduler/EventScheduler.h"
#include "output_rows_helper.h"
#include "Scheduler/SchedulerTypes.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

// A tick source that advances only when we say so, so "when did this fire" is exact.
uint32_t g_now = 0;
constexpr uint32_t kTicksPerSecond = 1'000'000;   // 1 MHz, so 1 tick = 1 us

// Records every drive so a pulse is observed at the pin, not inferred from internal state.
struct MockChannel final : ITimerChannel {
    int opens = 0, closes = 0;
    [[nodiscard]] uint32_t get_current_ticks()    const noexcept override { return g_now; }
    [[nodiscard]] uint32_t get_ticks_per_second() const noexcept override { return kTicksPerSecond; }
    void enable_output(OutputAction) noexcept override {}
    void force_output_now(OutputAction a) noexcept override {
        if (a == OutputAction::DRIVE_HIGH) ++opens; else ++closes;
    }
};

struct MockAlarm final : IAlarmTimer {
    void arm(uint32_t) noexcept override {}
    void disarm() noexcept override {}
    [[nodiscard]] uint32_t now() const noexcept override { return g_now; }
    void register_callback(MatchCallback, void*) noexcept override {}
};

// Build a scheduler for a given engine cycle, then run `teeth` virtual teeth and report the tooth
// index of every async squirt.
std::vector<int> run_burst(uint8_t cycle_type, AngleDeg10 vt_step, uint8_t pulses, int teeth) {
    EngineConfig eng{};
    eng.cylinder_count = 4;
    eng.cycle_type     = cycle_type;
    eng.num_inj_stages = 1;
    // Primary stage: sequential, one injector row per cylinder (LS1..LS4). The async burst fires every
    // active injector regardless of mode.
    eng.inj_stage[0].mode                = static_cast<uint8_t>(InjectionMode::SEQUENTIAL);
    eng.inj_stage[0].injections_per_cycle = 1;
    for (int c = 0; c < 4; ++c) eng.firing_order[c].cyl = static_cast<uint8_t>(c + 1);

    static MockChannel inj[4];
    static MockAlarm   angle_alarm, time_alarm;
    for (auto& ch : inj) { ch.opens = 0; ch.closes = 0; }

    EventScheduler sched;
    sched.set_config(eng, layout_output_map(eng, TestCoils::COP), true);
    for (uint8_t c = 0; c < 4; ++c) sched.assign_inj_channel(c, inj[c]);
    sched.assign_angle_alarm(angle_alarm);
    sched.assign_time_alarm(time_alarm);
    sched.build_skeleton(vt_step);
    sched.set_firing_enabled(true);
    sched.set_phase_known(true);

    sched.request_async_injection(2000, pulses);   // 2 ms per squirt

    std::vector<int> fired;
    const uint16_t teeth_per_rev = static_cast<uint16_t>(ANGLE_360 / vt_step);
    for (int t = 0; t < teeth; ++t) {
        const int before = inj[0].opens;
        g_now += 1000;                                        // 1 ms per tooth; value is irrelevant
        sched.on_virtual_tooth(static_cast<uint16_t>(t % teeth_per_rev), g_now, 1000);
        if (inj[0].opens > before) fired.push_back(t);
    }
    return fired;
}

int fails = 0;
void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-64s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

} // namespace

int main() {
    std::puts("=== Async injection train (transient enrichment burst) ===");

    // 10 deg grid: 36 teeth per revolution. A four-stroke cycle is 72 teeth, a two-stroke 36,
    // a rotary 108 — which is exactly what the burst must spread over.
    constexpr AngleDeg10 kStep = 100;   // 10.0 deg

    std::puts("\n-- four-stroke: 3 pulses spread over 72 teeth (720 deg) --");
    {
        const auto f = run_burst(static_cast<uint8_t>(EngineCycleType::FOUR_STROKE), kStep, 3, 90);
        ck(f.size() == 3, "three squirts delivered", std::to_string(f.size()));
        if (f.size() == 3) {
            const int gap = f[1] - f[0];
            ck(gap == 24, "spaced 72/3 = 24 teeth apart", std::to_string(gap));
            ck(f[2] - f[1] == 24, "…evenly");
            ck(f.back() < 72, "and the whole burst lands inside ONE cycle",
               "last at tooth " + std::to_string(f.back()));
        }
    }

    std::puts("\n-- two-stroke: the cycle is 36 teeth, not 72 --");
    {
        // The bug: teeth_per_cycle was buckets_per_rev * 2 regardless of cycle type, so a two-stroke
        // got 72 and spread its burst over TWO cycles — fuel meant for this intake arriving during
        // the next one.
        const auto f = run_burst(static_cast<uint8_t>(EngineCycleType::TWO_STROKE), kStep, 3, 90);
        ck(f.size() == 3, "three squirts delivered", std::to_string(f.size()));
        if (f.size() == 3) {
            const int gap = f[1] - f[0];
            ck(gap == 12, "spaced 36/3 = 12 teeth apart, not 24", std::to_string(gap));
            ck(f.back() < 36, "the burst finishes within the 360 deg cycle",
               "last at tooth " + std::to_string(f.back()));
        }
    }

    std::puts("\n-- rotary: the cycle is 108 teeth (1080 deg) --");
    {
        // The mirror of the two-stroke case: the old maths gave 72, cramming every pulse into the
        // first two thirds of the cycle.
        const auto f = run_burst(static_cast<uint8_t>(EngineCycleType::ROTARY), kStep, 3, 130);
        ck(f.size() == 3, "three squirts delivered", std::to_string(f.size()));
        if (f.size() == 3) {
            const int gap = f[1] - f[0];
            ck(gap == 36, "spaced 108/3 = 36 teeth apart, not 24", std::to_string(gap));
            ck(f.back() >= 72, "the burst uses the WHOLE cycle, not the first two thirds",
               "last at tooth " + std::to_string(f.back()));
        }
    }

    std::puts("\n-- a single pulse fires once, immediately --");
    {
        const auto f = run_burst(static_cast<uint8_t>(EngineCycleType::FOUR_STROKE), kStep, 1, 90);
        ck(f.size() == 1, "exactly one squirt", std::to_string(f.size()));
        if (!f.empty()) ck(f[0] == 0, "on the first tooth after the request",
                           std::to_string(f[0]));
    }

    std::puts("\n-- the train is finite: it does not repeat next cycle --");
    {
        // async_pulses_left_ must run down. A burst that re-armed itself would keep enriching long
        // after the tip-in, which on a real engine is a rich stumble that never clears.
        const auto f = run_burst(static_cast<uint8_t>(EngineCycleType::FOUR_STROKE), kStep, 2, 200);
        ck(f.size() == 2, "still only two squirts after 200 teeth", std::to_string(f.size()));
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "All async-injection tests passed",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
