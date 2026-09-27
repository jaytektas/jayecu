#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// PulseCounter — period AND high-time from BOTH-edge timestamps.
//
// A frequency input where the PULSE WIDTH carries information (e.g. a GM/
// Continental flex-fuel sensor: frequency = ethanol %, high-time = fuel temp).
// The capture ISR calls on_edge(now_us, rising) on every edge — `rising` is the
// pin level just after the edge. We record the rising→rising period and the
// rising→falling high-time; the read side derives frequency, pulse width (µs)
// and duty. Only timestamps + one subtract happen per edge — no division in the
// ISR (kept lean like FreqCounter). Header-only + hardware-free → host-testable.
//
// Single-writer (ISR) / single-reader (frame): fields update in an order that
// leaves a reader at worst one-edge stale, never torn.
// ---------------------------------------------------------------------------

class PulseCounter {
public:
    // Record an edge captured at now_us. `rising` = pin HIGH after the edge.
    void on_edge(uint32_t now_us, bool rising) {
        if (rising) {
            if (have_rise_) {
                const uint32_t p = now_us - last_rise_us_;   // rising→rising = period
                if (p > 0) period_us_ = p;
            }
            last_rise_us_ = now_us;
            have_rise_    = true;
        } else if (have_rise_) {
            high_us_ = now_us - last_rise_us_;               // rising→falling = high time
        }
        last_us_ = now_us;
    }

    // High time (µs) of the most recent pulse, or 0 if the signal has stopped.
    uint32_t high_us(uint32_t now_us) const {
        return stopped(now_us) ? 0u : high_us_;
    }

    // Frequency in Hz (rising→rising), or 0 if stopped.
    uint32_t hz(uint32_t now_us) const {
        if (stopped(now_us) || period_us_ == 0u) return 0u;
        return (1000000u + period_us_ / 2u) / period_us_;
    }

    // Duty cycle in tenths of a percent (0..1000), or 0 if stopped.
    uint16_t duty_pct_x10(uint32_t now_us) const {
        if (stopped(now_us) || period_us_ == 0u) return 0u;
        uint32_t d = (static_cast<uint64_t>(high_us_) * 1000u + period_us_ / 2u) / period_us_;
        return static_cast<uint16_t>(d > 1000u ? 1000u : d);
    }

    void reset() { have_rise_ = false; period_us_ = high_us_ = last_rise_us_ = last_us_ = 0; }

private:
    // "Stopped" once no edge has arrived for > 2 periods (floored at 100 ms),
    // matching FreqCounter so a steady input holds and a removed one decays.
    bool stopped(uint32_t now_us) const {
        if (!have_rise_ || period_us_ == 0u) return true;
        uint32_t timeout = period_us_ * 2u;
        if (timeout < 100000u) timeout = 100000u;
        return (now_us - last_us_) > timeout;
    }

    uint32_t last_rise_us_ = 0;
    uint32_t period_us_    = 0;
    uint32_t high_us_      = 0;
    uint32_t last_us_      = 0;
    bool     have_rise_    = false;
};
