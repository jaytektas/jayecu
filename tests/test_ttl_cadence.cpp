#include "test_helpers.h"
#include "../firmware/Engine/EngineModule.h"

// Locks ttl_for(): a producer's freshness ttl is period + grace (additive), where grace is an ABSOLUTE
// frame-slip floor OR half a period at low rates — NOT period × a fixed multiplier. So a fast signal and a
// slow signal each get ~their own period + a little slack, instead of one lingering 100× longer than the
// other. This was the design bug we fixed: uniform ×5 gave 5 ms vs 500 ms.

int main() {
    SECTION("ttl_for is period + grace, not period × N");
    CHECK(ttl_for(1000) == 5);     // period 1  + max(4, 0)  = 5   (== TTL_FRAME_MS)
    CHECK(ttl_for(500)  == 6);     // period 2  + max(4, 1)  = 6
    CHECK(ttl_for(200)  == 9);     // period 5  + max(4, 2)  = 9
    CHECK(ttl_for(100)  == 15);    // period 10 + max(4, 5)  = 15
    CHECK(ttl_for(50)   == 30);    // period 20 + max(4,10)  = 30
    CHECK(ttl_for(30)   == 49);    // period 33 + max(4,16)  = 49  (lambda)
    CHECK(ttl_for(20)   == 75);    // period 50 + max(4,25)  = 75  (egt_protect)
    CHECK(ttl_for(10)   == 150);   // period 100 + 50        = 150
    CHECK(ttl_for(1)    == 1500);  // period 1000 + 500      = 1500

    SECTION("hz==0 never expires (persistent signals)");
    CHECK(ttl_for(0) == 0);

    SECTION("fail-safe latency stays ~1 period, not a fixed multiple");
    // A 10 Hz signal's ttl is ~1.5× its period, same ratio as 50/100 Hz — the fast/slow ratio of the
    // *multiplier* is bounded, unlike the old ×5 (which made 10 Hz = 500 ms = 100× the 1 kHz window).
    CHECK(ttl_for(10) <= 2 * (1000 / 10));
    CHECK(ttl_for(50) <= 2 * (1000 / 50));
    return test_summary();
}
