// OVERLAPPING INJECTOR PULSES — an injector opened twice stays open until the LATER close.
//
// Every injector opening goes through EventScheduler::open_inj (sequential pulse, prime, async squirt)
// and every close comes off the time list in on_time_alarm. A close used to drive its whole mask low
// whoever had opened it, so an async squirt landing inside a cylinder's sequential pulse ended that
// pulse at the squirt's own, earlier close: the cylinder lost the rest of its fuel for the cycle. On a
// tip-in — the moment the async fuel exists for — and on boost, where the injectors are open most of
// the cycle, that turned extra fuel into less.
//
// This drives the real ISR entry points (on_virtual_tooth, on_angle_alarm, on_time_alarm) against mock
// pins that record the time of every level change, with the time alarm served at the tick it was
// armed for — so what is measured is how long each injector was actually open.
//
//   cmake --build build --target test_injector_overlap && ./build/test_injector_overlap
#include "test_helpers.h"
#include "Scheduler/EventScheduler.h"
#include "Scheduler/SchedulerTypes.h"
#include "output_rows_helper.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

uint32_t g_now = 0;
constexpr uint32_t kTicksPerSecond = 1'000'000;   // 1 MHz: 1 tick = 1 us
constexpr AngleDeg10 kStep = 100;                 // 10 deg grid, 1 ms per tooth below

// A pin that remembers when it went high and low. An open on a pin already high is not a new pulse.
struct Pin final : ITimerChannel {
    bool high = false;
    struct Pulse { uint32_t open, close; };
    std::vector<Pulse> pulses;
    [[nodiscard]] uint32_t get_current_ticks()    const noexcept override { return g_now; }
    [[nodiscard]] uint32_t get_ticks_per_second() const noexcept override { return kTicksPerSecond; }
    void enable_output(OutputAction) noexcept override {}
    void force_output_now(OutputAction a) noexcept override {
        const bool h = (a == OutputAction::DRIVE_HIGH);
        if (h && !high) pulses.push_back({g_now, 0});
        if (!h && high && !pulses.empty()) pulses.back().close = g_now;
        high = h;
    }
};

// The time alarm remembers the tick it was armed for, so the test can fire it exactly then.
struct TimeAlarm final : IAlarmTimer {
    bool armed = false; uint32_t at = 0;
    void arm(uint32_t t) noexcept override { armed = true; at = t; }
    void disarm() noexcept override { armed = false; }
    [[nodiscard]] uint32_t now() const noexcept override { return g_now; }
    void register_callback(MatchCallback, void*) noexcept override {}
};
struct AngleAlarm final : IAlarmTimer {
    bool armed = false;
    void arm(uint32_t) noexcept override { armed = true; }
    void disarm() noexcept override { armed = false; }
    [[nodiscard]] uint32_t now() const noexcept override { return g_now; }
    void register_callback(MatchCallback, void*) noexcept override {}
};

uint32_t g_seq_pw = 0;   // the sequential pulse width the compute hook arms (0 = no sequential fuel)
void compute_hook(void* ud, uint8_t index, EventAction kind) noexcept {
    auto* s = static_cast<EventScheduler*>(ud);
    if (kind != EventAction::INJ_SCHEDULE || !g_seq_pw) return;
    s->arm_injection(index, s->inj_event_base_angle(index), g_seq_pw);
}

struct Rig {
    EngineConfig eng{};
    Pin inj[4];
    AngleAlarm angle;
    TimeAlarm  time;
    EventScheduler sched;
    int tooth = 0;
    explicit Rig(bool sequential) {
        eng.cylinder_count = 4;
        eng.cycle_type     = static_cast<uint8_t>(EngineCycleType::FOUR_STROKE);
        eng.num_inj_stages = 1;
        eng.inj_stage[0].mode                 = static_cast<uint8_t>(InjectionMode::SEQUENTIAL);
        eng.inj_stage[0].injections_per_cycle = 1;
        for (int c = 0; c < 4; ++c) eng.firing_order[c].cyl = static_cast<uint8_t>(c + 1);
        update_cylinder_angles(eng);
        sched.set_config(eng, layout_output_map(eng, TestCoils::COP), true);
        for (uint8_t c = 0; c < 4; ++c) sched.assign_inj_channel(c, inj[c]);
        sched.assign_angle_alarm(angle);
        sched.assign_time_alarm(time);
        if (sequential) sched.register_compute_hook(compute_hook, &sched);
        sched.build_skeleton(kStep);
        sched.set_firing_enabled(true);
        sched.set_phase_known(true);
    }
    // One tooth: the tooth ISR, every angle event due, then 1 ms of time in 10 us steps with each close
    // served at the tick it was booked for.
    void run_tooth() {
        sched.on_virtual_tooth(static_cast<uint16_t>(tooth % 72), g_now, 1000);
        ++tooth;
        while (angle.armed) { angle.armed = false; sched.on_angle_alarm(); }
        for (int k = 0; k < 100; ++k) {
            g_now += 10;
            while (time.armed && static_cast<int32_t>(g_now - time.at) >= 0) {
                time.armed = false; sched.on_time_alarm();
            }
        }
    }
    void run(int teeth) { for (int i = 0; i < teeth; ++i) run_tooth(); }
    // The prime is consumed in the angle-alarm ISR (on_angle_alarm), which a rig with no scheduled
    // angle events never runs on its own — so serve it here, at the current tick, as the ISR would.
    void prime(uint32_t pw_us) { sched.request_prime(pw_us); sched.on_angle_alarm(); }
};

