// DWELL MUST PRECEDE ITS SPARK — and no coil may stay charged for ever (audit T2).
//
// The dwell is armed a whole cycle ahead, when the previous spark fires, from the OLD advance. The spark
// is re-armed ~90 deg before TDC with the NEW advance. When the advance jumps by more than the dwell
// angle (cranking table -> main map), the new spark came first, fired on an uncharged coil, and the dwell
// then started and held the coil on for a full cycle. EventScheduler::fit_dwell re-fits the dwell to the
// spark just armed; the max-dwell cutoff releases any coil that loses its spark anyway.
//
// Driven through the real ISR entry points with time-accurate alarms (1 tick = 0.01 deg).
#include "test_helpers.h"
#include "Scheduler/EventScheduler.h"
#include "Scheduler/SchedulerTypes.h"
#include "output_rows_helper.h"
#include <cstdio>
#include <vector>

namespace {

uint32_t g_now = 0;
constexpr uint32_t kTps  = 1'000'000;
constexpr AngleDeg10 kStep = 100;          // 10 deg grid
constexpr uint32_t kTooth = 1000;          // ticks per grid tooth

struct Coil final : ITimerChannel {
    bool on = false;
    uint32_t since = 0;
    uint32_t longest = 0;
    int charges = 0, sparks = 0;
    [[nodiscard]] uint32_t get_current_ticks()    const noexcept override { return g_now; }
    [[nodiscard]] uint32_t get_ticks_per_second() const noexcept override { return kTps; }
    void enable_output(OutputAction) noexcept override {}
    void force_output_now(OutputAction a) noexcept override {
        const bool high = (a == OutputAction::DRIVE_HIGH);
        if (high && !on) { on = true; since = g_now; ++charges; }
        else if (!high && on) {
            on = false; ++sparks;
            if (g_now - since > longest) longest = g_now - since;
        }
    }
};
struct NullCh final : ITimerChannel {
    [[nodiscard]] uint32_t get_current_ticks()    const noexcept override { return g_now; }
    [[nodiscard]] uint32_t get_ticks_per_second() const noexcept override { return kTps; }
    void enable_output(OutputAction) noexcept override {}
    void force_output_now(OutputAction) noexcept override {}
};

struct Alarm final : IAlarmTimer {
    bool armed = false;
    uint32_t at = 0;
    void arm(uint32_t t) noexcept override { armed = true; at = t; }
    void disarm() noexcept override { armed = false; }
    [[nodiscard]] uint32_t now() const noexcept override { return g_now; }
    void register_callback(MatchCallback, void*) noexcept override {}
};

struct Rig {
    EventScheduler s;
    Alarm aa, ta;
    AngleDeg10 adv = 50;       // 5.0 deg BTDC
    AngleDeg10 dwell = 50;     // 5.0 deg of dwell (≈ cranking)
    bool fit = true;
};

AngleDeg10 wrap(Rig& r, int32_t a) { return angle_wrap(static_cast<AngleDeg10>(a), r.s.cycle_angle()); }

void hook(void* ud, uint8_t cyl, EventAction kind) noexcept {
    auto& r = *static_cast<Rig*>(ud);
    const AngleDeg10 spark = wrap(r, r.s.tdc_angle(cyl) - r.adv);
    if (kind == EventAction::IGN_SCHEDULE) {
        r.s.arm_spark(cyl, spark);
        if (r.fit) r.s.fit_dwell(cyl, wrap(r, spark - r.dwell), false);
    } else if (kind == EventAction::IGN_DWELL_START) {
        r.s.arm_dwell(cyl, wrap(r, spark - r.dwell));   // next cycle's dwell, from the spark just fired
    }
}

// Service every alarm due before `until`, at its own tick.
void drain(Rig& r, uint32_t until) {
    for (int guard = 0; guard < 1000; ++guard) {
        Alarm* a = nullptr;
        if (r.aa.armed && (int32_t)(r.aa.at - until) < 0) a = &r.aa;
        if (r.ta.armed && (int32_t)(r.ta.at - until) < 0 && (!a || (int32_t)(r.ta.at - a->at) < 0)) a = &r.ta;
        if (!a) return;
        a->armed = false;
        if ((int32_t)(a->at - g_now) > 0) g_now = a->at;
        if (a == &r.aa) r.s.on_angle_alarm(); else r.s.on_time_alarm();
    }
}

Coil  g_coil[MAX_IGN_CHANNELS];
NullCh g_null;

void setup(Rig& r) {
    EngineConfig eng{};
    eng.cylinder_count = 4;
    eng.cycle_type = static_cast<uint8_t>(EngineCycleType::FOUR_STROKE);
    for (uint8_t c = 0; c < 4; ++c) { eng.firing_order[c].cyl = static_cast<uint8_t>(c + 1); eng.cyl[c].bank = 1; }
    update_cylinder_angles(eng);
    r.s.set_config(eng, layout_output_map(eng, TestCoils::COP), true);
    for (uint8_t i = 0; i < MAX_IGN_CHANNELS; ++i) { g_coil[i] = Coil{}; r.s.assign_ign_channel(i, g_coil[i]); }
    for (uint8_t i = 0; i < MAX_INJ_CHANNELS; ++i) r.s.assign_inj_channel(i, g_null);
    r.s.assign_angle_alarm(r.aa);
    r.s.assign_time_alarm(r.ta);
    r.s.register_compute_hook(hook, &r);
    r.s.build_skeleton(kStep);
    r.s.set_firing_enabled(true);
    r.s.set_phase_known(true);
}

void run_cycles(Rig& r, int cycles, int& tooth) {
    const int per = r.s.cycle_angle() / kStep;
    for (int k = 0; k < cycles * per; ++k, ++tooth) {
        const uint32_t t = static_cast<uint32_t>(tooth) * kTooth;
        drain(r, t);
        g_now = t;
        r.s.on_virtual_tooth(static_cast<uint16_t>(tooth % per), t, kTooth);
        drain(r, t + 1);
    }
    drain(r, static_cast<uint32_t>(tooth) * kTooth);   // finish the last tooth's events
}

uint32_t longest(int n) { uint32_t m = 0; for (int i = 0; i < n; ++i) m = std::max(m, g_coil[i].longest); return m; }
int sparks(int n) { int s = 0; for (int i = 0; i < n; ++i) s += g_coil[i].sparks; return s; }

} // namespace

