// Standalone tests for the WidthMatcher primitive (generic-trigger). Identifies a reference
// cam pulse by angular width (time ÷ velocity), so it works on coarse cranks where the old
// crank-teeth count failed (6g72: 40° vs 85° both span 0–1 of a 120° tooth).

#include "test_helpers.h"
#include "WidthMatcher.h"

int main() {
    fprintf(stdout, "=== WidthMatcher (generic WIDTH primitive) ===\n");
    // The rate now arrives as a RATIO — the last crank tooth's period in ticks and its pitch in
    // decidegrees — so the matcher divides exactly once instead of consuming an already-truncated
    // ticks-per-decidegree. At 1200 rpm = 7200 deg/s, 0.1 deg takes 1e6/72000 = 13.888 us; a 10 deg
    // tooth is 1389 ticks. The old integer rate carried 13, losing 6.4% of every width.
    const uint32_t PER = 1389, PITCH = 100;          // one 10.0 deg tooth at 1200 rpm
    // ticks for a pulse of `deg10` decidegrees at that rate
    auto ticks = [](uint32_t deg10, uint32_t per, uint32_t pitch) { return deg10 * per / pitch; };

    SECTION("the unique wide pulse (85°) is the reference; narrow (40°) ignored");
    {
        WidthMatcher m; m.configure(/*min*/600, /*max*/1100, /*target*/7050);  // 60..110°
        m.feed(ticks(40*10, PER, PITCH), PER, PITCH);   // 40° pulse → ignored
        CHECK(!m.is_locked());
        m.feed(ticks(40*10, PER, PITCH), PER, PITCH);   // another 40° → ignored
        CHECK(!m.is_locked());
        m.feed(ticks(85*10, PER, PITCH), PER, PITCH);   // 85° pulse → reference
        CHECK(m.is_locked());
        CHECK(m.is_absolute());
        CHECK(m.angle() == 7050);
    }

    SECTION("works on a COARSE crank — width is time-based, not tooth-counted");
    {
        // Same 40° vs 85° pulses; a 120°/tooth crank would count both as 0–1 teeth (ambiguous),
        // but time/velocity distinguishes them cleanly.
        WidthMatcher m; m.configure(600, 1100, 3600);
        m.feed(ticks(40*10, PER, PITCH), PER, PITCH); CHECK(!m.is_locked());
        m.feed(ticks(85*10, PER, PITCH), PER, PITCH); CHECK(m.is_locked());
        CHECK(m.angle() == 3600);
    }

    SECTION("velocity-invariant: same angular width at a different RPM still matches");
    {
        WidthMatcher m; m.configure(600, 1100, 1000);
        const uint32_t PER2 = 2778;   // half RPM → twice the ticks per tooth
        m.feed(ticks(85*10, PER2, PITCH), PER2, PITCH); // 85° at the slower speed
        CHECK(m.is_locked());
    }

    SECTION("a width just outside the window is rejected");
    {
        WidthMatcher m; m.configure(600, 1100, 0);
        m.feed(ticks(115*10, PER, PITCH), PER, PITCH);  // 115° > 110° max
        CHECK(!m.is_locked());
        m.feed(ticks(55*10, PER, PITCH), PER, PITCH);   // 55° < 60° min
        CHECK(!m.is_locked());
    }

    SECTION("a zero period is ignored (no divide-by-zero, no false lock)");
    {
        WidthMatcher m; m.configure(600, 1100, 0);
        m.feed(ticks(85*10, PER, PITCH), 0, PITCH);
        CHECK(!m.is_locked());
    }

    SECTION("EXACT at a rate that does not divide evenly — the truncation this replaced");
    {
        // 8000 rpm on a 180-slit Nissan: a 2.0 deg tooth is 41.67 ticks, so the true rate is 2.083
        // ticks per 0.1 deg. The old API took that as an integer 2, and an 80.0 deg pulse measured
        // 833 dd instead of 800 — 3.3 deg of error that landed in last_width_, which edge_angle()
        // adds to the anchor. Worst on exactly the fine wheels whose cam anchors an even crank.
        const uint32_t PER8 = 42, PITCH8 = 20;       // ~41.67 ticks per 2.0 deg tooth
        WidthMatcher m; m.configure(600, 1100, 0);
        const uint32_t w80 = 800u * PER8 / PITCH8;   // an 80.0 deg pulse in ticks
        m.feed(w80, PER8, PITCH8);
        CHECK(m.is_locked());
        // Within a decidegree of 80.0, against 83.3 before. The residue is the tick quantisation of
        // the stimulus itself, not the matcher's arithmetic.
        CHECK(m.edge_angle() >= 795 && m.edge_angle() <= 805);
    }

    SECTION("a pulse hundreds of pitches long is refused rather than overflowing");
    {
        WidthMatcher m; m.configure(600, 1100, 0);
        m.feed(0xFFFFFFFFu, 100, 100);               // garbage width
        CHECK(!m.is_locked());
    }

    return test_summary();
}
