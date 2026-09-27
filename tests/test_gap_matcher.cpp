// Standalone tests for the GapMatcher primitive (generic-trigger step 2). Subsumes
// EVEN/DISTRIBUTOR (ngap=0), MISSING_TOOTH (ngap=1), and PATTERN_EXCEPT multi-gap (ngap>1).
// Feeds present-tooth periods directly (gap teeth = ratio× a normal tooth).

#include "test_helpers.h"
#include "GapMatcher.h"
#include <vector>
#include <cstdint>

static bool is_gap_i(const std::vector<int>& g, int i) { for (int x : g) if (x == i) return true; return false; }
static int  present_of(int slots, int ngap, int ratio) { return slots - ngap * (ratio - 1); }

// Feed `cycles` cycles of a gap wheel at constant velocity, starting at present-index `start`.
static void feed_gap(GapMatcher& m, int slots, const std::vector<int>& gaps, int ratio,
                     uint32_t Pt, int cycles, int start = 0) {
    const int P = present_of(slots, (int)gaps.size(), ratio);
    for (int e = 0; e < cycles * P; ++e) {
        int i = (start + e) % P;
        m.feed(static_cast<uint32_t>(is_gap_i(gaps, i) ? ratio : 1) * Pt);
    }
}
// Expected absolute angle of present-index i (gap teeth advance ratio× tooth_angle).
static AngleDeg10 ang_of(int i, const std::vector<int>& gaps, int ratio, AngleDeg10 ta) {
    int a = 0; for (int j = 1; j <= i; ++j) a += (is_gap_i(gaps, j) ? ratio : 1) * ta;
    return static_cast<AngleDeg10>(a % 3600);
}

