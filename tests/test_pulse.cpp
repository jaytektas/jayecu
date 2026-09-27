// Host test for PulseCounter — period + high-time from both-edge timestamps.

#include "Sensors/PulseCounter.h"
#include <cassert>
#include <cstdio>
#include <cstdint>

static bool near(uint32_t a, uint32_t b, uint32_t tol) {
    return (a > b ? a - b : b - a) <= tol;
}

// Drive N cycles of a square wave: period_us, high_us high then (period-high) low.
static uint32_t drive(PulseCounter& p, uint32_t t, uint32_t period_us, uint32_t high_us, int n) {
    for (int i = 0; i < n; ++i) {
        p.on_edge(t, true);              // rising
        p.on_edge(t + high_us, false);   // falling
        t += period_us;
    }
    return t;
}

int main() {
    // 50 Hz (20000 us), 50% duty -> high 10000 us.
    {
        PulseCounter p; uint32_t t = drive(p, 1000, 20000, 10000, 4);
        assert(near(p.hz(t), 50, 1));
        assert(near(p.high_us(t), 10000, 2));
        assert(near(p.duty_pct_x10(t), 500, 2));
        printf("ok  50Hz 50%%  -> %u Hz, high=%u us, duty=%u.%u%%\n",
               p.hz(t), p.high_us(t), p.duty_pct_x10(t) / 10, p.duty_pct_x10(t) % 10);
    }

    // 50 Hz, 20% duty -> high 4000 us; frequency unchanged.
    {
        PulseCounter p; uint32_t t = drive(p, 0, 20000, 4000, 4);
        assert(near(p.hz(t), 50, 1));
        assert(near(p.high_us(t), 4000, 2));
        assert(near(p.duty_pct_x10(t), 200, 2));
        printf("ok  50Hz 20%%  -> %u Hz, high=%u us, duty=%u.%u%%\n",
               p.hz(t), p.high_us(t), p.duty_pct_x10(t) / 10, p.duty_pct_x10(t) % 10);
    }

    // 50 Hz, 80% duty -> high 16000 us; frequency STILL 50 (duty-insensitive freq).
    {
        PulseCounter p; uint32_t t = drive(p, 0, 20000, 16000, 4);
        assert(near(p.hz(t), 50, 1));
        assert(near(p.high_us(t), 16000, 2));
        printf("ok  50Hz 80%%  -> %u Hz, high=%u us\n", p.hz(t), p.high_us(t));
    }

    // Flex-ish: 150 Hz (100%% ethanol) with a 5 ms temp pulse.
    {
        PulseCounter p; uint32_t t = drive(p, 0, 6667, 5000, 5);
        assert(near(p.hz(t), 150, 1));
        assert(near(p.high_us(t), 5000, 2));
        printf("ok  150Hz 5ms -> %u Hz, high=%u us\n", p.hz(t), p.high_us(t));
    }

    // No edges -> zero.
    {
        PulseCounter p;
        assert(p.hz(123456) == 0 && p.high_us(123456) == 0);
        printf("ok  no signal -> 0\n");
    }

    // Stops -> decays after the staleness window.
    {
        PulseCounter p; uint32_t t = drive(p, 0, 20000, 10000, 4);
        assert(p.high_us(t) == 10000);
        assert(p.high_us(t + 200000u) == 0);   // 200 ms later: stopped
        printf("ok  stop -> decays to 0\n");
    }

    printf("\nall pulse counter tests passed\n");
    return 0;
}
