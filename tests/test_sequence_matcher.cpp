// Standalone tests for the SequenceMatcher primitive (generic-trigger step 1). No decoder,
// no MCU — feed event periods directly. Covers the three streams it must subsume: uneven
// crank teeth (odd-fire), a repeated-value cell (GM 4200), and cam-pulse intervals (6g72),
// plus uniform→relative, any-rotation start, accel, and noise recovery.

#include "test_helpers.h"
#include "SequenceMatcher.h"
#include <vector>
#include <cstdint>

static AngleDeg10 angle_of(const std::vector<AngleDeg10>& cell, int i) {
    AngleDeg10 a = 0; for (int j = 0; j < i; ++j) a = static_cast<AngleDeg10>(a + cell[j]); return a;
}
static int wrap(int i, int n) { int m = i % n; if (m < 0) m += n; return m; }

// Feed `cycles` cycles of the cell at constant velocity (period = angle*k), starting at
// cell index `start`. Returns the matcher (configured).
static void feed_cell(SequenceMatcher& m, const std::vector<AngleDeg10>& cell,
                      int cycles, uint32_t k = 10, int start = 0) {
    const int n = static_cast<int>(cell.size());
    for (int e = 0; e < cycles * n; ++e)
        m.feed(static_cast<uint32_t>(cell[wrap(start + e, n)]) * k);
}

// After lock, the invariant must hold: angle == A(idx), pitch == cell[idx-1].
static bool invariant(SequenceMatcher& m, const std::vector<AngleDeg10>& cell) {
    const int n = static_cast<int>(cell.size());
    return m.angle() == angle_of(cell, m.index())
        && m.pitch() == cell[wrap(m.index() - 1, n)];
}

int main() {
    fprintf(stdout, "=== SequenceMatcher (generic SEQUENCE primitive) ===\n");

    SECTION("uneven odd-fire cell [1350,2250] locks ABSOLUTE, invariant holds");
    {
        std::vector<AngleDeg10> cell = {1350, 2250};
        SequenceMatcher m; m.configure(cell.data(), 2, 25);
        feed_cell(m, cell, 6);
        CHECK(m.is_locked());
        CHECK(m.is_absolute());
        CHECK(invariant(m, cell));
        CHECK(m.period() == 3600);
    }

    SECTION("a LOCKED window violation DROPS THE LOCK — position is proven or it is not");
    {
        std::vector<AngleDeg10> cell = {1350, 2250};
        SequenceMatcher m; m.configure(cell.data(), 2, 25);
        feed_cell(m, cell, 6);                                 // lock on clean, steady periods
        CHECK(m.is_locked());
        CHECK(!m.errored());                                   // last clean period → no error
        m.feed(99999);                                         // grossly off → predicted-window violation
        CHECK(m.errored());
        // It used to tolerate four of these and keep the lock, which is the ride-through the gap
        // path had already dropped: an event out of place means the decoder cannot say where the
        // engine is, and carrying on advances the pattern on an assumption. One is enough.
        CHECK(!m.is_locked());
    }

    SECTION("uniform cell [1800,1800] locks RELATIVE (no false absolute)");
    {
        std::vector<AngleDeg10> cell = {1800, 1800};
        SequenceMatcher m; m.configure(cell.data(), 2, 25);
        feed_cell(m, cell, 6);
        CHECK(m.is_locked());
        CHECK(!m.is_absolute());
        CHECK(m.pitch() == 1800);
    }

    SECTION("GM 4200 repeated-value cell [500,600,600,100,500,600,700] locks ABSOLUTE");
    {
        std::vector<AngleDeg10> cell = {500, 600, 600, 100, 500, 600, 700};
        SequenceMatcher m; m.configure(cell.data(), 7, 25);
        feed_cell(m, cell, 8);
        CHECK(m.is_locked());
        CHECK(m.is_absolute());
        CHECK(invariant(m, cell));
    }

    SECTION("6g72 cam-pulse intervals [1900,1700,1950,1650] (period 7200) lock ABSOLUTE");
    {
        std::vector<AngleDeg10> cell = {1900, 1700, 1950, 1650};   // Σ = 7200 (cam-rate)
        SequenceMatcher m; m.configure(cell.data(), 4, 20);
        feed_cell(m, cell, 8);
        CHECK(m.is_locked());
        CHECK(m.is_absolute());
        CHECK(invariant(m, cell));
        CHECK(m.period() == 7200);
    }

    SECTION("locks to the SAME absolute index regardless of starting rotation");
    {
        std::vector<AngleDeg10> cell = {500, 600, 600, 100, 500, 600, 700};
        // Reference: feed from start 0, note the angle after a fixed number of post-lock events.
        // For each start offset, after feeding whole cycles the current event must map to a
        // valid cell angle and satisfy the invariant (absolute frame independent of start).
        for (int start : {0, 1, 3, 4, 6}) {
            SequenceMatcher m; m.configure(cell.data(), 7, 25);
            feed_cell(m, cell, 8, 10, start);
            CHECK(m.is_locked());
            CHECK(m.is_absolute());
            CHECK(invariant(m, cell));
        }
    }

    SECTION("acceleration ramp (ratios preserved) holds the lock");
    {
        std::vector<AngleDeg10> cell = {1350, 2250};
        SequenceMatcher m; m.configure(cell.data(), 2, 25);
        feed_cell(m, cell, 4);
        CHECK(m.is_locked());
        uint32_t k = 10000;                         // Q-ish scaled base (×1000)
        for (int c = 0; c < 10; ++c)
            for (int i = 0; i < 2; ++i) {
                m.feed(static_cast<uint32_t>(cell[i]) * k / 1000);
                k = (k * 97) / 100; if (k < 1000) k = 1000;   // accelerate ~3%/event
            }
        CHECK(m.is_locked());
        CHECK(invariant(m, cell));
    }

    SECTION("a single garbage period does not drop a held lock");
    {
        std::vector<AngleDeg10> cell = {1350, 2250};
        SequenceMatcher m; m.configure(cell.data(), 2, 25);
        feed_cell(m, cell, 5);
        CHECK(m.is_locked());
        // one wild period (a dropout), then resume the clean stream
        m.feed(9999);                               // way out of window — one miss
        feed_cell(m, cell, 5);
        CHECK(m.is_locked());                        // single glitch tolerated
        CHECK(invariant(m, cell));
    }

    SECTION("sustained garbage drops the lock (does not freewheel on noise)");
    {
        std::vector<AngleDeg10> cell = {1350, 2250};
        SequenceMatcher m; m.configure(cell.data(), 2, 25);
        feed_cell(m, cell, 5);
        CHECK(m.is_locked());
        // alternate wildly out-of-window periods for several events
        for (int i = 0; i < SequenceMatcher::MAX_MISSES + 3; ++i)
            m.feed((i & 1) ? 200u : 40000u);
        CHECK(!m.is_locked());                       // dropped, will re-acquire
    }

    SECTION("does not lock before enough confirming events");
    {
        std::vector<AngleDeg10> cell = {1350, 2250};
        SequenceMatcher m; m.configure(cell.data(), 2, 25);
        m.feed(13500); m.feed(22500);               // only one ratio observed
        CHECK(!m.is_locked());
    }

    return test_summary();
}
