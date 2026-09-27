// SequenceMatcher — generic decode primitive for the generic-trigger architecture.
//
// Locks onto a repeating cyclic "cell" of inter-event angles by matching the measured
// event-period RATIO sequence. Stream-agnostic: the events can be crank teeth (odd-fire
// uneven wheel) or cam pulses (phase identification). Matching is on period RATIOS, so it
// is RPM-invariant AND independent of any other stream's angular resolution — that's what
// lets it phase a multi-pulse cam sitting on a coarse (e.g. 120°) crank.
//
// cell[i] = angular span (decidegrees) from event i to event i+1; Σ cell = the stream's
// period (3600 for a crank-rate stream, 7200 for cam-rate). Event i sits at absolute
// intra-period angle A(i) = Σ_{j<i} cell[j].
//
// A uniform cell (all spans equal) has no unique reference → it locks RELATIVE (index 0
// arbitrary), like a plain distributor. An asymmetric cell locks ABSOLUTE.
//
// Header-only + all-inline: it runs in the capture ISR. No allocation; the caller owns the
// cell storage (a config array). One 64-bit multiply per candidate during acquisition only;
// steady-state is O(1) (one predicted-window check + one indexed add).
#pragma once
#include <cstdint>
#include "SchedulerTypes.h"   // AngleDeg10, MAX_PATTERN_TEETH

class SequenceMatcher {
public:
    // Confirm this many consecutive index-advancing matches before declaring a lock.
    static constexpr uint8_t REQUIRED_CONFIRMS = 3;
    // Drop the lock after this many consecutive predicted-window violations.
    static constexpr uint8_t MAX_MISSES = 4;

    SequenceMatcher() { reset(); }

    // cell: inter-event angles (decideg), n entries (2..MAX_PATTERN_TEETH). window_pct = ratio
    // match tolerance (±%). Caller keeps `cell` alive (points at config storage).
    void configure(const AngleDeg10* cell, uint8_t n, uint8_t window_pct) noexcept {
        cell_ = cell; n_ = n; window_pct_ = window_pct ? window_pct : 25;
        period_ = 0; uniform_ = (n > 0);
        for (uint8_t i = 0; i < n; ++i) {
            period_ += cell ? cell[i] : 0;
            if (cell && cell[i] != cell[0]) uniform_ = false;
        }
        reset();
    }

    void reset() noexcept {
        state_ = State::ACQUIRING; prev_ = 0; cand_ = -1; confirms_ = 0;
        miss_ = 0; idx_ = 0; angle_ = 0; pitch_ = 0; abs_ = false;
    }

    // Feed one event's measured period (ticks since the previous event of THIS stream).
    void feed(uint32_t period_ticks) noexcept {
        errored_ = false;                          // cleared each feed; set on a LOCKED window violation
        if (n_ < 2 || cell_ == nullptr) return;
        const uint32_t T = period_ticks, Tp = prev_;
        prev_ = T;
        if (Tp == 0) return;                       // need two periods to form a ratio

        if (state_ != State::LOCKED) { acquire(T, Tp); return; }

        // ---- LOCKED: predict the next interval from the locked phase + velocity ----
        const AngleDeg10 sp = cell_[wrap(idx_ - 1)];          // previous interval
        uint64_t expected = (sp > 0) ? (static_cast<uint64_t>(Tp) * cell_[idx_]) / sp : T;
        const uint64_t tol = (expected * window_pct_) / 100;
        const uint64_t d   = (T > expected) ? (T - expected) : (expected - T);
        // POSITION IS PROVEN OR IT IS NOT — the same rule GapMatcher applies, and this was left
        // behind when that changed. It tolerated four window violations before dropping the lock,
        // which is the ride-through the gap path had removed: an event out of place means the
        // decoder cannot say where the engine is, and riding through it advances the pattern on an
        // assumption. SEQUENCE is not a corner case either — it is the odd-fire cranks, the GM 4200,
        // and every multi-pulse cam.
        //
        // The `!uniform_` guard is gone with it. A uniform cell was exempted from the check
        // altogether, so a pattern of equal steps accepted ANY interval once locked — the same hole
        // the even-wheel path had. Uniform needs no special case: with every cell equal the
        // prediction reduces to "the same as the last one", which is exactly the right question.
        if (expected > 0 && d > tol) {
            errored_ = true;
            reset();
            return;
        }
        miss_ = 0;
        pitch_ = cell_[idx_];
        angle_ = addmod(angle_, cell_[idx_]);
        idx_   = wrap(idx_ + 1);
    }

