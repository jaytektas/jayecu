#include "test_helpers.h"
#include "../firmware/Signal/SignalBus.h"
#include "../generated/signal_ids.h"
#include <cstring>

int main() {
    fprintf(stdout, "=== SignalBus ===\n");

    SECTION("fresh bus — all slots invalid");
    {
        SignalBus bus{};
        for (uint16_t i = 0; i < SIG_COUNT; i++) {
            CHECK(!bus.valid(static_cast<SignalId>(i)));
            CHECK(bus.get(static_cast<SignalId>(i), -1.0f) == -1.0f);
        }
    }

    SECTION("set/get round-trip");
    {
        SignalBus bus{};
        bus.set(SIG_MAP, 101.3f);
        CHECK(bus.valid(SIG_MAP));
        CHECK_NEAR(bus.get(SIG_MAP), 101.3f, 0.001f);
    }

    SECTION("get returns fallback when invalid");
    {
        SignalBus bus{};
        CHECK(bus.get(SIG_CLT, 20.0f) == 20.0f);
        bus.set(SIG_CLT, 80.0f);
        CHECK_NEAR(bus.get(SIG_CLT, 20.0f), 80.0f, 0.001f);
    }

    SECTION("set_bool / get_bool");
    {
        SignalBus bus{};
        bus.set_bool(SIG_CLUTCH_SW, true);
        CHECK(bus.get_bool(SIG_CLUTCH_SW));
        CHECK_NEAR(bus.get(SIG_CLUTCH_SW), 1.0f, 0.001f);

        bus.set_bool(SIG_CLUTCH_SW, false);
        CHECK(!bus.get_bool(SIG_CLUTCH_SW));
        CHECK_NEAR(bus.get(SIG_CLUTCH_SW), 0.0f, 0.001f);
    }

    SECTION("invalidate clears valid flag");
    {
        SignalBus bus{};
        bus.set(SIG_IAT, 25.0f);
        CHECK(bus.valid(SIG_IAT));
        bus.invalidate(SIG_IAT);
        CHECK(!bus.valid(SIG_IAT));
        CHECK(bus.get(SIG_IAT, -99.0f) == -99.0f);
    }

    SECTION("age_ms reflects set_at_ms");
    {
        SignalBus bus{};
        bus.set(SIG_BATTERY, 12.6f, true, 1000u);
        CHECK(bus.age_ms(SIG_BATTERY, 1050u) == 50u);
        CHECK(bus.age_ms(SIG_BATTERY, 1000u) == 0u);
    }

    SECTION("expire_stale ages out slots past their ttl");
    {
        SignalBus bus{};
        bus.set(SIG_MAP, 100.0f, true, 1000u, 200u);   // ttl 200ms
        bus.set(SIG_CLT,    80.0f, true, 1000u, 0u);      // ttl 0 = never expire
        bus.expire_stale(1150u);                          // +150ms
        CHECK(bus.valid(SIG_MAP));                      // within ttl
        bus.expire_stale(1300u);                          // +300ms > ttl
        CHECK(!bus.valid(SIG_MAP));                     // expired
        CHECK(bus.valid(SIG_CLT));                        // ttl 0 never expires

        // A co-writer that refreshes the slot resets the age, so it never expires.
        bus.set(SIG_MAP, 101.0f, true, 1290u, 200u);   // refreshed just before
        bus.expire_stale(1300u);                          // age 10ms < ttl
        CHECK(bus.valid(SIG_MAP));
    }

    SECTION("a ttl of N ms is N ms, not N+1");
    {
        // The frame ages the bus after its reads, so a value set with ttl 5 at t must be gone by the
        // sweep at t+5 — or it is still read in that frame, and a one-decision soft cut holds 6 frames.
        SignalBus bus{};
        bus.set_bool(SIG_FUEL_CUT, true, 1000u, 5u);
        bus.expire_stale(1004u);                          // age 4 < 5: still live
        CHECK(bus.valid(SIG_FUEL_CUT));
        bus.expire_stale(1005u);                          // age 5: expired
        CHECK(!bus.valid(SIG_FUEL_CUT));
    }

    SECTION("clear resets all slots to invalid");
    {
        SignalBus bus{};
        bus.set(SIG_MAP, 100.0f);
        bus.set(SIG_CLT,   80.0f);
        bus.clear();
        CHECK(!bus.valid(SIG_MAP));
        CHECK(!bus.valid(SIG_CLT));
        CHECK(bus.get(SIG_MAP, 0.0f) == 0.0f);
    }

    SECTION("out-of-range id (SIG_NONE) is safe");
    {
        SignalBus bus{};
        bus.set(SIG_NONE, 1.0f);  // should be no-op
        CHECK(!bus.valid(SIG_NONE));
        CHECK(bus.get(SIG_NONE, 5.0f) == 5.0f);
    }

    SECTION("id_by_name -- known names found");
    {
        CHECK(SignalBus::id_by_name("map")      == SIG_MAP);
        CHECK(SignalBus::id_by_name("clt")        == SIG_CLT);
        CHECK(SignalBus::id_by_name("lambda_1")   == SIG_LAMBDA_1);
        CHECK(SignalBus::id_by_name("gear")       == SIG_GEAR);
        CHECK(SignalBus::id_by_name("boost_est")  == SIG_BOOST_EST);
        CHECK(SignalBus::id_by_name("vehicle_spd")== SIG_VEHICLE_SPD);
    }

    SECTION("id_by_name -- unknown name returns SIG_NONE");
    {
        CHECK(SignalBus::id_by_name("no_such_signal") == SIG_NONE);
        CHECK(SignalBus::id_by_name("")               == SIG_NONE);
    }

    SECTION("get_by_name / set_by_name");
    {
        SignalBus bus{};
        CHECK(bus.set_by_name("wheel_fl", 123.4f));
        CHECK_NEAR(bus.get_by_name("wheel_fl"), 123.4f, 0.01f);

        // Unknown name returns false / fallback
        CHECK(!bus.set_by_name("nonexistent", 0.0f));
        CHECK(bus.get_by_name("nonexistent", 99.0f) == 99.0f);
    }

    SECTION("multiple independent slots don't alias");
    {
        SignalBus bus{};
        bus.set(SIG_MAP, 100.0f);
        bus.set(SIG_TPS, 200.0f);
        bus.set(SIG_CLT,    80.0f);
        CHECK_NEAR(bus.get(SIG_MAP), 100.0f, 0.001f);
        CHECK_NEAR(bus.get(SIG_TPS), 200.0f, 0.001f);
        CHECK_NEAR(bus.get(SIG_CLT),    80.0f, 0.001f);
    }

    SECTION("valid flag is per-slot, set independently");
    {
        SignalBus bus{};
        bus.set(SIG_TPS, 50.0f, false);  // write with valid=false
        CHECK(!bus.valid(SIG_TPS));
        CHECK(bus.get(SIG_TPS, 0.0f) == 0.0f);  // fallback since invalid

        bus.set(SIG_TPS, 50.0f, true);
        CHECK(bus.valid(SIG_TPS));
        CHECK_NEAR(bus.get(SIG_TPS, 0.0f), 50.0f, 0.001f);
    }

    SECTION("typed u32 / i32 cell — lossless integer round-trip");
    {
        SignalBus bus{};
        // A value above 2^24 cannot be held exactly by float; the int cell must be.
        const uint32_t big = 0x0A1B2C3Du;                       // 169,090,621
        bus.set_u32(SIG_DTC_INDICATORS, big, true, 500u, 100u);
        CHECK(bus.valid(SIG_DTC_INDICATORS));
        CHECK(bus.get_u32(SIG_DTC_INDICATORS) == big);           // exact — no float rounding
        CHECK(bus.age_ms(SIG_DTC_INDICATORS, 540u) == 40u);      // shares freshness machinery

        bus.set_i32(SIG_DIFF_OIL_TEMP, -12345, true, 500u);
        CHECK(bus.get_i32(SIG_DIFF_OIL_TEMP) == -12345);

        // invalid → typed fallback; an independent valid slot is unaffected
        bus.invalidate(SIG_DTC_INDICATORS);
        CHECK(bus.get_u32(SIG_DTC_INDICATORS, 7u) == 7u);
        CHECK(bus.get_i32(SIG_DIFF_OIL_TEMP, -1) == -12345);

        // expiry works on a typed slot too
        bus.set_u32(SIG_DTC_INDICATORS, big, true, 1000u, 200u);
        bus.expire_stale(1300u);                               // age 300 > ttl 200
        CHECK(!bus.valid(SIG_DTC_INDICATORS));

        // out-of-range id is safe for the typed setters/getters
        bus.set_u32(SIG_NONE, 1u);
        CHECK(bus.get_u32(SIG_NONE, 9u) == 9u);
    }

    // ---- TYPE-ERASED access must consult the catalog, not assume float --------------------------------
    // A cell is a union: reading .f from an integer channel REINTERPRETS its bits rather than converting
    // them, so a by-name reader (Lua's signalRead, the CAN plumbing) that always called get() saw garbage
    // for every u32-typed channel — a count of 1000 read back as a denormal near 1e-42, not as 1000.
    SECTION("get_typed / by-name honour the channel's cell type");
    {
        SignalBus bus;
        bus.clear();

        // A u32-typed channel. dtc_indicators is a BITMASK, which makes the failure vivid: reinterpreted
        // as a float its bits mean nothing, and rounding one would light the wrong warning lamps.
        CHECK(SIGNAL_TYPES[SIG_DTC_INDICATORS] == SIG_T_U32);
        bus.set_u32(SIG_DTC_INDICATORS, 1000u);
        CHECK(bus.get_u32(SIG_DTC_INDICATORS) == 1000u);
        CHECK_NEAR(bus.get_typed(SIG_DTC_INDICATORS), 1000.0f, 0.001f);   // converts...
        CHECK(bus.get(SIG_DTC_INDICATORS) < 1.0f);                        // ...where get() reinterprets

        // A float channel is unaffected.
        bus.set(SIG_MAP, 101.3f);
        CHECK_NEAR(bus.get_typed(SIG_MAP), 101.3f, 0.001f);

        // By name, both directions: a write must land in the cell the consumer reads.
        CHECK(bus.set_by_name("dtc_indicators", 12345.0f));
        CHECK(bus.get_u32(SIG_DTC_INDICATORS) == 12345u);                 // stored as an INTEGER cell
        CHECK_NEAR(bus.get_by_name("dtc_indicators"), 12345.0f, 0.001f);

        CHECK(bus.set_by_name("map", 55.5f));
        CHECK_NEAR(bus.get(SIG_MAP), 55.5f, 0.001f);
        CHECK_NEAR(bus.get_by_name("map"), 55.5f, 0.001f);

        // An invalid slot still falls back rather than reinterpreting whatever was left there.
        bus.clear();
        CHECK_NEAR(bus.get_typed(SIG_DTC_INDICATORS, -1.0f), -1.0f, 0.001f);

        // WRITER side: a producer reaching for the wrong accessor is the same bug in reverse. set() on a
        // channel the catalog declares integer must CONVERT into the right cell, not write float bits into
        // one that consumers read as .u.
        bus.set(SIG_DTC_INDICATORS, 300.0f);
        CHECK(bus.get_u32(SIG_DTC_INDICATORS) == 300u);
        CHECK_NEAR(bus.get_typed(SIG_DTC_INDICATORS), 300.0f, 0.001f);

        // ...and the float path is untouched by that routing.
        bus.set(SIG_MAP, 42.5f);
        CHECK_NEAR(bus.get(SIG_MAP), 42.5f, 0.001f);
    }

    return test_summary();
}
