#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// FreqCounter — period-based frequency measurement from edge timestamps.
//
// An edge ISR calls on_edge(now_us) on each rising edge of a frequency input;
// hz(now_us) returns the current frequency in Hz, or 0 when the signal has
// stopped (no edge for longer than the staleness window). Period-based (not
// edge-count-per-window) so it is accurate at low RPM and responds within one
// pulse. Header-only + hardware-free so the logic is host-testable; the STM32
// platform owns one per frequency pin and feeds it from a capture interrupt.
//
// Single-writer (the ISR) / single-reader (the frame): the 32-bit fields are
// updated in an order that leaves hz() reading at worst a one-pulse-stale value,
// never a torn one — good enough for a sensor read.
// ---------------------------------------------------------------------------

class FreqCounter {
public:
    // Record a rising edge captured at now_us (microseconds, free-running).
    void on_edge(uint32_t now_us) {
        if (have_) {
            const uint32_t p = now_us - last_us_;   // wraps correctly (unsigned)
            if (p > 0) period_us_ = p;
        }
        last_us_ = now_us;
        have_    = true;
    }

    // Current frequency in Hz at now_us, or 0 if no signal / stopped. The signal
    // is "stopped" once no edge has arrived for >2 periods (floored at 100 ms) —
    // so a steady input holds its value and a removed input decays to 0.
    uint32_t hz(uint32_t now_us) const {
        if (!have_ || period_us_ == 0) return 0;
        uint32_t timeout = period_us_ * 2u;
        if (timeout < 100000u) timeout = 100000u;       // 100 ms floor (10 Hz min)
        if ((now_us - last_us_) > timeout) return 0;
        return (1000000u + period_us_ / 2u) / period_us_;   // rounded 1e6 / period
    }

    void reset() { have_ = false; period_us_ = 0; last_us_ = 0; }

private:
    uint32_t last_us_   = 0;
    uint32_t period_us_ = 0;
    bool     have_      = false;
};