int fails = 0;
void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-70s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}
std::string us(uint32_t v) { return std::to_string(v) + " us"; }

} // namespace

int main() {
    std::puts("=== Overlapping injector pulses ===");

    std::puts("\n-- a short async squirt inside a long prime does not cut the prime short --");
    {
        g_seq_pw = 0; g_now = 100000;
        Rig r(false);
        r.prime(6000);                               // 6 ms, every injector, opens now
        r.run(2);
        r.sched.request_async_injection(1000, 1);    // 1 ms squirt, ~1-2 ms into the prime
        r.run(12);
        const auto& p = r.inj[0].pulses;
        ck(p.size() == 1, "one continuous open (the squirt merged into the prime)", std::to_string(p.size()));
        if (!p.empty()) {
            const uint32_t len = p[0].close - p[0].open;
            ck(len >= 5990 && len <= 6010, "open for the prime's full 6 ms, not cut at the squirt's close", us(len));
        }
    }

    std::puts("\n-- a long squirt starting inside a short prime keeps the injector open to ITS close --");
    {
        g_seq_pw = 0; g_now = 200000;
        Rig r(false);
        r.prime(1500);                               // 1.5 ms, opens now
        r.sched.request_async_injection(3000, 1);    // 3 ms, fired on the next tooth (inside the prime)
        r.run(10);
        const auto& p = r.inj[1].pulses;
        ck(p.size() == 1, "one continuous open", std::to_string(p.size()));
        if (!p.empty()) {
            const uint32_t len = p[0].close - p[0].open;
            ck(len >= 2990 && len <= 3010, "open until the later close (3 ms), not the prime's 1.5 ms", us(len));
        }
    }

    std::puts("\n-- an async squirt inside a SEQUENTIAL pulse does not truncate it --");
    {
        g_seq_pw = 0; g_now = 300000;
        Rig r(true);
        g_seq_pw = 8000;                             // 8 ms sequential pulses: open across ~8 teeth
        r.run(72 * 2);                               // two cycles: the volatile nodes arm, then steady
        // Find the most recent complete sequential pulse on cylinder 1 as the reference length.
        const auto& p0 = r.inj[0].pulses;
        const uint32_t ref = p0.empty() ? 0 : p0.back().close - p0.back().open;
        ck(ref >= 7990 && ref <= 8010, "reference: a sequential pulse is 8 ms", us(ref));
        // Now run tooth by tooth until cylinder 1 is mid-pulse, then fire a 1 ms squirt.
        bool fired = false;
        for (int i = 0; i < 72 && !fired; ++i) {
            r.run_tooth();
            if (r.inj[0].high && !r.inj[0].pulses.empty() && g_now - r.inj[0].pulses.back().open >= 1500) {
                r.sched.request_async_injection(1000, 1);
                fired = true;
            }
        }
        ck(fired, "a squirt was fired inside cylinder 1's sequential pulse");
        const size_t n_before = r.inj[0].pulses.size();
        r.run(20);
        const auto& p = r.inj[0].pulses;
        if (n_before >= 1 && p.size() >= n_before) {
            const auto& hit = p[n_before - 1];
            const uint32_t len = hit.close - hit.open;
            ck(len >= 7990 && len <= 8010, "that sequential pulse still ran its full 8 ms", us(len));
        }
    }

    std::puts("\n-- pulses that do NOT overlap still open and close separately --");
    {
        g_seq_pw = 0; g_now = 500000;
        Rig r(false);
        r.prime(1000);
        r.run(4);                                    // prime 1 ms, done
        r.sched.request_async_injection(1000, 1);
        r.run(4);
        const auto& p = r.inj[2].pulses;
        ck(p.size() == 2, "two separate pulses", std::to_string(p.size()));
        if (p.size() == 2) {
            ck(p[0].close - p[0].open >= 990 && p[0].close - p[0].open <= 1010, "first 1 ms", us(p[0].close - p[0].open));
            ck(p[1].close - p[1].open >= 990 && p[1].close - p[1].open <= 1010, "second 1 ms", us(p[1].close - p[1].open));
        }
    }

    std::puts("\n-- emergency off mid-pulse: everything low, and the stale close reopens nothing --");
    {
        g_seq_pw = 0; g_now = 700000;
        Rig r(false);
        r.prime(5000);
        r.run(1);
        ck(r.inj[3].high, "open before the panic stop");
        r.sched.emergency_off();
        ck(!r.inj[3].high, "low at once");
        for (int k = 0; k < 600; ++k) {              // let the old close come due and be served
            g_now += 10;
            if (r.time.armed && static_cast<int32_t>(g_now - r.time.at) >= 0) { r.time.armed = false; r.sched.on_time_alarm(); }
        }
        ck(!r.inj[3].high, "still low after the stale close");
        ck(r.inj[3].pulses.size() == 1, "no new pulse", std::to_string(r.inj[3].pulses.size()));
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "All overlap tests passed", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
