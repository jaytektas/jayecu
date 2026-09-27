#include "test_helpers.h"
#include "../firmware/Scheduler/SegmentTimer.h"

#include <vector>

// A synthetic wheel: emit `teeth` evenly spaced teeth per cycle at a constant tick rate, optionally
// stretching ONE named segment (the misfire — the crank slowing because a cylinder did not fire).
// Availability is decided from the measured pitch on the first tooth, so a test that wants a
// configured timer has to show it one.
static void pitch_hint(SegmentTimer& st, AngleDeg10 pitch) { st.on_tooth(0, 0, pitch); }

struct Wheel {
    SegmentTimer& st;
    AngleDeg10 cycle;
    int teeth;
    uint32_t ticks_per_tooth;
    uint32_t tick = 0;
    AngleDeg10 angle = 0;

    void spin(int n, AngleDeg10 slow_from = -1, AngleDeg10 slow_to = -1, float factor = 1.0f) {
        const AngleDeg10 pitch = static_cast<AngleDeg10>(cycle / teeth);
        for (int i = 0; i < n; ++i) {
            const bool slow = (slow_from >= 0 && angle >= slow_from && angle < slow_to);
            tick += static_cast<uint32_t>(ticks_per_tooth * (slow ? factor : 1.0f));
            angle = static_cast<AngleDeg10>((angle + pitch) % cycle);
            st.on_tooth(angle, tick, pitch);
        }
    }
};

static std::vector<SegmentTimer::Segment> drain(SegmentTimer& st) {
    std::vector<SegmentTimer::Segment> out;
    SegmentTimer::Segment s{};
    while (st.pop(s)) out.push_back(s);
    return out;
}

