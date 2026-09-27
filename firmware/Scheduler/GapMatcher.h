// GapMatcher — generic decode primitive: a uniform base wheel of `slots` teeth with zero or
// more missing-tooth gaps at known present-tooth indices. Subsumes the old EVEN/DISTRIBUTOR
// (ngap==0 → relative lock on steady teeth, no unique reference), MISSING_TOOTH (ngap==1),
// and PATTERN_EXCEPT multi-gap (ngap>1 → deferred lock once the full cyclic inter-gap count
// sequence confirms). Gaps are classified against a running NOMINAL period (not the previous
// tooth — so an adjacent close-pair of gaps, both ~ratio×, is each detected).
//
// Parametric (compact): a 36-1 stores slots=36 + one gap; it never expands to a 35-entry cell.
// Same interface as SequenceMatcher so the stream layer dispatches uniformly. Header-only,
// ISR-inlinable; steady-state O(1).
#pragma once
#include <cstdint>
#include "SchedulerTypes.h"   // AngleDeg10, MAX_ANOMALIES

class GapMatcher {
public:
    static constexpr uint8_t REQUIRED_CONFIRMS = 3;   // steady teeth (EVEN) before relative lock
    static constexpr uint8_t MAX_MISSES        = 4;

    GapMatcher() { reset(); }

    // period_angle: the stream period (3600 crank-rate / 7200 cam-rate). slots: base tooth count.
    // gap_idx/gap_ratio: ngap gaps at present-tooth indices, each spanning `ratio`× a normal tooth
    // (ratio-1 teeth missing). window_pct: gap-position tolerance (unused for the count match here,
    // kept for parity). Caller owns the gap arrays.
    void configure(AngleDeg10 period_angle, uint16_t slots,
                   const uint8_t* gap_idx, const uint8_t* gap_ratio, uint8_t ngap,
                   uint8_t window_pct) noexcept {
        period_ = period_angle; slots_ = slots;
        gap_idx_ = gap_idx; gap_ratio_ = gap_ratio;
        ngap_ = (ngap <= MAX_ANOMALIES) ? ngap : MAX_ANOMALIES;
        window_pct_ = window_pct ? window_pct : 50;
        tooth_angle_ = slots ? static_cast<AngleDeg10>(period_angle / slots) : 0;
        uint16_t missing = 0;
        max_ratio_ = 1;
        for (uint8_t k = 0; k < ngap_; ++k)
            if (gap_ratio_[k] > max_ratio_) max_ratio_ = gap_ratio_[k];
        for (uint8_t k = 0; k < ngap_; ++k) missing += static_cast<uint16_t>(gap_ratio_[k] - 1);
        present_ = (slots > missing) ? static_cast<uint16_t>(slots - missing) : slots;
        reset();
    }

    void reset() noexcept {
        state_ = State::ACQUIRING; nominal_ = 0; since_gap_ = 0; obs_n_ = 0;
        idx_ = 0; angle_ = 0; pitch_ = 0; abs_ = false; miss_ = 0; confirms_ = 0;
        last_T_ = 0; last_gr_ = 1;
    }