int main() {
    std::puts("=== Dwell fit + max dwell ===");
    const uint32_t dwell_ticks = 50 * 10;          // 5 deg = 500 ticks

    SECTION("advance jumps past the dwell: the dwell still precedes the spark");
    {
        g_now = 0;
        Rig r; setup(r);
        int tooth = 0;
        run_cycles(r, 3, tooth);
        CHECK(sparks(4) >= 8);                     // firing in steady state
        CHECK(longest(4) <= dwell_ticks + kTooth); // never more than a tooth past the dwell
        const int before = sparks(4);
        r.adv = 350;                               // 5 -> 35 deg: a 30 deg jump on a 5 deg dwell
        run_cycles(r, 3, tooth);
        std::printf("  sparks after jump %d\n", sparks(4) - before);
        CHECK(sparks(4) - before == 12);           // every cylinder, every cycle
        CHECK(longest(4) <= dwell_ticks + kTooth); // no coil held for a cycle
        std::printf("  longest charge %u ticks (dwell %u)\n", (unsigned)longest(4), (unsigned)dwell_ticks);
    }

    SECTION("without fit_dwell the same jump holds a coil for most of a cycle (the old bug)");
    {
        g_now = 0;
        Rig r; r.fit = false; setup(r);
        int tooth = 0;
        run_cycles(r, 3, tooth);
        r.adv = 350;
        run_cycles(r, 3, tooth);
        std::printf("  longest charge %u ticks\n", (unsigned)longest(4));
        CHECK(longest(4) > 20 * kTooth);           // 200+ deg: the failure this test exists for
    }

    SECTION("...and the max-dwell cutoff caps even that");
    {
        g_now = 0;
        Rig r; r.fit = false; setup(r);
        r.s.set_max_dwell_us(2 * 500);             // 2x a 5 deg dwell at this speed (1 tick = 1 us)
        int tooth = 0;
        run_cycles(r, 3, tooth);
        r.adv = 350;
        run_cycles(r, 3, tooth);
        CHECK(longest(4) <= 1000 + kTooth);        // released within a tooth of the limit
        CHECK(r.s.overdwell_count() > 0);
    }

    SECTION("first spark after sync has a dwell (none armed by a previous spark)");
    {
        g_now = 0;
        Rig r; setup(r);
        int tooth = 0;
        run_cycles(r, 1, tooth);
        int with_charge = 0;
        for (int i = 0; i < 4; ++i) if (g_coil[i].charges >= 1 && g_coil[i].sparks >= 1) ++with_charge;
        for (int i = 0; i < 4; ++i) std::printf("  coil %d charges %d sparks %d\n", i, g_coil[i].charges, g_coil[i].sparks);
        CHECK(with_charge == 4);                   // every coil charged and fired in cycle one
    }

    return test_summary();
}
