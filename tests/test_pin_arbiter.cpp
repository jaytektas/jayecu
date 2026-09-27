// Host unit test for PinArbiter — one pool, one index space (a row IS a pin). The tune can no longer ask
// for two owners on one pin, so what is pinned here is what still can happen: a pin held by one owner is
// refused to another (a row changed from a coil to a generic output before the scheduler has let go),
// release_owner forces exactly that owner's pins back to Hi-Z, and nothing outside the pool is granted.
//   build: tests/CMakeLists.txt -> ctest -R pin_arbiter
#include "test_helpers.h"
#include "Scheduler/PinArbiter.h"

// Minimal ITimerChannel: identity + a disable_output (Hi-Z) counter so we can assert the release.
class FakeCh final : public ITimerChannel {
public:
    int hiz_calls = 0;
    uint32_t get_current_ticks()    const noexcept override { return 0; }
    uint32_t get_ticks_per_second() const noexcept override { return 1; }
    void force_output_now(OutputAction) noexcept override {}
    void disable_output() noexcept override { ++hiz_calls; }
};

int main() {
    fprintf(stdout, "=== PinArbiter ===\n");

    // Rows 0-1 coils, 2-3 low-side, 4-5 high-side.
    FakeCh p[6];
    ITimerChannel* pool[6] = { &p[0], &p[1], &p[2], &p[3], &p[4], &p[5] };
    PinArbiter arb;
    arb.bind(pool, 6);

    SECTION("a free row is granted to its owner and returns that row's channel");
    {
        CHECK(arb.claim(0, PinOwner::IGNITION) == &p[0]);
        CHECK(arb.claim(2, PinOwner::INJECTION) == &p[2]);
        CHECK(arb.claim(4, PinOwner::AUX) == &p[4]);
        CHECK(arb.owner_of(0) == PinOwner::IGNITION);
        CHECK(!arb.has_conflict());
    }

    SECTION("re-claiming a row you already own is fine");
    {
        CHECK(arb.claim(0, PinOwner::IGNITION) == &p[0]);
        CHECK(!arb.has_conflict());
    }

    SECTION("a row another owner holds is refused and the conflict names both");
    {
        CHECK(arb.claim(0, PinOwner::AUX) == nullptr);
        CHECK(arb.has_conflict());
        CHECK(arb.conflict_denied() == PinOwner::AUX);
        CHECK(arb.conflict_holder() == PinOwner::IGNITION);
    }

    SECTION("release_owner forces only that owner's pins Hi-Z and frees them");
    {
        arb.clear_conflict();
        const int c0 = p[0].hiz_calls, c2 = p[2].hiz_calls, c4 = p[4].hiz_calls;
        arb.release_owner(PinOwner::IGNITION);
        CHECK(p[0].hiz_calls == c0 + 1);
        CHECK(p[2].hiz_calls == c2);            // the injector is untouched
        CHECK(p[4].hiz_calls == c4);            // …and so is the generic output
        CHECK(arb.owner_of(0) == PinOwner::FREE);
        CHECK(arb.claim(0, PinOwner::AUX) == &p[0]);   // freed -> claimable by the row's new function
        CHECK(arb.claim(2, PinOwner::AUX) == nullptr); // still the injector's
    }

    SECTION("a row outside the pool is refused, not silently granted");
    {
        arb.clear_conflict();
        CHECK(arb.claim(6, PinOwner::AUX) == nullptr);
        CHECK(arb.claim(200, PinOwner::AUX) == nullptr);
        CHECK(!arb.has_conflict());             // not a conflict: there is no such pin
    }

    return test_summary();
}
