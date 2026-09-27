#include "test_helpers.h"
#include "../firmware/Scheduler/EngineSyncSampler.h"

// The sampler's only platform dependency: the DMA-filtered ADC read. Drive it from a table so we
// can feed a known per-tooth sequence and check the window average.
static uint16_t g_ain[64];
extern "C" uint16_t platform_read_ain_raw(uint8_t pin) { return pin < 64 ? g_ain[pin] : 0; }

int main() {
    fprintf(stdout, "=== EngineSyncSampler ===\n");

    SECTION("fallback to direct read before any window closes");
    {
        EngineSyncSampler s;
        uint8_t pins[1] = {5};
        s.configure(pins, 1, 300);          // window = 30°
        g_ain[5] = 1000;
        CHECK(s.read_raw(5) == 1000);        // no window closed yet -> direct read
    }

    SECTION("read_raw returns the crank-angle-window average once a window closes");
    {
        EngineSyncSampler s;
        uint8_t pins[1] = {5};
        s.configure(pins, 1, 300);          // 3 teeth of 10° close the window
        g_ain[5] = 1000; s.feed(100);
        g_ain[5] = 2000; s.feed(100);
        g_ain[5] = 3000; s.feed(100);       // closes: avg(1000,2000,3000)
        fprintf(stdout, "    windowed mV = %u (expect 2000)\n", s.read_raw(5));
        CHECK(s.read_raw(5) == 2000);
        // value holds across the next (open) window regardless of the live ADC
        g_ain[5] = 9000; s.feed(100);
        CHECK(s.read_raw(5) == 2000);
    }

    SECTION("non-registered pin always falls through to a direct read");
    {
        EngineSyncSampler s;
        uint8_t pins[1] = {5};
        s.configure(pins, 1, 300);
        g_ain[7] = 1234;
        CHECK(s.read_raw(7) == 1234);
    }

    SECTION("window rejects the old samples — second window is independent");
    {
        EngineSyncSampler s;
        uint8_t pins[1] = {2};
        s.configure(pins, 1, 200);          // 2 teeth per window
        g_ain[2] = 100; s.feed(100);
        g_ain[2] = 300; s.feed(100);        // window 1 closes: avg = 200
        CHECK(s.read_raw(2) == 200);
        g_ain[2] = 800; s.feed(100);
        g_ain[2] = 1000; s.feed(100);       // window 2 closes: avg = 900 (no carryover)
        fprintf(stdout, "    window2 mV = %u (expect 900)\n", s.read_raw(2));
        CHECK(s.read_raw(2) == 900);
    }

    fprintf(stdout, "ALL EngineSyncSampler tests passed.\n");
    return test_summary();
}
