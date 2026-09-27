// INJECTION DISTRIBUTION MODES — what each mode actually delivers, at the pins.
//
// The scheduler used to have one injection node per cylinder driving that cylinder's own channel, so
// three of the four modes in the tune were unreachable: batch and multi-point could not be expressed
// at all, and the InjectionMode field was read only to ask whether it said STAGED. Injection is now
// compiled into a list of EVENTS (EventScheduler::build_inj_events), and a mode is nothing more than
// which channels an event drives and how many events a cycle holds.
//
// This drives EventScheduler through its real ISR entry points (on_virtual_tooth + on_angle_alarm)
// with mock hardware and records which CHANNEL opened at which TOOTH — so it tests delivery, not the
// event list's internal shape. A mode that compiles a beautiful event list and fires the wrong pin
// fails here.
//
//   cmake --build build --target test_injection_modes && ./build/test_injection_modes
#include "test_helpers.h"
#include "Scheduler/EventScheduler.h"
#include "Scheduler/SchedulerTypes.h"
#include "output_rows_helper.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

uint32_t g_now = 0;
constexpr uint32_t kTicksPerSecond = 1'000'000;   // 1 MHz, so 1 tick = 1 us
constexpr AngleDeg10 kStep         = 100;         // 10.0 deg grid -> 36 teeth/rev, 72 teeth/cycle

// One mock per injector channel; records opens only (the close comes off the time list and is not
// what these modes differ in).
struct MockChannel final : ITimerChannel {
    int opens = 0;
    [[nodiscard]] uint32_t get_current_ticks()    const noexcept override { return g_now; }
    [[nodiscard]] uint32_t get_ticks_per_second() const noexcept override { return kTicksPerSecond; }
    void enable_output(OutputAction) noexcept override {}
    void force_output_now(OutputAction a) noexcept override {
        if (a == OutputAction::DRIVE_HIGH) ++opens;
    }
};

// The angle alarm is a "call me at tick X" sink; the scheduler re-arms it for each event in the
// tooth. The test drains it by hand so every due event dispatches deterministically.
struct MockAlarm final : IAlarmTimer {
    bool armed = false;
    void arm(uint32_t) noexcept override { armed = true; }
    void disarm() noexcept override { armed = false; }
    [[nodiscard]] uint32_t now() const noexcept override { return g_now; }
    void register_callback(MatchCallback, void*) noexcept override {}
};

// One injector open, as observed at the pin.
struct Open { int tooth; int channel; };

// Stand in for EnginePositionHal's compute hook: arm each injection event at its own base angle with
// a fixed pulse width (the HAL subtracts the cylinder's opening angle, which is zero here so the
// event lands exactly on its base TDC and the tooth index reads directly as an angle).
void compute_hook(void* ud, uint8_t index, EventAction kind) noexcept {
    auto* s = static_cast<EventScheduler*>(ud);
    if (kind != EventAction::INJ_SCHEDULE) return;
    // One event = one stage now; arm it at its own base angle with a fixed PW (the HAL gates staged
    // engagement on staged_enabled — the test drives every event so all stages are observable at the pins).
    const AngleDeg10 base = s->inj_event_base_angle(index);
    s->arm_injection(index, base, 2000);
}

// Build an engine and run exactly one engine cycle of virtual teeth, returning every injector open.
// The injectors are OUTPUT ROWS, laid out as the wizard does by default (output_rows_helper.h): stage 1
// from LS1, one per cylinder; stage2_outputs > 0 wires a secondary stage in the LS block above stage 1.
struct Result {
    std::vector<Open> opens;
    uint8_t           event_count = 0;
    bool              any_staged  = false;
};