    void feed(uint32_t period_ticks) noexcept {
        errored_ = false;                                // cleared each feed; set on a LOCKED gap-presence miss
        if (slots_ == 0 || present_ == 0) return;
        const uint32_t T = period_ticks;
        if (nominal_ == 0) nominal_ = T;
        // A NOMINAL THAT IS TOO LOW IS A ONE-WAY TRAP, and it cost the engine. The estimate snaps
        // DOWN instantly and only rises on a tooth judged "not a gap" — so once a spurious short
        // interval drags it to a fraction of a pitch, every real tooth afterwards reads as a gap,
        // the refresh below never runs again, and it is stuck there. Measured: three edges of
        // ignition crosstalk and a 36-1 never re-acquired in over 4000 teeth, 114 revolutions. One
        // burst and the engine does not restart until something reconfigures the decoder.
        //
        // No gap on any wheel is longer than its largest configured ratio, so an interval beyond
        // that is not a gap however the estimate is scaled — it means the ESTIMATE is wrong, not the
        // wheel. Re-prime from it and the ratchet cannot close.
        if (T > static_cast<uint64_t>(nominal_) * (max_ratio_ + 1u)) nominal_ = T;
        const bool is_gap = (static_cast<uint64_t>(T) * 2 >= static_cast<uint64_t>(nominal_) * 3); // ≥1.5×
        if (!is_gap) {                                   // refresh nominal: snap down, rise slowly
            if (T < nominal_) nominal_ = T;
            else nominal_ = static_cast<uint32_t>((int32_t)nominal_ + (((int32_t)T - (int32_t)nominal_) >> 3));
        }
        ++since_gap_;

        if (ngap_ == 0) { feed_even(T); return; }        // EVEN/DISTRIBUTOR: relative lock

        if (state_ != State::LOCKED) {
            acquire(is_gap);
            // Hand the prediction its basis. Acquisition happens ON the gap, so the interval just
            // measured spanned the GAP's pitches, not one — seeding this with 1 made the very first
            // locked prediction short by the gap ratio and unlocked the wheel on the next tooth.
            last_T_  = T;
            last_gr_ = (state_ == State::LOCKED && tooth_angle_)
                       ? static_cast<uint8_t>(pitch_ / tooth_angle_) : 1;
            if (last_gr_ == 0) last_gr_ = 1;
            return;
        }

        // ---- LOCKED: EVERY EDGE MUST LAND WHERE THE SCHEDULE SAYS ------------------------------
        //
        // POSITION IS EITHER PROVEN OR IT IS NOT. This used to check only gap PRESENCE (is this
        // interval longer than 1.5x nominal or not), tolerate four mismatches, and — whether or not
        // it matched — advance the angle by the pitch it had ASSUMED. So the one event that proves
        // the decoder's position is wrong was handled by stepping forward on the assumption that it
        // was right, for ever, because nothing ever re-anchored. A chipped tooth or a marginal VR
        // gap gives one bad tooth every few revolutions, which never reaches four IN A ROW (miss_
        // was cleared by any conforming tooth), so sync was never dropped while the angle walked. A
        // spark ends up somewhere it was never commanded and the fault table says severity 1.
        //
        // The presence test was also blind by construction: "is_gap" is anything >= 1.5x, so a whole
        // extra missing tooth INSIDE the gap (3x where 2x was scheduled) read as a perfectly good
        // gap. The window catches that; a presence bit never could.
        //
        // So: predict the interval from the one just measured, scaled by the ratio of the next
        // slot's angular span to the last one's — the same ratio form SequenceMatcher uses, which
        // tracks acceleration exactly because it rides on the immediately preceding tooth rather
        // than a lagging average. Anything outside +/- window_pct means this edge is not the edge
        // that was due, and the decoder no longer knows where the engine is. Drop the lock; the
        // fusion layer turns that into a sync loss, which cuts fuel and spark and re-acquires from
        // the wheel's own reference. There is no tolerated-miss path any more, deliberately: an
        // engine that may be 6 degrees out of position (18 at the gap) must not keep firing.
        const uint16_t idx  = static_cast<uint16_t>((idx_ + 1) % present_);
        const uint8_t  gr   = gap_ratio_at(idx);
        const uint8_t  grn  = gr ? gr : 1;               // this slot's span, in tooth pitches
        // A VALID TOOTH LANDS BETWEEN HALF AND ONE AND A HALF PITCHES. The tolerance is half a tooth
        // PITCH, not a percentage of the interval — which is the same thing for a normal tooth and a
        // very different thing at a gap. Stated in pitches the whole classification is one line: an
        // interval within half a pitch of N pitches IS N pitches, and the schedule says what N
        // should be. Below 0.5 no crank could have got there; at or beyond 1.5 it is not this tooth,
        // it is the next slot along — which is exactly the 1.5x that already defines a gap, so the
        // acceptance band and the gap rule now share one boundary instead of two that can disagree.
        //
        // A percentage band could not do this. At +/-25% a gap of two pitches accepted [1.5, 2.5]
        // and a tooth accepted [0.75, 1.25], so the tolerance shrank as the wheel got coarser and a
        // cranking engine — where tooth-to-tooth speed genuinely swings — was held to the tightest
        // band exactly where it needed the loosest.
        const uint32_t pitch_est = (last_T_ && last_gr_) ? (last_T_ / last_gr_) : nominal_;
        const uint32_t expected  = pitch_est * grn;
        const uint32_t tol = static_cast<uint32_t>(
            (static_cast<uint64_t>(pitch_est) * window_pct_) / 100ull);
        if (expected == 0 || T + tol < expected || T > expected + tol) {
            errored_ = true;
            // Name it in the wheel's terms. Too EARLY is an edge that is not a tooth at all — noise,
            // a ground problem, a shield. Too LATE where a tooth was scheduled is a tooth that did
            // not arrive. A normal interval where the GAP was scheduled is a wheel that is not the
            // wheel the tune describes.
            err_kind_ = (T + tol < expected)     ? TriggerErrorKind::NOISE_EDGE
                      : (gr != 0)                ? TriggerErrorKind::GAP_MISMATCH
                                                 : TriggerErrorKind::MISSED_TOOTH;
            reset();
            return;
        }
        pitch_ = static_cast<AngleDeg10>(grn * tooth_angle_);
        angle_ = addmod(angle_, pitch_);
        idx_   = idx;
        last_T_ = T; last_gr_ = grn;
        // RE-ANCHOR ON THE WHEEL'S OWN REFERENCE. The gap is the only unique feature a gap wheel
        // has and it arrives once per pattern, so seeing it should SET the position, not merely
        // agree with it. With every edge now proven in-window this is belt and braces — but it is
        // the difference between a position derived from the wheel and one derived from a count
        // that started being right a thousand teeth ago.
        if (gr != 0) {
            AngleDeg10 a = 0;
            for (uint16_t i = 1; i <= idx; ++i)
                a = static_cast<AngleDeg10>(a + (gap_ratio_at(i) ? gap_ratio_at(i) : 1) * tooth_angle_);
            while (period_ > 0 && a >= period_) a = static_cast<AngleDeg10>(a - period_);
            angle_ = a;
        }
    }

