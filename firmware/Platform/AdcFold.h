#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// The ADC boxcar fold — pure arithmetic over a rank-interleaved DMA ring.
//
// A scan ADC writes its regular sequence into the ring back to back, so the ring is
// rank-MAJOR-per-scan: [s0r0 s0r1 .. s0rN-1][s1r0 s1r1 ..] ... Folding rank r means
// striding by rank_count, not walking a contiguous block — get that wrong and every
// channel reads a blend of its neighbours, which looks like plausible noise rather
// than a bug.
//
// It lives here, free of any MCU header, for two reasons: it is the only part of the
// ADC path with arithmetic worth testing (the rest is register plumbing), and it is
// identical on any silicon that scans into a ring. See tests/test_adc_fold.cpp.
//
// THE BOXCAR IS THE ONLY FILTER. A per-channel IIR used to sit after this and was
// removed outright: it bought about the same noise reduction (2.65x against 2.83x)
// for roughly ten times the group delay (2.96 ms against 0.32 ms), because an
// average weights every sample equally while an exponential filter drags an infinite
// tail behind it. On a 500 Hz position loop that 3 ms was ~21 degrees of phase margin
// spent on noise that had already been removed: measured on the bench with a still
// plate, the channel sits at 0.43 counts RMS against a 0.289-count quantisation
// floor, i.e. there is no analog noise left to filter. If a channel ever needs more
// rejection, raise the oversample -- it is strictly the better trade. A CONSUMER that
// wants a smoother value filters it itself; the acquisition layer does not get to
// decide that for everyone reading the pin.
// ---------------------------------------------------------------------------

// Average `scans` consecutive scans of `rank_count` channels starting at `base`, and
// publish one filtered word per rank into `out`. `scans` must be non-zero; `base` must
// hold at least rank_count*scans samples.
inline void adc_fold_ranks(const uint16_t* base, uint8_t rank_count,
                           uint32_t scans, volatile uint16_t* out) noexcept {
    if (!base || !out || rank_count == 0u || scans == 0u) return;
    for (uint32_t c = 0; c < rank_count; ++c) {
        uint32_t sum = 0;
        for (uint32_t k = 0; k < scans; ++k) sum += base[k * rank_count + c];
        out[c] = static_cast<uint16_t>(sum / scans);
    }
}