int main() {
    fprintf(stdout, "=== SegmentTimer ===\n");

    // A 4-cylinder, 720 deg cycle, TDCs in firing order 1-3-4-2 -> angles 0, 180, 360, 540.
    const AngleDeg10 CYCLE = 7200;
    const AngleDeg10 tdc4[4] = { 0, 1800, 3600, 5400 };

    SECTION("a 36-tooth wheel gives plenty of teeth per segment and is available");
    {
        SegmentTimer st;
        st.configure(tdc4, 4, CYCLE); pitch_hint(st, 200);          // 20 deg pitch -> 36 teeth/cycle -> 9 per segment
        CHECK(st.available());
        CHECK_NEAR(st.teeth_per_seg(), 9.0f, 0.01f);
    }

    SECTION("a coarse wheel is UNAVAILABLE rather than approximate");
    {
        // 4 teeth across a 720 deg cycle on a 4-cylinder is one tooth per segment: the answer would be
        // decided by where the teeth happen to fall, not by what the engine did.
        SegmentTimer st;
        st.configure(tdc4, 4, CYCLE); pitch_hint(st, 1800);         // 180 deg pitch -> 4 teeth/cycle -> 1 per segment
        CHECK(!st.available());
        CHECK_NEAR(st.teeth_per_seg(), 1.0f, 0.01f);

        Wheel w{st, CYCLE, 4, 1000};
        w.spin(40);
        CHECK(drain(st).empty());                   // and it emits NOTHING, not noise
        CHECK(st.segments() == 0);
    }

    SECTION("steady spin -> one segment per cylinder per cycle, all equal");
    {
        SegmentTimer st;
        st.configure(tdc4, 4, CYCLE); pitch_hint(st, 200);
        Wheel w{st, CYCLE, 36, 1000};
        w.spin(36 * 3);                             // three full cycles
        auto segs = drain(st);
        fprintf(stdout, "  segments=%zu  first=%u ticks\n", segs.size(), segs.empty() ? 0 : segs[0].ticks);
        CHECK(segs.size() >= 8);                    // 4 per cycle, minus the first partial
        bool all_equal = true, all_nonzero = true;
        for (auto& s : segs) {
            if (s.ticks != segs[0].ticks) all_equal = false;
            if (s.ticks == 0) all_nonzero = false;
        }
        CHECK(all_nonzero);
        CHECK(all_equal);
        CHECK_NEAR(static_cast<float>(segs[0].ticks), 9000.0f, 1.0f);   // 9 teeth x 1000 ticks
    }

    SECTION("every cylinder gets its own segment, in firing order");
    {
        SegmentTimer st;
        st.configure(tdc4, 4, CYCLE); pitch_hint(st, 200);
        Wheel w{st, CYCLE, 36, 1000};
        w.spin(36 * 2);
        auto segs = drain(st);
        bool seen[4] = {};
        for (auto& s : segs) { CHECK(s.cyl < 4); seen[s.cyl] = true; }
        CHECK(seen[0] && seen[1] && seen[2] && seen[3]);
    }

    // ---- The actual measurement: a slow segment IS the misfire signal ---------------------------

    SECTION("a cylinder that does not fire leaves a LONGER segment");
    {
        SegmentTimer st;
        st.configure(tdc4, 4, CYCLE); pitch_hint(st, 200);
        Wheel w{st, CYCLE, 36, 1000};
        w.spin(36);                                  // one clean cycle to get going
        drain(st);
        // Stretch the 180..360 deg span — the segment opened by the TDC at 180, i.e. cylinder 1.
        w.spin(36, 1800, 3600, 1.30f);
        auto segs = drain(st);
        uint32_t slow = 0, normal = 0;
        for (auto& s : segs) {
            if (s.cyl == 1) slow = s.ticks;
            else if (!normal) normal = s.ticks;
        }
        fprintf(stdout, "  misfiring cyl1=%u ticks, a firing one=%u ticks\n", slow, normal);
        CHECK(slow > normal);
        CHECK_NEAR(static_cast<float>(slow) / static_cast<float>(normal), 1.30f, 0.02f);
    }

    SECTION("the segment that spans the cycle origin is measured, not split");
    {
        // The last cylinder's segment runs from 540 deg across 0 to 180. Getting the wrap wrong shows
        // up as a missing segment or one with an absurd length, so assert it directly.
        SegmentTimer st;
        st.configure(tdc4, 4, CYCLE); pitch_hint(st, 200);
        Wheel w{st, CYCLE, 36, 1000};
        w.spin(36 * 3);
        auto segs = drain(st);
        int n3 = 0;
        for (auto& s : segs) if (s.cyl == 3) { ++n3; CHECK_NEAR(static_cast<float>(s.ticks), 9000.0f, 1.0f); }
        CHECK(n3 >= 2);
    }

    SECTION("reset abandons the segment in flight rather than closing it across the gap");
    {
        // After a sync loss the next tick is unrelated to the one before it. Closing a segment across
        // that discontinuity would manufacture an enormous elapsed time and read as a dead cylinder.
        SegmentTimer st;
        st.configure(tdc4, 4, CYCLE); pitch_hint(st, 200);
        Wheel w{st, CYCLE, 36, 1000};
        w.spin(20);
        drain(st);
        st.reset();
        CHECK(st.segments() == 0);
        Wheel w2{st, CYCLE, 36, 1000};
        w2.tick = 999999;                            // a wildly different timebase after the gap
        w2.spin(36);
        auto segs = drain(st);
        for (auto& s : segs) CHECK_NEAR(static_cast<float>(s.ticks), 9000.0f, 1.0f);
    }

    SECTION("a full ring drops rather than stalling the capture ISR");
    {
        SegmentTimer st;
        st.configure(tdc4, 4, CYCLE); pitch_hint(st, 200);
        Wheel w{st, CYCLE, 36, 1000};
        w.spin(36 * 20);                             // ~80 segments, never drained
        CHECK(st.dropped() > 0);
        CHECK(st.segments() > 60);                   // it kept COUNTING what it could not queue
    }

    SECTION("an unconfigured timer is inert");
    {
        SegmentTimer st;
        CHECK(!st.available());
        st.on_tooth(100, 1000, 200);
        SegmentTimer::Segment s{};
        CHECK(!st.pop(s));
    }

    return test_summary();
}