    // Live tolerance, in percent of the predicted interval. Pushed per frame so it can follow RPM:
    // a cranking engine needs a wider band than one at 6000 (see GenericTrigger::set_rpm).
    void set_window_pct(uint8_t pct) noexcept { window_pct_ = pct ? pct : 25; }
    // Steady events a UNIFORM pattern needs before it locks: one for a sync-always wheel (see GapMatcher).
    void set_uniform_confirms(uint8_t n) noexcept { uniform_confirms_ = n ? n : REQUIRED_CONFIRMS; }
    [[nodiscard]] bool        is_locked()   const noexcept { return state_ == State::LOCKED; }
    // True if the just-fed event was a LOCKED predicted-window violation — a tolerated anomaly the
    // decoder rode through (sync kept) but which MUST be counted/recorded, never silently forgotten.
    [[nodiscard]] bool        errored()     const noexcept { return errored_; }
    [[nodiscard]] bool        is_absolute() const noexcept { return abs_; }   // unique vs relative
    [[nodiscard]] AngleDeg10  angle()       const noexcept { return angle_; } // intra-period, at current event
    [[nodiscard]] AngleDeg10  pitch()       const noexcept { return pitch_; } // span just traversed (PLL feed)
    // Span of the interval expected NEXT. idx_ already points at the cell the next event closes
    // (feed() advances it after using cell_[idx_old]), so this is a plain lookup.
    [[nodiscard]] AngleDeg10  next_pitch()  const noexcept {
        return (state_ == State::LOCKED && cell_ && n_) ? cell_[idx_] : 0; }
    [[nodiscard]] uint8_t     index()       const noexcept { return idx_; }   // current cell index
    [[nodiscard]] AngleDeg10  period()      const noexcept { return period_; }

private:
    enum class State : uint8_t { ACQUIRING, LOCKED };

    uint8_t wrap(int i) const noexcept { int m = i % n_; if (m < 0) m += n_; return static_cast<uint8_t>(m); }
    AngleDeg10 addmod(AngleDeg10 a, AngleDeg10 step) const noexcept {
        AngleDeg10 r = static_cast<AngleDeg10>(a + step);
        while (period_ > 0 && r >= period_) r = static_cast<AngleDeg10>(r - period_);
        return r;
    }
    AngleDeg10 angle_of(uint8_t i) const noexcept {           // A(i) = Σ_{j<i} cell[j]
        AngleDeg10 a = 0;
        for (uint8_t j = 0; j < i; ++j) a = static_cast<AngleDeg10>(a + cell_[j]);
        return a;
    }

    void acquire(uint32_t T, uint32_t Tp) noexcept {
        if (uniform_) {
            // No unique reference: lock relative once the stream is steady (ratio ≈ 1).
            const uint64_t d = (T > Tp) ? (T - Tp) : (Tp - T);
            if (d * 100 <= static_cast<uint64_t>(Tp) * window_pct_) {
                if (++confirms_ >= uniform_confirms_) {
                    state_ = State::LOCKED; abs_ = false;
                    idx_ = 0; angle_ = 0; pitch_ = cell_[0]; miss_ = 0;
                }
            } else confirms_ = 0;
            return;
        }
        // Non-uniform: the completed interval is the cell index j whose ratio cell[j]/cell[j-1]
        // is CLOSEST to T/Tp (argmin, cross-multiplied to stay integer) — nearest, not every
        // in-tolerance one, so adjacent cell ratios can't both qualify.
        int best = -1; uint64_t best_d = ~0ULL, best_rhs = 0;
        for (uint8_t j = 0; j < n_; ++j) {
            const uint64_t lhs = static_cast<uint64_t>(T)  * cell_[wrap(j - 1)];
            const uint64_t rhs = static_cast<uint64_t>(Tp) * cell_[j];
            const uint64_t d   = (lhs > rhs) ? (lhs - rhs) : (rhs - lhs);
            if (d < best_d) { best_d = d; best = j; best_rhs = rhs; }
        }
        if (best >= 0 && best_d <= (best_rhs * window_pct_) / 100) {
            if (cand_ >= 0 && best == ((cand_ + 1) % n_)) ++confirms_; else confirms_ = 1;
            cand_ = static_cast<int8_t>(best);
            if (confirms_ >= REQUIRED_CONFIRMS) {
                // completed interval `best` → we are AT event (best+1); anchor absolute angle.
                idx_   = wrap(best + 1);
                angle_ = angle_of(idx_);
                pitch_ = cell_[best];
                state_ = State::LOCKED; abs_ = true; miss_ = 0;
            }
        } else { confirms_ = 0; cand_ = -1; }
    }

    const AngleDeg10* cell_ = nullptr;
    uint8_t    n_ = 0;
    uint8_t    window_pct_ = 25;
    uint8_t    uniform_confirms_ = REQUIRED_CONFIRMS;
    AngleDeg10 period_ = 0;
    bool       uniform_ = false;

    State      state_ = State::ACQUIRING;
    uint32_t   prev_ = 0;
    int8_t     cand_ = -1;
    uint8_t    confirms_ = 0;
    uint8_t    miss_ = 0;
    bool       errored_ = false;     // this-feed window violation (reported even when tolerated)
    uint8_t    idx_ = 0;
    AngleDeg10 angle_ = 0;
    AngleDeg10 pitch_ = 0;
    bool       abs_ = false;
};
