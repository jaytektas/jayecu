#pragma once

#include <cstdint>

// KnockProfile — one knock burst, resolved in PHASE rather than averaged into a number.
//
// WHY THIS EXISTS. Knock and pre-ignition excite the same bore resonance, so they land in the same
// frequency band and no filter separates them. What separates them is WHERE the energy sits relative
// to the spark: knock is end-gas autoignition after it, pre-ignition starts at or before it. A single
// RMS over the whole window — what the DSP used to return — averages that away by construction, and
// nothing downstream can recover it. So the primitive is a series of band levels across the window,
// and the classifier reads the shape.
//
// BUCKET COUNT IS DERIVED, NOT FIXED, and this is the part that is easy to get wrong.
//
// A bucket's value is an RMS, and an RMS over less than one cycle of the signal it measures is not a
// level — it is a sample of where that cycle happened to be, and it swings wildly with phase. At a
// 7 kHz knock frequency and a 281.25 kHz sample rate one knock period is ~40 samples, so a bucket
// narrower than that reports noise dressed as a measurement.
//
// The burst is not a fixed length either: the worker scales the sample count to window_length_deg
// at the current RPM, so a 40 deg window is ~2048 samples at idle and ~313 at 6000 rpm. A FIXED
// bucket count would therefore be honest at one end of the range and meaningless at the other —
// 32 buckets of 313 samples is 10 samples each, a quarter of one oscillation.
//
// So the count falls out of the physics instead: each bucket spans at least MIN_CYCLES periods of the
// knock frequency, and the number of buckets is however many of those fit in the burst, capped at
// MAX_BUCKETS. Resolution degrades gracefully with RPM — coarse but true at 6000, fine at idle —
// rather than staying nominally high and quietly becoming noise.

struct KnockProfile {
    // 32 is the ceiling, not the target. At the 2048-sample buffer and a 7 kHz band this yields ~25
    // buckets; the cap exists so the struct has a fixed size, not because 32 is meaningful.
    static constexpr unsigned MAX_BUCKETS = 32;

    // Knock periods a bucket must span before its RMS means anything. Two is the floor at which the
    // measurement stops depending on where in the cycle the bucket happened to start.
    static constexpr float MIN_CYCLES = 2.0f;

    // Level with no signal, and the value every empty bucket reads. Matches the DSP's silent floor.
    static constexpr float SILENT_DB = -100.0f;

    // ---- Filled by the DSP (energy only — it knows samples, not crank angle) ----------------------
    uint8_t count = 0;                       // buckets actually filled (0 = nothing measured)
    float   db[MAX_BUCKETS] = {};            // per-bucket band RMS, dB
    float   overall_db = SILENT_DB;          // RMS across the WHOLE window
    //
    // overall_db is not a legacy leftover. It is the magnitude criterion the pre-ignition classifier
    // needs (an extreme single event is pre-ignition-class whatever its phase), and it is what the
    // knock reference is learned against. The buckets say WHERE, this says HOW MUCH.

    // ---- Stamped by the caller (it knows the window geometry the DSP does not) --------------------
    // Engine degrees, SIGNED and relative to this cylinder's TDC: negative is BTDC. bucket i covers
    // [start_deg + i*step_deg, start_deg + (i+1)*step_deg). Zero step means the caller did not stamp,
    // which the classifier must treat as "no phase information", not as "everything at TDC".
    float start_deg = 0.0f;
    float step_deg  = 0.0f;

    [[nodiscard]] bool empty()      const { return count == 0; }
    [[nodiscard]] bool hasPhase()   const { return count > 0 && step_deg != 0.0f; }

    // Centre angle of bucket i. Only meaningful when hasPhase().
    [[nodiscard]] float angleOf(unsigned i) const {
        return start_deg + step_deg * (static_cast<float>(i) + 0.5f);
    }
};