Result run_cycle(InjectionMode mode, uint8_t ncyl, uint8_t cycle_type,
                 uint8_t events_per_cycle = 2, uint8_t stage2_outputs = 0,
                 uint8_t stage3_outputs = 0, const uint8_t* banks = nullptr,
                 bool service_alarm = true, bool phase = true) {
    EngineConfig eng{};
    eng.cylinder_count = ncyl;
    eng.cycle_type     = cycle_type;
    eng.num_inj_stages = (stage3_outputs > 0) ? 3 : (stage2_outputs > 0) ? 2 : 1;
    eng.inj_stage[0].mode                 = static_cast<uint8_t>(mode);
    eng.inj_stage[0].injections_per_cycle = events_per_cycle;
    for (uint8_t s = 1; s <= 2; ++s) {
        const uint8_t outs = (s == 1) ? stage2_outputs : stage3_outputs;
        if (outs == 0) continue;
        eng.inj_stage[s].mode                 = static_cast<uint8_t>(mode);
        eng.inj_stage[s].injections_per_cycle = events_per_cycle;
    }
    // Firing order = cylinder order (cylinder c fires c-th), so the resolver's even-fire TDCs are
    // c × cycle/ncyl — the same evenly-spaced angles the assertions below expect.
    for (uint8_t c = 0; c < ncyl; ++c) {
        eng.firing_order[c].cyl = static_cast<uint8_t>(c + 1);
        eng.cyl[c].bank = banks ? banks[c] : 1;   // default single-bank; BANK-mode tests pass a split
    }
    // A rotary's faces: each rotor fires once per shaft revolution, rotors 180 apart. Position p in
    // the order is face k = p/2 of rotor r = p%2, i.e. faces 1,4,2,5,3,6 (rotor 1 = faces 1-3).
    if (cycle_type == static_cast<uint8_t>(EngineCycleType::ROTARY) && ncyl == 6)
        for (uint8_t p = 0; p < 6; ++p)
            eng.firing_order[p].cyl = static_cast<uint8_t>((p % 2) * 3 + p / 2 + 1);

    static MockChannel inj[MAX_INJ_CHANNELS];
    static MockAlarm   angle_alarm, time_alarm;
    for (auto& ch : inj) ch.opens = 0;
    angle_alarm.armed = false;

    EventScheduler sched;
    update_cylinder_angles(eng);   // config owns each cylinder's TDC angle; the scheduler fetches it
    const uint8_t grouped[MAX_INJ_STAGES] = { ncyl, stage2_outputs, stage3_outputs, 0 };
    sched.set_config(eng, layout_output_map(eng, TestCoils::COP, grouped), true);
    for (uint8_t i = 0; i < MAX_INJ_CHANNELS; ++i) sched.assign_inj_channel(i, inj[i]);
    sched.assign_angle_alarm(angle_alarm);
    sched.assign_time_alarm(time_alarm);
    sched.register_compute_hook(compute_hook, &sched);
    sched.build_skeleton(kStep);
    sched.set_firing_enabled(true);
    sched.set_phase_known(phase);         // true: full cycle visible; false: CRANK sync, buckets fold

    Result r;
    r.event_count = sched.inj_event_count();
    for (uint8_t e = 0; e < r.event_count; ++e)
        if (sched.inj_event_stage(e) != 0) r.any_staged = true;   // any event belonging to a staged stage

    const int teeth_per_cycle = engine_cycle_angle(cycle_type) / kStep;
    // Two cycles: the first arms the VOLATILE nodes from the STATIC schedule nodes (which sit a
    // compute-lead ahead of their event), the second is the steady state we measure.
    for (int pass = 0; pass < 2; ++pass) {
        for (int t = 0; t < teeth_per_cycle; ++t) {
            int before[MAX_INJ_CHANNELS];
            for (int i = 0; i < MAX_INJ_CHANNELS; ++i) before[i] = inj[i].opens;

            g_now += 1000;
            // At CRANK sync the grid index is folded to one revolution, as the HAL feeds it.
            const int per_rev = ANGLE_360 / kStep;
            sched.on_virtual_tooth(static_cast<uint16_t>(phase ? t : t % per_rev), g_now, 1000);
            while (service_alarm && angle_alarm.armed) {   // drain every event due in this tooth
                angle_alarm.armed = false;
                sched.on_angle_alarm();
            }
            // Closes: the pool has one slot per channel, and an open with no free close slot is now
            // refused (a stuck injector otherwise). Serve them each tooth so the pool recycles.
            while (time_alarm.armed) { time_alarm.armed = false; sched.on_time_alarm(); }

            if (pass == 1)
                for (int i = 0; i < MAX_INJ_CHANNELS; ++i)
                    for (int n = before[i]; n < inj[i].opens; ++n) r.opens.push_back({t, i});
        }
    }
    return r;
}