    [[nodiscard]] bool       is_locked()   const noexcept { return state_ == State::LOCKED; }
    // True if the just-fed tooth was a LOCKED gap-presence miss — a tolerated anomaly the decoder
    // rode through (sync kept) but which MUST be counted/recorded so it is never silently forgotten.
    [[nodiscard]] bool       errored()     const noexcept { return errored_; }
    // What the just-fed tooth was wrong ABOUT (valid while errored()).
    [[nodiscard]] TriggerErrorKind err_kind() const noexcept { return err_kind_; }
    [[nodiscard]] bool       is_absolute() const noexcept { return abs_; }
    [[nodiscard]] AngleDeg10 angle()       const noexcept { return angle_; }
    [[nodiscard]] AngleDeg10 pitch()       const noexcept { return pitch_; }
    // The angular span of the interval expected NEXT — one tooth pitch, or a gap's `ratio` pitches
    // where the schedule says a gap is coming. This is what lets the caller predict WHEN the next
    // tooth is due, and it is why the prediction must be ANGULAR: the tooth after a gap is due one
    // pitch later, not one gap later, so any "same as the last interval" estimate is wrong exactly
    // at the gap — the one place on the wheel where being wrong matters most.
    // Ticks per ONE tooth pitch, from the last accepted interval and the span it covered. Not the
    // last interval itself: across a gap that is `ratio` pitches, and half of it is not half a tooth.
    // Live tolerance, in percent of a tooth pitch. Pushed per frame so it can follow RPM: a cranking
    // engine needs a wider band than one at 6000, and one constant cannot serve both.
    void set_window_pct(uint8_t pct) noexcept { window_pct_ = pct ? pct : 50; }
    // How many steady teeth an EVEN wheel needs before it locks. Three normally; one for a sync-always
    // wheel, whose first tooth is a sync tooth (see TriggerConfigCheck). Config, so reset() keeps it.
    void set_even_confirms(uint8_t n) noexcept { even_confirms_ = n ? n : REQUIRED_CONFIRMS; }
    [[nodiscard]] uint32_t   pitch_ticks() const noexcept {
        return (last_T_ && last_gr_) ? (last_T_ / last_gr_) : nominal_; }
    [[nodiscard]] AngleDeg10 next_pitch()  const noexcept {
        if (state_ != State::LOCKED || present_ == 0) return 0;
        const uint16_t nidx = static_cast<uint16_t>((idx_ + 1) % present_);
        const uint8_t  gr   = gap_ratio_at(nidx);
        return static_cast<AngleDeg10>((gr ? gr : 1) * tooth_angle_);
    }
    [[nodiscard]] uint16_t   index()       const noexcept { return idx_; }
    [[nodiscard]] uint16_t   present()     const noexcept { return present_; }

private:
    enum class State : uint8_t { ACQUIRING, LOCKED };

    AngleDeg10 addmod(AngleDeg10 a, AngleDeg10 s) const noexcept {
        AngleDeg10 r = static_cast<AngleDeg10>(a + s);
        while (period_ > 0 && r >= period_) r = static_cast<AngleDeg10>(r - period_);
        return r;
    }
    uint8_t gap_ratio_at(uint16_t i) const noexcept {
        for (uint8_t k = 0; k < ngap_; ++k) if (gap_idx_[k] == i) return gap_ratio_[k];
        return 0;
    }
    uint16_t expect_iv(uint8_t k) const noexcept {       // present teeth gap k → gap k+1
        const uint16_t a = gap_idx_[k], b = gap_idx_[(k + 1) % ngap_];
        const uint16_t iv = static_cast<uint16_t>((b + present_ - a) % present_);
        return iv ? iv : present_;                        // single gap (k+1 wraps to itself) = full cycle
    }

