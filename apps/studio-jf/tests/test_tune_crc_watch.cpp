// test_tune_crc_watch.cpp — when does a moving tune CRC mean "re-read the ECU's configuration"?
//
// A rusEFI ECU edits its own tune: "Grab Idle/Up" writes the pedal's ADC reading into the calibration,
// an ETB autotune stores what it learned, a Lua script sets a value. The protocol announces none of it.
// What the firmware publishes is a CRC over the whole tune, so a change in it means the studio's copy is
// stale — and a stale copy is not merely a display problem, because the next burn writes it back over
// what the ECU worked out.
//
// The rules are worth testing on their own: a re-read at the wrong moment is as harmful as a missed one.
//
//   cmake --build build --target tune_crc_watch_test && ./build/tune_crc_watch_test

#include "comms/TsCacheBridge.h"

#include <cstdio>

using A = TsCacheBridge::JCrcAction;

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL (line %d): %s\n", __LINE__, #cond); ++g_fails; } } while (0)

int main() {
    // The first frame has nothing to compare against: adopt it, never treat it as a change.
    CHECK(TsCacheBridge::crcAction(false, 0,     0x1234, false, 999999) == A::Adopt);
    CHECK(TsCacheBridge::crcAction(false, 0x1234, 0x1234, false, 999999) == A::Adopt);

    // Steady state: the overwhelmingly common case, and it must cost nothing.
    CHECK(TsCacheBridge::crcAction(true, 0x1234, 0x1234, false, 999999) == A::Ignore);

    // The ECU changed its own tune, with nothing of ours in flight → re-read.
    CHECK(TsCacheBridge::crcAction(true, 0x1234, 0x9999, false, 999999) == A::Reload);

    // Our own edits move the CRC too. Within the write window it is ours: adopt the new value and
    // carry on, rather than spending a full config read to be told what we just wrote.
    CHECK(TsCacheBridge::crcAction(true, 0x1234, 0x9999, false, 10)   == A::Adopt);
    CHECK(TsCacheBridge::crcAction(true, 0x1234, 0x9999, false, 3999) == A::Adopt);
    CHECK(TsCacheBridge::crcAction(true, 0x1234, 0x9999, false, 4001) == A::Reload);   // window elapsed

    // Writes still queued: a re-read here would overwrite bytes the user has typed but we have not
    // flushed. Leave it entirely — the next frame reconsiders, and the CRC is not adopted, so the
    // change is not forgotten either.
    CHECK(TsCacheBridge::crcAction(true, 0x1234, 0x9999, true, 999999) == A::Ignore);
    CHECK(TsCacheBridge::crcAction(true, 0x1234, 0x9999, true, 10)     == A::Ignore);

    // A CRC that moves and then moves back (any 16-bit value can recur) is still a change from what we
    // last adopted, so it is not missed.
    CHECK(TsCacheBridge::crcAction(true, 0x9999, 0x1234, false, 999999) == A::Reload);

    std::printf(g_fails ? "tune crc watch: FAILED (%d)\n" : "tune crc watch: OK\n", g_fails);
    return g_fails ? 1 : 0;
}
