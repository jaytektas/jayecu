#pragma once

#include "KnockDsp.h"
#include "KnockProfile.h"

#include <cstdint>

class SignalBus;

// KnockDetector — turns a completed ADC burst (per knock sensor) into a phase-resolved KnockProfile
// and publishes the window level on knock_1 / knock_2.
//
// This is where the burst STOPS being a pile of samples and becomes something in the angle domain.
// The DSP deliberately knows nothing about crank angle; the caller knows where it opened the window
// and how fast the engine was turning, so the conversion belongs here — the same boundary the rest of
// this firmware keeps between "time-domain measurement" and "angle-domain meaning".
//
// Pure + host-testable (synthetic bursts): the ADC3 DMA burst and the crank-angle windowing that
// drive on_burst() are the hardware half (board_knock_*).
class KnockDetector {
public:
    void configure(float sampleRate, float knockFreqHz, float Q = 3.0f);

    // The geometry of the window a burst was captured in. Supplied by the caller because only it knows
    // what it armed and at what speed.
    struct Window {
        float start_deg   = 0.0f;   // crank angle of the FIRST sample, signed, relative to TDC (-ve = BTDC)
        float deg_per_sec = 0.0f;   // crank speed when the burst was armed; 0 = unknown -> no phase stamp
    };

    // A completed burst for sensor index 0/1: bandpass -> profile -> publish the window level to
    // knock_1/knock_2. `win` stamps the profile into the angle domain.
    //
    // The window is REQUIRED, with no default. A caller that does not know where it opened or how fast
    // the crank was turning has to say so by passing Window{} — deg_per_sec 0 leaves the profile
    // unstamped, which a classifier reads as "no phase information". Making that an explicit act
    // rather than a defaulted argument keeps "I don't know" from being the thing that happens when
    // nobody thought about it.
    //
    // Returns the window level in dB; `out` receives the full profile.
    float on_burst(unsigned sensor, const uint16_t* samples, unsigned count, SignalBus& bus,
                   KnockProfile& out, const Window& win, float vref = 3.3f);

    [[nodiscard]] float sampleRate() const { return sample_rate_; }

private:
    KnockDsp dsp_;
    float    sample_rate_ = 0.0f;   // kept so a burst's sample count can be turned into degrees
};
