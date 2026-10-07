// Host unit test for the SignalBus priority gate — the override-bus primitive that lets Lua keep its
// "last word" by precedence (so it can move off the engine task) instead of temporal order.
// Gate rule: a write at `prio` lands iff prio >= slot.prio OR the slot is stale (invalid / aged past
// ttl). ttl == 0 latches (fail-closed). get() never consults prio/ttl — freshness is a writer's job.
//   build: tests/CMakeLists.txt -> ctest -R signal_priority
#include "test_helpers.h"
#include "Signal/SignalBus.h"

int main() {
    fprintf(stdout, "=== SignalBus priority gate ===\n");
    const SignalId X = static_cast<SignalId>(0);   // any valid slot

    SECTION("backward compatible: equal/base priority is last-writer-wins");
    {
        SignalBus bus;
        bus.set(X, 1.0f);                          // default PRIO_BASE
        CHECK_NEAR(bus.get(X), 1.0f, 1e-6);
        bus.set(X, 2.0f);                          // equal prio overwrites, as today
        CHECK_NEAR(bus.get(X), 2.0f, 1e-6);
    }

    SECTION("a fresh higher-priority override out-votes the base");
    {
        SignalBus bus;
        bus.set(X, 1.0f, true, /*now*/0, /*ttl*/100, PRIO_BASE);
        bus.set(X, 9.0f, true, 0, 100, PRIO_LUA);          // override lands
        CHECK_NEAR(bus.get(X), 9.0f, 1e-6);
        bus.set(X, 2.0f, true, 0, 100, PRIO_BASE);         // base blocked while override fresh
        CHECK_NEAR(bus.get(X), 9.0f, 1e-6);
    }

    SECTION("override expires by ttl -> the continuous base reclaims");
    {
        SignalBus bus;
        bus.set(X, 9.0f, true, /*now*/0,  /*ttl*/10, PRIO_LUA);   // override @ t=0
        bus.set(X, 2.0f, true, /*now*/5,  10, PRIO_BASE);         // t=5: still fresh -> blocked
        CHECK_NEAR(bus.get(X), 9.0f, 1e-6);
        bus.set(X, 3.0f, true, /*now*/20, 10, PRIO_BASE);         // t=20: override stale -> reclaimed
        CHECK_NEAR(bus.get(X), 3.0f, 1e-6);
    }

    SECTION("latch (ttl=0) is fail-closed: held through base writes until an equal/higher release");
    {
        SignalBus bus;
        bus.set(X, 9.0f, true, /*now*/0, /*ttl*/0, PRIO_LUA);     // latched override (a 'limit')
        for (uint32_t t = 1; t < 5000; t += 1000)
            bus.set(X, 1.0f, true, t, 50, PRIO_BASE);             // base can't release it — dead-script safe
        CHECK_NEAR(bus.get(X), 9.0f, 1e-6);
        // release = one equal-priority short-ttl write, then stop and let the base reclaim
        bus.set(X, 9.0f, true, /*now*/6000, /*ttl*/1, PRIO_LUA);  // overwrites the latch (ttl 0 -> 1)
        bus.set(X, 1.0f, true, /*now*/6002, 50, PRIO_BASE);       // now stale -> base reclaims
        CHECK_NEAR(bus.get(X), 1.0f, 1e-6);
    }

    SECTION("an invalidated slot is reclaimable by anyone");
    {
        SignalBus bus;
        bus.set(X, 9.0f, true, 0, /*ttl*/0, PRIO_LUA);   // latched override
        bus.invalidate(X);                                // dropped
        bus.set(X, 4.0f, true, 0, 0, PRIO_BASE);          // invalid -> stale -> base accepted
        CHECK_NEAR(bus.get(X), 4.0f, 1e-6);
    }

    SECTION("get() ignores priority and ttl — the reader stays a single-word read");
    {
        SignalBus bus;
        bus.set(X, 7.0f, true, 0, /*ttl*/5, PRIO_LUA);
        CHECK_NEAR(bus.get(X, -1.0f), 7.0f, 1e-6);        // value is there regardless of ttl
        CHECK(bus.valid(X) == true);
        // only the writer-side sweep (expire_stale) flips validity for a truly-dead signal:
        bus.expire_stale(/*now*/1000);                    // 1000 > set_at(0)+ttl(5)
        CHECK(bus.valid(X) == false);
    }

    // ---------------------------------------------------------------------------------------------
    // THE REGRESSION THESE EXIST FOR. Every case above passes a timestamp. The engine task did not:
    // set()'s now_ms defaults to 0, and accept_() subtracted that 0 from the stored one to age the
    // value already in the slot. Unsigned, 0 - 100000 is 4 294 867 296, not "before" — so every live
    // override looked ~50 days old, was judged stale, and a base write walked over it. Twenty-two
    // call sites did that, sixteen in EngineTask, which is why rpm could not be overridden while a
    // channel nobody published could. The cases below all use a realistic uptime, because the bug
    // cannot appear while set_at_ms is 0.
    // ---------------------------------------------------------------------------------------------
    const uint32_t T = 100000;

    SECTION("an UNTIMED base write does not defeat a live override");
    {
        SignalBus bus;
        bus.set(X, 9.0f, true, T, 500, PRIO_LUA);
        bus.set(X, 1.0f);                                  // written the way EngineTask wrote rpm
        CHECK_NEAR(bus.get(X), 9.0f, 1e-6);
    }

    SECTION("…through the typed setters too, which share accept_()");
    {
        SignalBus bus;
        const SignalId U = SIG_TRIGGER_TEETH;              // a declared-integer channel
        bus.set_u32(U, 4242u, true, T, 500, PRIO_LUA);
        bus.set_u32(U, 0u);
        CHECK(bus.get_u32(U, 0u) == 4242u);
    }

    SECTION("an untimed write still lands on a channel nobody has published");
    {
        // What the missing timestamp was wrongly credited with expressing. "Never published" is
        // carried by valid, which is tested first and short-circuits — so ignoring an absent
        // timestamp takes no meaning away.
        SignalBus bus;
        CHECK(!bus.valid(X));
        bus.set(X, 1234.0f);
        CHECK_NEAR(bus.get(X), 1234.0f, 1e-6);
    }

    SECTION("a cut channel takes only a TRUE: a script cannot write a cut away");
    {
        // fuel_cut / ign_cut hold a cut while a requester writes true. A Lua or CAN write of 0 at a
        // higher priority used to take the slot and out-vote the rev limiter's cut; now it is dropped.
        SignalBus bus;
        bus.set(SIG_FUEL_CUT, 0.0f, true, T, 0, PRIO_LUA);     // a script "clearing" the cut
        CHECK(!bus.valid(SIG_FUEL_CUT));                       // dropped: not a cut, holds nothing
        bus.set_bool(SIG_FUEL_CUT, true, T, 5);                // the rev limiter asks for one
        CHECK(bus.get_bool(SIG_FUEL_CUT));                     // and gets it
        bus.set(SIG_IGN_CUT, 1.0f, true, T, 0, PRIO_LUA);      // a script may ADD a cut
        CHECK(bus.get_bool(SIG_IGN_CUT));
        bus.set(SIG_IGN_CUT, 0.0f, false, T, 0, PRIO_LUA);     // ...an invalid write does not end it either
        CHECK(bus.get_bool(SIG_IGN_CUT));
    }

    SECTION("an EXPIRED override is still reclaimed by an untimed writer");
    {
        // The other half of the contract: the fix must not let an override hold a slot for ever.
        // expire_stale() invalidates it, and !valid lets the base back in with or without a tick.
        SignalBus bus;
        bus.set(X, 9.0f, true, T, 500, PRIO_LUA);
        bus.expire_stale(T + 501);
        CHECK(bus.valid(X) == false);
        bus.set(X, 1.0f);
        CHECK_NEAR(bus.get(X), 1.0f, 1e-6);
    }

    return test_summary();
}
