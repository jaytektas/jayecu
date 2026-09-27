#pragma once

#include "Biquad.h"
#include "KnockProfile.h"

#include <cstdint>

// KnockDsp — the knock detection signal processing. Bandpasses a burst of raw ADC samples around the
// knock frequency and reports the band energy, both as a whole-window RMS and as a PHASE-RESOLVED
// PROFILE across the window (see KnockProfile for why the profile is the primitive and why its bucket
// count is derived rather than fixed).
//
// Pure and host-testable: no HAL, no crank angle, no RPM. The burst that feeds this and the window
// geometry that gives it meaning are the caller's business.
class KnockDsp {
public:
    // sampleRate Hz, centerFreq = knock frequency (Hz), Q = bandpass sharpness (default 3).
    void configure(float sampleRate, float centerFreq, float Q = 3.0f);

    // Bandpass `count` raw 12-bit ADC samples (0..4095, biased ~mid-rail) and fill `out` with the
    // per-bucket band RMS plus the whole-window RMS, both in dB. `vref` scales counts to volts.
    //
    // The ANGLE fields of `out` are left alone — this transform has no idea where the crank was. The
    // caller stamps them (KnockDetector does).
    //
    // Returns out.overall_db for callers that only want the level.
    float process(const uint16_t* samples, unsigned count, KnockProfile& out, float vref = 3.3f);

private:
    // Buckets this burst supports: enough samples each to span KnockProfile::MIN_CYCLES periods of the
    // band centre, capped at MAX_BUCKETS, floored at 1. Zero samples or an unconfigured band gives 1,
    // so a profile always has at least the whole-window value in it.
    [[nodiscard]] unsigned bucketsFor(unsigned count) const;

    Biquad filter_;
    float  sample_rate_ = 0.0f;
    float  center_freq_ = 0.0f;
};