int main() {
    fprintf(stdout, "=== GapMatcher (generic GAP/EVEN primitive) ===\n");
    const uint32_t Pt = 1000;

    SECTION("ngap=0 → EVEN/distributor: relative lock, constant pitch");
    {
        GapMatcher m; m.configure(3600, 3, nullptr, nullptr, 0, 25);  // 3 even teeth (6-cyl dizzy)
        for (int i = 0; i < 10; ++i) m.feed(1200);                    // 120°/tooth steady
        CHECK(m.is_locked());
        CHECK(!m.is_absolute());
        CHECK(m.pitch() == 1200);
    }

    SECTION("ngap=1 → 36-1 missing-tooth: absolute lock, gap pitch = 2×");
    {
        uint8_t gi[1] = {0}, gr[1] = {2};
        GapMatcher m; m.configure(3600, 36, gi, gr, 1, 25);
        feed_gap(m, 36, {0}, 2, Pt, 5);
        CHECK(m.is_locked());
        CHECK(m.is_absolute());
        // current index 0 is the gap tooth → pitch 2×TA (200), or a normal 100 elsewhere
        CHECK(m.pitch() == 200 || m.pitch() == 100);
    }

    SECTION("a SINGLE tooth out of place drops the lock — position is proven or it is not");
    {
        uint8_t gi[1] = {0}, gr[1] = {2};
        GapMatcher m; m.configure(3600, 36, gi, gr, 1, 25);
        feed_gap(m, 36, {0}, 2, Pt, 5);                 // lock 36-1; ends at idx 34, next expects the gap
        CHECK(m.is_locked());
        m.feed(2 * Pt);                                 // the gap, exactly where the schedule says
        CHECK(!m.errored());
        CHECK(m.is_locked());

        // Next the schedule expects a NORMAL tooth (idx 1). Feed a gap-length interval there. This
        // used to be a "tolerated miss": reported, then the angle advanced by the pitch the matcher
        // had ASSUMED, and firing continued. The engine would then be up to a gap width out of
        // position with a severity-1 code and no cut, indefinitely, because nothing re-anchored and
        // the miss counter was cleared by the next conforming tooth. One tooth out of place means
        // the decoder does not know where the engine is.
        m.feed(2 * Pt);
        CHECK(m.errored());
        CHECK(!m.is_locked());                          // sync dropped, not ridden through
        // A long interval where a TOOTH was due is a tooth that did not arrive. GAP_MISMATCH is
        // reserved for the other direction — a normal interval where the GAP was scheduled, which
        // means the wheel is not the wheel the tune describes.
        CHECK(m.err_kind() == TriggerErrorKind::MISSED_TOOTH);
    }

    SECTION("an extra tooth in the gap is caught — a presence bit could not see it");
    {
        uint8_t gi[1] = {0}, gr[1] = {3};               // 60-2 style: the gap spans THREE pitches
        GapMatcher m; m.configure(3600, 60, gi, gr, 1, 25);
        feed_gap(m, 60, {0}, 3, Pt, 5);
        CHECK(m.is_locked());
        // A 2x interval where the 3x gap was scheduled — one of the missing teeth is present, or an
        // edge landed inside the gap. The old presence test asked only "is this >= 1.5x nominal",
        // so 2x and 3x were the same answer and this was invisible by construction.
        m.feed(2 * Pt);
        CHECK(m.errored());
        CHECK(!m.is_locked());
    }

    SECTION("ngap=3 → 36-2-2-2 multi-gap: absolute lock + correct angles (incl. close pair)");
    {
        const std::vector<int> gaps = {0, 1, 14};                     // inter-gap [1,13,16]
        uint8_t gi[3] = {0,1,14}, gr[3] = {3,3,3};
        GapMatcher m; m.configure(3600, 36, gi, gr, 3, 25);
        feed_gap(m, 36, gaps, 3, Pt, 6);
        CHECK(m.is_locked());
        CHECK(m.is_absolute());
        // invariant: angle == cumulative angle of the current present-index
        CHECK(m.angle() == ang_of(m.index(), gaps, 3, 100));
    }

    SECTION("multi-gap locks the same absolute frame from any starting rotation");
    {
        const std::vector<int> gaps = {0, 1, 14};
        uint8_t gi[3] = {0,1,14}, gr[3] = {3,3,3};
        for (int start : {0, 1, 7, 14, 22}) {
            GapMatcher m; m.configure(3600, 36, gi, gr, 3, 25);
            feed_gap(m, 36, gaps, 3, Pt, 6, start);
            CHECK(m.is_locked());
            CHECK(m.is_absolute());
            CHECK(m.angle() == ang_of(m.index(), gaps, 3, 100));
        }
    }

    SECTION("wrong wheel: a single-gap stream never matches a 3-gap config");
    {
        uint8_t gi[3] = {0,1,14}, gr[3] = {3,3,3};
        GapMatcher m; m.configure(3600, 36, gi, gr, 3, 25);
        feed_gap(m, 36, {0}, 2, Pt, 10);                             // 36-1 stream into 36-2-2-2 cfg
        CHECK(!m.is_locked());
    }

    SECTION("cam-rate gap wheel (period 7200) locks absolute");
    {
        // a cam-mounted 18-1 style wheel over 720°
        uint8_t gi[1] = {0}, gr[1] = {2};
        GapMatcher m; m.configure(7200, 18, gi, gr, 1, 25);
        // present = 17; gap tooth = 2×, tooth_angle = 7200/18 = 400
        for (int c = 0; c < 6; ++c) for (int i = 0; i < 17; ++i) m.feed((i==0?2:1)*Pt);
        CHECK(m.is_locked());
        CHECK(m.is_absolute());
    }

    SECTION("acceleration ramp holds the multi-gap lock");
    {
        const std::vector<int> gaps = {0, 1, 14};
        uint8_t gi[3] = {0,1,14}, gr[3] = {3,3,3};
        GapMatcher m; m.configure(3600, 36, gi, gr, 3, 25);
        feed_gap(m, 36, gaps, 3, Pt, 4);
        CHECK(m.is_locked());
        uint32_t p = Pt; const int P = present_of(36,3,3);
        for (int c = 0; c < 6; ++c) for (int i = 0; i < P; ++i) {
            m.feed((is_gap_i(gaps,i)?3:1)*p); p = (p*97)/100; if (p<50) p=50;
        }
        CHECK(m.is_locked());
    }

    SECTION("does not lock before the full gap sequence confirms");
    {
        uint8_t gi[3] = {0,1,14}, gr[3] = {3,3,3};
        GapMatcher m; m.configure(3600, 36, gi, gr, 3, 25);
        // one gap + a few normals: not enough to confirm 3-gap sequence
        m.feed(3*Pt); for (int i=0;i<3;i++) m.feed(Pt);
        CHECK(!m.is_locked());
    }

    return test_summary();
}