// The teeth at which a given channel opened, in order.
std::vector<int> teeth_of(const Result& r, int channel) {
    std::vector<int> out;
    for (const auto& o : r.opens) if (o.channel == channel) out.push_back(o.tooth);
    return out;
}

// The channels that opened at a given tooth, in ascending order.
std::vector<int> channels_at(const Result& r, int tooth) {
    std::vector<int> out;
    for (const auto& o : r.opens) if (o.tooth == tooth) out.push_back(o.channel);
    return out;
}

std::string join(const std::vector<int>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += ","; s += std::to_string(v[i]); }
    return s.empty() ? "-" : s;
}

int fails = 0;
void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-66s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

constexpr uint8_t FOUR = static_cast<uint8_t>(EngineCycleType::FOUR_STROKE);
constexpr uint8_t TWO  = static_cast<uint8_t>(EngineCycleType::TWO_STROKE);
constexpr uint8_t ROT  = static_cast<uint8_t>(EngineCycleType::ROTARY);

} // namespace

int main() {
    std::puts("=== Injection distribution modes ===");
    // Four cylinders, one injector each (LS0..LS3 -> cyl0..cyl3), TDCs every 180 deg -> teeth
    // 0 / 18 / 36 / 54 on a 10 deg grid.

    std::puts("\n-- Sequential: each injector once per cycle, at its own TDC --");
    {
        const auto r = run_cycle(InjectionMode::SEQUENTIAL, 4, FOUR);
        ck(r.event_count == 4, "one event per cylinder", std::to_string(r.event_count));
        ck(r.opens.size() == 4, "four opens per cycle", std::to_string(r.opens.size()));
        ck(join(teeth_of(r, 0)) == "0",  "injector 1 fires at its own TDC only", join(teeth_of(r, 0)));
        ck(join(teeth_of(r, 1)) == "18", "injector 2 at 180 deg",  join(teeth_of(r, 1)));
        ck(join(teeth_of(r, 2)) == "36", "injector 3 at 360 deg",  join(teeth_of(r, 2)));
        ck(join(teeth_of(r, 3)) == "54", "injector 4 at 540 deg",  join(teeth_of(r, 3)));
    }

    // T1. The angle alarm for an event can lose the race to the NEXT tooth (its time lands after the
    // tooth, or the tooth ISR is serviced first). The new tooth used to re-point the walk and the event
    // was simply lost — a missed injection, or a spark that never discharged the coil. Now the tooth
    // fires what the last one left pending, one tooth late at worst.
    std::puts("\n-- A missed angle alarm fires late at the next tooth, not never --");
    {
        const auto r = run_cycle(InjectionMode::SEQUENTIAL, 4, FOUR, 2, 0, 0, nullptr, /*service*/ false);
        ck(r.opens.size() == 4, "all four injections still delivered", std::to_string(r.opens.size()));
        ck(join(teeth_of(r, 0)) == "1",  "injector 1 one tooth late", join(teeth_of(r, 0)));
        ck(join(teeth_of(r, 3)) == "55", "injector 4 one tooth late", join(teeth_of(r, 3)));
    }

    // T3. Without the cam the walk visits every event every revolution.
    std::puts("\n-- CRANK sync: Sequential squirts per revolution, Any-Sync stays once per cycle --");
    {
        const auto seq = run_cycle(InjectionMode::SEQUENTIAL, 4, FOUR, 2, 0, 0, nullptr, true, false);
        ck(seq.opens.size() == 8, "sequential: 8 opens per cycle (fuel halved to match)",
           std::to_string(seq.opens.size()));
        const auto any = run_cycle(InjectionMode::SEQUENTIAL_ANY_SYNC, 4, FOUR, 2, 0, 0, nullptr, true, false);
        ck(any.opens.size() == 4, "any-sync: 4 opens per cycle", std::to_string(any.opens.size()));
        ck(injection_deliveries_per_cycle(static_cast<uint8_t>(InjectionMode::SEQUENTIAL), FOUR, 1, false) == 2,
           "sequential at crank sync divides by 2");
    }

    // T5. A rotary spans THREE revolutions; the crank-sync walk visited only two, so events in the
    // third (720-1080) never fired before cam sync.
    std::puts("\n-- Rotary at CRANK sync: every event fires every revolution, including 720-1080 --");
    {
        // Two rotors = six faces (the count is faces); each face's event is one squirt.
        const auto ph = run_cycle(InjectionMode::SEQUENTIAL, 6, ROT, 2, 0, 0, nullptr, true, true);
        const auto cr = run_cycle(InjectionMode::SEQUENTIAL, 6, ROT, 2, 0, 0, nullptr, true, false);
        std::printf("    rotor 1 opens at teeth %s, rotor 2 at %s (phase sync)\n",
                    join(teeth_of(ph, 0)).c_str(), join(teeth_of(ph, 1)).c_str());
        ck(ph.opens.size() == 6, "phase sync: one squirt per face", std::to_string(ph.opens.size()));
        // Crank sync: every face event every revolution -> 18 drive calls, and the three faces of a
        // rotor coincide, so each injector still opens once a revolution: 3 per cycle, as with the cam.
        ck(cr.opens.size() == 18, "crank sync: all faces every revolution, 720-1080 included",
           std::to_string(cr.opens.size()));
        std::vector<int> u = teeth_of(cr, 0);
        u.erase(std::unique(u.begin(), u.end()), u.end());   // the three coincident face events are one opening
        ck(join(u) == join(teeth_of(ph, 0)), "rotor 1 opens on the same teeth as with the cam", join(u));
        ck(injection_deliveries_per_cycle(static_cast<uint8_t>(InjectionMode::SEQUENTIAL), ROT, 1, false) == 1,
           "rotary sequential keeps its divisor at crank sync");
    }

    std::puts("\n-- Sequential-Any-Sync: same event shape as Sequential --");
    {
        const auto r = run_cycle(InjectionMode::SEQUENTIAL_ANY_SYNC, 4, FOUR);
        ck(r.event_count == 4, "one event per cylinder", std::to_string(r.event_count));
        ck(join(teeth_of(r, 0)) == "0",  "injector 1 at its own TDC", join(teeth_of(r, 0)));
        ck(join(teeth_of(r, 3)) == "54", "injector 4 at 540 deg", join(teeth_of(r, 3)));
    }

    std::puts("\n-- Semi-Sequential: each injector once per CRANK REVOLUTION --");
    {
        const auto r = run_cycle(InjectionMode::SEMI_SEQUENTIAL, 4, FOUR);
        ck(r.event_count == 8, "two events per cylinder on a four-stroke", std::to_string(r.event_count));
        ck(join(teeth_of(r, 0)) == "0,36",  "injector 1 at its TDC and 360 deg later", join(teeth_of(r, 0)));
        ck(join(teeth_of(r, 1)) == "18,54", "injector 2 likewise", join(teeth_of(r, 1)));
        ck(join(teeth_of(r, 2)) == "0,36",  "injector 3 likewise (TDC 360 + wrap)", join(teeth_of(r, 2)));
        ck(join(teeth_of(r, 3)) == "18,54", "injector 4 likewise", join(teeth_of(r, 3)));
    }

    std::puts("\n-- Semi-Sequential on a TWO-STROKE: one revolution IS the cycle --");
    {
        // The count is derived from the cycle span, not hardcoded to the four-stroke's 2. Getting
        // this wrong doubles a two-stroke's fuel.
        const auto r = run_cycle(InjectionMode::SEMI_SEQUENTIAL, 4, TWO);
        ck(r.event_count == 4, "one event per cylinder, not two", std::to_string(r.event_count));
        ck(join(teeth_of(r, 0)) == "0", "injector 1 fires once per cycle", join(teeth_of(r, 0)));
    }

    std::puts("\n-- Multi-Point: every injector together, N times per cycle --");
    {
        const auto r = run_cycle(InjectionMode::MULTI_POINT, 4, FOUR, 2);
        ck(r.event_count == 2, "two events (one group x 2 per cycle)", std::to_string(r.event_count));
        ck(r.opens.size() == 8, "4 injectors x 2 events = 8 opens", std::to_string(r.opens.size()));
        ck(join(channels_at(r, 0))  == "0,1,2,3", "all four open together at 0 deg",   join(channels_at(r, 0)));
        ck(join(channels_at(r, 36)) == "0,1,2,3", "and again at 360 deg",              join(channels_at(r, 36)));
        ck(channels_at(r, 18).empty(), "nothing in between", join(channels_at(r, 18)));
    }

    std::puts("\n-- Multi-Point at 1 event per cycle: the squirt count is honoured --");
    {
        const auto r = run_cycle(InjectionMode::MULTI_POINT, 4, FOUR, 1);
        ck(r.event_count == 1, "one event", std::to_string(r.event_count));
        ck(r.opens.size() == 4, "all four injectors, once", std::to_string(r.opens.size()));
    }

    std::puts("\n-- Bank: a bank's drivers are CONTIGUOUS, whatever the cylinders are numbered --");
    {
        // Alternating banks prove grouping is by BANK and not by channel index: cyl0,cyl2 = bank 1;
        // cyl1,cyl3 = bank 2. TDCs (4-cyl, cyl order) = 0,180,360,540, so bank-1 earliest = cyl0 @0deg,
        // bank-2 earliest = cyl1 @180deg. interval = 720/2 = 360deg.
        //
        // THE DRIVERS CHANGED, AND ON PURPOSE. This used to interleave — bank 1 on LS0,LS2 and bank 2 on
        // LS1,LS3 — because the binding handed driver k to CYLINDER k and the banks happened to
        // alternate. That is a statement about how the cylinders were numbered, not about the banks, and
        // renumbering the same engine 1,1,2,2 collapsed both banks into one group. A grouped stage now
        // divides its block BETWEEN the banks: bank 1 takes the first half, bank 2 the second. Same
        // engine, same injectors, and the answer no longer moves when the numbering does.
        const uint8_t banks[4] = {1, 2, 1, 2};
        const auto r = run_cycle(InjectionMode::BANK, 4, FOUR, 2, 0, 0, banks);
        ck(r.event_count == 4, "two banks x 2 events", std::to_string(r.event_count));
        ck(r.opens.size() == 8, "each injector fires twice", std::to_string(r.opens.size()));
        ck(join(channels_at(r, 0))  == "0,1", "bank 1 takes the first half of the block (LS0,LS1)",
           join(channels_at(r, 0)));
        ck(join(channels_at(r, 18)) == "2,3", "bank 2 takes the second (LS2,LS3) at 180 deg",
           join(channels_at(r, 18)));
        ck(join(channels_at(r, 36)) == "0,1", "bank 1 again at 360 deg", join(channels_at(r, 36)));
        ck(join(channels_at(r, 54)) == "2,3", "bank 2 again at 540 deg", join(channels_at(r, 54)));

        // ...AND THE POINT OF THE CHANGE: number the same engine the other way and nothing moves.
        // Under the old rule this produced ONE group of all four at 0 deg, because cyl0 and cyl1 were
        // both bank 1 and bank 2 held no cylinder with a driver.
        // ONE squirt per cycle here, deliberately: with two, bank 2's second event wraps onto bank 1's
        // first (they are half a cycle apart and the interval IS half a cycle), so both groups land on
        // the same two angles and the test cannot tell them apart. That coincidence is correct
        // behaviour and it is exactly what would hide a regression.
        const uint8_t blocked[4] = {1, 1, 2, 2};
        const auto b = run_cycle(InjectionMode::BANK, 4, FOUR, 1, 0, 0, blocked);
        ck(b.event_count == 2, "blocked numbering still gives two bank groups",
           std::to_string(b.event_count));
        ck(join(channels_at(b, 0)) == "0,1", "bank 1 (cyl1,cyl2) is still the first half at 0 deg",
           join(channels_at(b, 0)));
        ck(join(channels_at(b, 36)) == "2,3",
           "bank 2 (cyl3,cyl4) is still the second at 360 deg — the numbering no longer decides it",
           join(channels_at(b, 36)));
    }

    std::puts("\n-- Bank on a single-bank engine collapses to every output together --");
    {
        const uint8_t banks[4] = {1, 1, 1, 1};
        const auto r = run_cycle(InjectionMode::BANK, 4, FOUR, 1, 0, 0, banks);
        ck(r.event_count == 1, "one group = the whole engine", std::to_string(r.event_count));
        ck(r.opens.size() == 4, "all four injectors, once", std::to_string(r.opens.size()));
        ck(join(channels_at(r, 0)) == "0,1,2,3", "every output together at the earliest TDC",
           join(channels_at(r, 0)));
    }

    std::puts("\n-- One injector per output: no two cylinders ever share a channel --");
    {
        // The driver cannot run two injectors; grouped modes group DISTINCT outputs in time, never
        // combine them. So every open is on a channel matching exactly one cylinder.
        const uint8_t banks[4] = {1, 2, 1, 2};
        const auto r = run_cycle(InjectionMode::BANK, 4, FOUR, 2, 0, 0, banks);
        bool distinct = true;
        for (const auto& o : r.opens) if (o.channel < 0 || o.channel >= 4) distinct = false;
        ck(distinct, "every open lands on a real per-cylinder output");
    }

    std::puts("\n-- Staging: a secondary stage produces its own independent events --");
    {
        // Stage 2 = 4 outputs in the LS block above stage 1 (LS4..LS7 -> cyl0..cyl3); its own events now.
        const auto r = run_cycle(InjectionMode::SEQUENTIAL, 4, FOUR, 1, /*stage2_outputs=*/4);
        ck(r.any_staged, "a staged secondary produces its own events");
        const auto plain = run_cycle(InjectionMode::SEQUENTIAL, 4, FOUR, 1);
        ck(!plain.any_staged, "and an engine with no secondary stage has none");
    }

    std::puts("\n-- Three stages: each fires its own contiguous LS block, together --");
    {
        // 4-cyl sequential, 3 stages of 4 outputs each: stage 1 = LS0-3, stage 2 = LS4-7,
        // stage 3 = LS8-11. Cylinder 0 fires at TDC 0 (tooth 0) on all three: LS0, LS4, LS8.
        const auto r = run_cycle(InjectionMode::SEQUENTIAL, 4, FOUR, 1, /*stage2*/4, /*stage3*/4);
        ck(r.event_count == 12, "4 cyl x 3 stages = 12 independent events", std::to_string(r.event_count));
        ck(join(channels_at(r, 0)) == "0,4,8", "cyl1: primary LS0 + stage2 LS4 + stage3 LS8, together",
           join(channels_at(r, 0)));
        ck(join(channels_at(r, 18)) == "1,5,9", "cyl2 at 180deg: LS1 + LS5 + LS9", join(channels_at(r, 18)));
        ck(join(channels_at(r, 54)) == "3,7,11", "cyl4 at 540deg: LS3 + LS7 + LS11", join(channels_at(r, 54)));
        ck(r.opens.size() == 12, "4 cylinders x 3 stages = 12 opens", std::to_string(r.opens.size()));
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "OK", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
