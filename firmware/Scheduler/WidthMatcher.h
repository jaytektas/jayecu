// WidthMatcher — generic decode primitive: identify a reference cam pulse by its ANGULAR
// width. Unlike the old WIDTH_IDENTIFICATION (which counted crank teeth and broke on coarse
// cranks), this measures the pulse high-time in TICKS and converts to degrees via the live
// velocity (ticks per 0.1°, supplied by the fusion layer from the finest stream). So an 85°
// reference among 40° pulses is found even on a 120°/tooth crank.
//
// The matched pulse anchors the cycle at `target_angle` (0..7200) → an absolute reference the
// fusion layer uses to resolve the engine revolution. Header-only, ISR-inlinable.
#pragma once
#include <cstdint>
#include "SchedulerTypes.h"   // AngleDeg10

class WidthMatcher {
public:
    WidthMatcher() { reset(); }

    // min/max: accepted angular width window (decideg). target_angle: absolute crank angle
    // (0..7200) the matched pulse's reference edge sits at.
    void configure(AngleDeg10 min_deg10, AngleDeg10 max_deg10, AngleDeg10 target_angle) noexcept {
        min_ = min_deg10; max_ = max_deg10; target_ = target_angle; reset();
    }
    void reset() noexcept { locked_ = false; angle_ = 0; matched_ = false; last_width_ = 0; }

    // Feed one pulse: its high-time in ticks, and the crank rate as a RATIO — the last tooth's
    // period in ticks and that tooth's pitch in decidegrees. A pulse whose angular width falls in
    // [min,max] is the reference → lock/anchor at target_angle. Others are ignored (no false anchor).
    //
    // THE RATE ARRIVES AS A RATIO, NOT AS A PRE-DIVIDED RATE, and that is the whole point. This used
    // to take ticks-per-decidegree, which the caller had already computed as an integer division —
    // so two truncations compounded. At 8000 rpm on a 180-slit wheel the true rate is 2.083 ticks
    // per 0.1°, the integer carried 2, and an 80.0° pulse measured 83.3°. That error lands in
    // last_width_, which edge_angle() adds to the anchor, so a cam that anchors an even crank put
    // the engine's position out by an rpm-dependent amount. Dividing once, here, is exact.
    void feed(uint32_t width_ticks, uint32_t period_ticks, AngleDeg10 pitch_deg10) noexcept {
        matched_ = false;                                     // cleared each feed; see matched()
        if (period_ticks == 0 || pitch_deg10 <= 0) return;
        // A pulse hundreds of crank pitches long is not a cam reference under any geometry. The
        // guard is a plausibility check first and an overflow bound second: it caps `whole` so the
        // product below cannot run away on a garbage width.
        const uint32_t whole = width_ticks / period_ticks;    // whole crank pitches in the pulse
        if (whole > 720u) return;
        // EXACT AND 32-BIT. Splitting into whole pitches plus a remainder keeps the one wide product
        // bounded by period_ticks * pitch_deg10 — 1.8e9 at the 30 rpm floor on the coarsest even
        // crank, inside 2^32 — so this needs no uint64_t and therefore no __aeabi_uldivmod on the
        // capture path. Same bound, and the same reasoning, as EventScheduler::angle_at.
        const uint32_t rem = width_ticks - whole * period_ticks;
        const uint32_t w   = whole * static_cast<uint32_t>(pitch_deg10)
                           + (rem * static_cast<uint32_t>(pitch_deg10)) / period_ticks;
        if (min_ >= 0 && w >= static_cast<uint32_t>(min_) &&
            w <= static_cast<uint32_t>(max_)) {
            locked_ = true; angle_ = target_; matched_ = true;
            last_width_ = static_cast<AngleDeg10>(w > 0x7fff ? 0x7fff : w);
        }
    }

    // WHERE THE ENGINE IS *NOW*, as opposed to where the pulse STARTED.
    //
    // angle() reports target_ — the angle of the pulse's REFERENCE (leading) edge — but a width can
    // only be known once the pulse has ended, so this matcher is fed at the TRAILING edge. The two
    // are a whole pulse width apart. That never mattered while the cam only answered "which
    // revolution", a +/-360 question an 800 deci-degree pulse cannot change; it matters completely
    // now that a cam can anchor the within-revolution angle of a wheel that has none of its own.
    // Anchoring on angle() put an even wheel out by exactly the pulse width — measured, 80.0 deg on
    // an 80 deg pulse.
    [[nodiscard]] AngleDeg10  edge_angle()  const noexcept {
        return static_cast<AngleDeg10>(angle_ + last_width_); }

    // Did THIS pulse match the reference window? A cam carries decoys — narrow pulses the matcher
    // ignores — and only the matched one is the cycle's sync. Whoever is counting crank teeth
    // between syncs has to know which pulse was the sync and which was merely a pulse.
    [[nodiscard]] bool        matched()     const noexcept { return matched_; }
    [[nodiscard]] bool        is_locked()   const noexcept { return locked_; }
    [[nodiscard]] bool        is_absolute() const noexcept { return locked_; }  // a width match IS absolute
    [[nodiscard]] AngleDeg10  angle()       const noexcept { return angle_; }   // target (reference) angle

private:
    AngleDeg10 min_ = 0, max_ = 0, target_ = 0, angle_ = 0, last_width_ = 0;
    bool       locked_ = false, matched_ = false;
};
