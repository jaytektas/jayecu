// Host test for FreqCounter — period-based frequency from edge timestamps.

#include "Sensors/FreqCounter.h"
#include <cassert>
#include <cstdio>

static bool near_hz(uint32_t got, uint32_t want, uint32_t tol) {
    return (got > want ? got - want : want - got) <= tol;
}

int main() {
    // 1 kHz square wave: rising edges every 1000 us.
    {
        FreqCounter f;
        uint32_t t = 10000;
        for (int i = 0; i < 5; i++) { f.on_edge(t); t += 1000; }
        assert(near_hz(f.hz(t), 1000, 1));
        printf("ok  1 kHz -> %u Hz\n", f.hz(t));
    }

    // 50 Hz: edges every 20000 us.
    {
        FreqCounter f;
        uint32_t t = 0;
        for (int i = 0; i < 4; i++) { f.on_edge(t); t += 20000; }
        assert(near_hz(f.hz(t), 50, 1));
        printf("ok  50 Hz -> %u Hz\n", f.hz(t));
    }

    // No edges yet -> 0.
    {
        FreqCounter f;
        assert(f.hz(123456) == 0);
        printf("ok  no signal -> 0 Hz\n");
    }

    // Signal stops -> decays to 0 after the staleness window (>2 periods, >=100ms).
    {
        FreqCounter f;
        uint32_t t = 0;
        for (int i = 0; i < 4; i++) { f.on_edge(t); t += 1000; }   // 1 kHz
        assert(near_hz(f.hz(t), 1000, 1));
        assert(f.hz(t + 200000u) == 0);                            // 200 ms later: stopped
        printf("ok  stopped signal -> 0 Hz after timeout\n");
    }

    // Timestamp wrap (unsigned subtraction stays correct across 2^32).
    {
        FreqCounter f;
        uint32_t t = 0xFFFFFE00u;            // near wrap
        for (int i = 0; i < 4; i++) { f.on_edge(t); t += 1000; }   // wraps mid-train
        assert(near_hz(f.hz(t), 1000, 1));
        printf("ok  wrap-safe -> %u Hz\n", f.hz(t));
    }

    printf("\nAll FreqCounter tests passed.\n");
    return 0;
}
