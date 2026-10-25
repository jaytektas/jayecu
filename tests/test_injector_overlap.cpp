// FUEL REQUESTS ON AN INJECTOR — the opening price is paid once per opening, and fuel always adds.
//
// A request is spray time. Every injector opening goes through EventScheduler::open_inj (sequential
// pulse, prime, async squirt): a CLOSED injector opens and pays its opening price (dead time) on top of
// the spray; one already OPEN has paid it, so it just stays open the spray longer. Closed again means
// closed: the next request pays again.
//
// Before, a close drove its whole mask low whoever had opened it: an async squirt landing inside a
// cylinder's sequential pulse ended that pulse at the squirt's own, earlier close (a 1 ms squirt cut an
// 8 ms pulse to 3 ms), and a squirt that did not truncate added nothing.
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
constexpr uint32_t kCost   = 1000;                // the injectors' opening price (dead time), us

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

uint32_t g_seq_pw   = 0;   // the sequential SPRAY the compute hook arms (0 = no sequential fuel)
uint32_t g_seq_cost = 0;   // ...and that pulse's opening price
void compute_hook(void* ud, uint8_t index, EventAction kind) noexcept {
    auto* s = static_cast<EventScheduler*>(ud);
    if (kind != EventAction::INJ_SCHEDULE || !g_seq_pw) return;
    s->arm_injection(index, s->inj_event_base_angle(index), g_seq_pw, g_seq_cost);
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
        sched.set_open_cost_us(0, kCost);
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
    void prime(uint32_t fuel_us) { sched.request_prime(fuel_us); sched.on_angle_alarm(); }
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
    std::puts("=== Fuel requests on an injector: price once per opening, fuel adds ===");
    auto len = [](const Pin::Pulse& p) { return p.close - p.open; };
    auto near = [](uint32_t v, uint32_t want) { return v + 15 >= want && v <= want + 15; };

    std::puts("\n-- the example: 1 ms into a 5 ms pulse, ask for 3 ms more -> 7 ms left, 8 ms in all --");
    {
        g_seq_pw = 0; g_now = 100000;
        Rig r(false);
        r.prime(4000);                               // 4 ms spray + 1 ms price = a 5 ms pulse, opens now
        r.run(1);                                    // 1 ms in (the tooth before the squirt)
        r.sched.request_async_injection(3000, 1);    // 3 ms of spray, fired on the next tooth
        r.run(12);
        const auto& p = r.inj[0].pulses;
        ck(p.size() == 1, "one opening: the squirt joined the pulse already running", std::to_string(p.size()));
        if (!p.empty()) ck(near(len(p[0]), 8000), "open 8 ms: 5 ms pulse + 3 ms spray, the price paid once", us(len(p[0])));
    }

    std::puts("\n-- a long squirt inside a short prime: the squirt's fuel is ADDED, not just the later close --");
    {
        g_seq_pw = 0; g_now = 200000;
        Rig r(false);
        r.prime(500);                                // 0.5 ms spray + 1 ms price = 1.5 ms
        r.sched.request_async_injection(3000, 1);    // next tooth: 1 ms in, still open
        r.run(10);
        const auto& p = r.inj[1].pulses;
        ck(p.size() == 1, "one opening", std::to_string(p.size()));
        if (!p.empty()) ck(near(len(p[0]), 4500), "open 1.5 + 3 = 4.5 ms", us(len(p[0])));
    }

    std::puts("\n-- an async squirt inside a SEQUENTIAL pulse lengthens it by its spray --");
    {
        g_seq_pw = 0; g_now = 300000;
        Rig r(true);
        g_seq_pw = 7000; g_seq_cost = kCost;         // 7 ms spray + 1 ms price = 8 ms pulses
        r.run(72 * 2);                               // two cycles: the volatile nodes arm, then steady
        const auto& p0 = r.inj[0].pulses;
        const uint32_t ref = p0.empty() ? 0 : len(p0.back());
        ck(near(ref, 8000), "reference: a sequential pulse alone is 8 ms", us(ref));
        bool fired = false;
        for (int i = 0; i < 72 && !fired; ++i) {
            r.run_tooth();
            if (r.inj[0].high && !r.inj[0].pulses.empty() && g_now - r.inj[0].pulses.back().open >= 1500) {
                r.sched.request_async_injection(1000, 1);
                fired = true;
            }
        }
        ck(fired, "a squirt was fired inside cylinder 1's sequential pulse");
        const size_t hit = r.inj[0].pulses.size() - 1;
        r.run(20);
        const auto& p = r.inj[0].pulses;
        ck(near(len(p[hit]), 9000), "that pulse ran 8 + 1 = 9 ms (and was not cut to the squirt's close)", us(len(p[hit])));
        // The other cylinders were closed when the squirt fired: each opened on its own and paid the price.
        bool own_ok = false;
        for (int c = 1; c < 4; ++c)
            for (const auto& q : r.inj[c].pulses) if (near(len(q), 2000)) own_ok = true;
        ck(own_ok, "a closed injector in the same squirt opened for 1 ms spray + 1 ms price = 2 ms");
        g_seq_pw = 0; g_seq_cost = 0;
    }

    std::puts("\n-- closed again means closed: the next request pays the price again --");
    {
        g_seq_pw = 0; g_now = 500000;
        Rig r(false);
        r.prime(1000);                               // 1 + 1 = 2 ms
        r.run(4);                                    // done and closed
        r.sched.request_async_injection(1000, 1);
        r.run(4);
        const auto& p = r.inj[2].pulses;
        ck(p.size() == 2, "two separate openings", std::to_string(p.size()));
        if (p.size() == 2) {
            ck(near(len(p[0]), 2000), "first: 1 ms spray + 1 ms price", us(len(p[0])));
            ck(near(len(p[1]), 2000), "second: pays the price again, 2 ms", us(len(p[1])));
        }
    }

    std::puts("\n-- crank sync: two events folded onto one angle are ONE pulse, not two requests --");
    {
        // A two-event multi-point on a four-stroke: events at 0 and 360 deg. Without the cam the walk visits
        // both every revolution at the same folded angle, and the fuel calc divides by 2 deliveries a
        // cycle on the understanding that they merge (injection_deliveries_per_cycle). Adding them would
        // double the fuel while cranking before cam sync.
        g_now = 900000;
        EngineConfig eng{};
        eng.cylinder_count = 4;
        eng.cycle_type     = static_cast<uint8_t>(EngineCycleType::FOUR_STROKE);
        eng.num_inj_stages = 1;
        eng.inj_stage[0].mode                 = static_cast<uint8_t>(InjectionMode::MULTI_POINT);
        eng.inj_stage[0].injections_per_cycle = 2;
        for (int c = 0; c < 4; ++c) eng.firing_order[c].cyl = static_cast<uint8_t>(c + 1);
        update_cylinder_angles(eng);
        static Pin inj[4]; static AngleAlarm angle; static TimeAlarm time;
        EventScheduler sched;
        sched.set_config(eng, layout_output_map(eng, TestCoils::COP), true);
        for (uint8_t c = 0; c < 4; ++c) sched.assign_inj_channel(c, inj[c]);
        sched.assign_angle_alarm(angle); sched.assign_time_alarm(time);
        g_seq_pw = 2000; g_seq_cost = kCost;         // 2 ms spray + 1 ms price
        sched.register_compute_hook(compute_hook, &sched);
        sched.build_skeleton(kStep);
        sched.set_firing_enabled(true);
        sched.set_phase_known(false);                // CRANK sync: the walk folds to one revolution
        sched.set_open_cost_us(0, kCost);
        for (int t = 0; t < 36 * 6; ++t) {           // six revolutions
            sched.on_virtual_tooth(static_cast<uint16_t>(t % 36), g_now, 1000);
            while (angle.armed) { angle.armed = false; sched.on_angle_alarm(); }
            for (int k = 0; k < 100; ++k) {
                g_now += 10;
                while (time.armed && static_cast<int32_t>(g_now - time.at) >= 0) { time.armed = false; sched.on_time_alarm(); }
            }
        }
        const auto& p = inj[0].pulses;
        bool all_single = !p.empty();
        for (size_t i = 1; i < p.size(); ++i) all_single = all_single && near(len(p[i]), 3000);
        ck(p.size() >= 4, "it fires every revolution", std::to_string(p.size()));
        ck(all_single, "every pulse is ONE pulse long (2 ms + 1 ms), not two added together",
           p.size() > 1 ? us(len(p[1])) : std::string("none"));
        g_seq_pw = 0; g_seq_cost = 0;
    }

    std::puts("\n-- emergency off mid-pulse: everything low, and the old close reopens nothing --");
    {
        g_seq_pw = 0; g_now = 700000;
        Rig r(false);
        r.prime(5000);
        r.run(1);
        ck(r.inj[3].high, "open before the panic stop");
        r.sched.emergency_off();
        ck(!r.inj[3].high, "low at once");
        for (int k = 0; k < 800; ++k) {              // let the old close come due and be served
            g_now += 10;
            if (r.time.armed && static_cast<int32_t>(g_now - r.time.at) >= 0) { r.time.armed = false; r.sched.on_time_alarm(); }
        }
        ck(!r.inj[3].high, "still low after the old close");
        ck(r.inj[3].pulses.size() == 1, "no new pulse", std::to_string(r.inj[3].pulses.size()));
        r.sched.set_firing_enabled(true);            // firing back: a fresh request opens and closes cleanly
        r.prime(1000);
        r.run(4);
        ck(r.inj[3].pulses.size() == 2 && !r.inj[3].high && near(len(r.inj[3].pulses.back()), 2000),
           "after re-enable, a new pulse runs its full 2 ms and closes");
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "All injector-request tests passed", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