    void feed_even(uint32_t T) noexcept {
        if (state_ == State::LOCKED) {
            // AN EVEN WHEEL GETS THE SAME RULE. It has no gap, so there is nothing to check gap
            // PRESENCE against — which is why this path used to accept any interval at all once
            // locked, and a wheel with no missing tooth got no per-tooth validation whatsoever.
            // Every pitch being equal does not mean every interval is right; it means the check is
            // simply "the same as the last one", and an edge that is not is not a tooth. Such a
            // wheel carries no absolute reference of its own, so its position rests entirely on an
            // unbroken count — which makes validating every tooth MORE important here, not less.
            const uint32_t expected = last_T_ ? last_T_ : nominal_;
            const uint32_t tol = static_cast<uint32_t>((static_cast<uint64_t>(expected) * window_pct_) / 100);
            if (expected == 0 || T + tol < expected || T > expected + tol) {
                errored_  = true;
                err_kind_ = (T + tol < expected) ? TriggerErrorKind::NOISE_EDGE
                                                 : TriggerErrorKind::MISSED_TOOTH;
                reset();
                return;
            }
            pitch_ = tooth_angle_; angle_ = addmod(angle_, tooth_angle_);
            last_T_ = T; last_gr_ = 1;
            return;
        }
        // steady teeth (ratio ≈ 1 vs nominal) → relative lock
        const uint64_t d = (T > nominal_) ? (T - nominal_) : (nominal_ - T);
        if (d * 100 <= static_cast<uint64_t>(nominal_) * window_pct_) {
            if (++confirms_ >= even_confirms_) {
                state_ = State::LOCKED; abs_ = false; idx_ = 0; angle_ = 0; pitch_ = tooth_angle_;
                last_T_ = T; last_gr_ = 1;          // seed the prediction the locked path will use
            }
        } else confirms_ = 0;
    }

    void acquire(bool is_gap) noexcept {
        if (!is_gap) return;                              // count toward the next interval
        const uint16_t iv = since_gap_; since_gap_ = 0;
        const uint8_t ivb = (iv > 255) ? 255 : static_cast<uint8_t>(iv);
        if (obs_n_ < ngap_) obs_[obs_n_++] = ivb;
        else { for (uint8_t j = 1; j < ngap_; ++j) obs_[j-1] = obs_[j]; obs_[ngap_-1] = ivb; }
        if (obs_n_ < ngap_) return;

        int match_r = -1, matches = 0;
        for (uint8_t r = 0; r < ngap_; ++r) {
            bool ok = true;
            for (uint8_t j = 0; j < ngap_; ++j)
                if (obs_[j] != expect_iv(static_cast<uint8_t>((r + j) % ngap_))) { ok = false; break; }
            if (ok) { match_r = r; ++matches; }
        }
        if (matches == 0) return;                         // keep sliding the window

        const uint16_t idx0 = gap_idx_[match_r];          // we are AT this gap now
        AngleDeg10 a = 0;
        for (uint16_t i = 1; i <= idx0; ++i)
            a = static_cast<AngleDeg10>(a + (gap_ratio_at(i) ? gap_ratio_at(i) : 1) * tooth_angle_);
        while (period_ > 0 && a >= period_) a = static_cast<AngleDeg10>(a - period_);
        idx_ = idx0; angle_ = a; pitch_ = static_cast<AngleDeg10>(gap_ratio_[match_r] * tooth_angle_);
        state_ = State::LOCKED; abs_ = true; miss_ = 0;
    }

    AngleDeg10     period_ = 0, tooth_angle_ = 0;
    uint16_t       slots_ = 0, present_ = 0;
    const uint8_t* gap_idx_ = nullptr;
    const uint8_t* gap_ratio_ = nullptr;
    uint8_t        ngap_ = 0, window_pct_ = 25;
    uint8_t        max_ratio_ = 1;    // widest gap this wheel has, in tooth pitches
    uint8_t        even_confirms_ = REQUIRED_CONFIRMS;   // steady teeth before an even wheel locks

    State    state_ = State::ACQUIRING;
    uint32_t nominal_ = 0;
    uint16_t since_gap_ = 0;
    uint8_t  obs_[MAX_ANOMALIES] = {};
    uint8_t  obs_n_ = 0, confirms_ = 0, miss_ = 0;
    uint32_t last_T_ = 0;             // the last ACCEPTED interval, and how many tooth pitches it
    uint8_t  last_gr_ = 1;            // spanned — the basis for predicting the next one
    bool     errored_ = false;        // this-feed gap-presence miss (reported even when tolerated)
    TriggerErrorKind err_kind_ = TriggerErrorKind::NONE;   // which one, while errored_
    uint16_t idx_ = 0;
    AngleDeg10 angle_ = 0, pitch_ = 0;
    bool     abs_ = false;
};
