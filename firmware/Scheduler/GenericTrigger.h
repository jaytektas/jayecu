// GenericTrigger — the generic-trigger core: N signal streams, each with a rate (crank/cam)
// and a decode primitive (GAP/SEQUENCE/WIDTH), fused into engine position + sync level. This
// replaces the SyncStrategy switch with a composable pool.
//
// Hardware-independent and testable: callers feed edges via on_edge(stream, tick); the class
// produces level()/angle()/velocity for the PLL + scheduler layer. No allocation; header-only.
//
// Fusion model:
//   - Each stream's primitive yields {locked, absolute, intra-period angle, pitch}.
//   - The within-revolution angle + velocity come from the FINE source (a locked CRANK-rate
//     stream — finest); the engine angle is tracked over the full 720° cycle, the revolution
//     bit TOGGLING each time the crank angle wraps 360°.
//   - A locked+absolute CAM-rate stream CORRECTS the revolution bit (and, alone, provides the
//     whole cycle — a cam-only CAS).
//   - level: NONE (nothing locked) → CRANK (a stream locked) → PHASE (revolution resolved).
#pragma once
#include <cstdint>
#include "SchedulerTypes.h"
#include "GapMatcher.h"
#include "SequenceMatcher.h"
#include "WidthMatcher.h"

inline constexpr int MAX_STREAMS = 6;

// HOW MANY TIMES a stream's pattern repeats per ENGINE CYCLE. Its angular period is cycle/repeats.
//
// This replaces a CRANK/CAM binary, which could only express periods of 360 and 720 and so had no
// room for the wheels that sit between and beyond them: a symmetrical crank wheel whose pattern
// appears twice per revolution (repeats 4), a 12-tooth Honda crank (24), a V10 with 5-fold symmetry
// (10), or a distributor on an ODD-cylinder engine — 5 evenly spaced pulses per cycle is repeats 5,
// which is 2.5 per crank revolution and therefore not expressible as a crank-rate wheel at all.
//
// The familiar two are just the ends of it: REPEATS_CRANK is 2 on a four-stroke, and 3 on a rotary
// where one e-shaft revolution is a third of the 1080-degree cycle — which the old enum hid.
using StreamRepeats = uint8_t;
inline constexpr StreamRepeats REPEATS_PHASE = 1;   // pattern spans the whole engine cycle
enum class PrimKind   : uint8_t { GAP = 0, SEQUENCE = 1, WIDTH = 2 };
enum class SyncLvl    : uint8_t { NONE = 0, CRANK = 1, PHASE = 2 };

class GenericTrigger {
public:
    GenericTrigger() { reset_all(); }

    // The ENGINE CYCLE this decoder is resolving position within: 3600 (two-stroke), 7200
    // (four-stroke) or 10800 (rotary). Set BEFORE the streams are added — a cam-rate stream's
    // period is the cycle, and a rotary needs three revolutions distinguished rather than two.
    // Defaults to four-stroke, which is what it was hardcoded to before.
    void set_cycle(AngleDeg10 cycle) noexcept {
        cycle_ = (cycle >= ANGLE_360) ? cycle : ANGLE_720;
        revs_  = static_cast<uint8_t>(cycle_ / ANGLE_360);   // 1, 2 or 3
    }
    [[nodiscard]] AngleDeg10 cycle() const noexcept { return cycle_; }

    // rpm-band gate for the phase (cam) stream (min/max_full_sync_rpm). The HAL sets this each service
    // pass from live rpm; the decoder reads it in the ISR. false => the cam stream is out of its trusted
    // band, so fuse() ignores its edges for BOTH acquisition and correction (a VR cam glitches below
    // ~1500 rpm). An already-known revolution is RETAINED — crank counting holds it, unaffected. Default
    // true = band disabled (min==max==0), the historical behaviour.
    void set_phase_stream_valid(bool v) noexcept { phase_stream_valid_ = v; }

    // SYNC ALWAYS (TriggerConfigCheck::sync_always): the tune is one even wheel in distributor mode,
    // so every tooth is a sync tooth. The wheel locks on its first steady tooth and that lock IS full
    // sync — there is no anchor to wait for and no revolution to resolve, because with one coil and a
    // rotor every tooth is equivalent over the whole cycle. Engine configuration, like the cycle: it
    // survives reset_all()/drop_sync(), and is applied to streams as they are added.
    void set_sync_always(bool v) noexcept {
        sync_always_ = v;
        for (int i = 0; i < MAX_STREAMS; ++i) {
            st_[i].gap.set_even_confirms(v ? 1 : GapMatcher::REQUIRED_CONFIRMS);
            st_[i].seq.set_uniform_confirms(v ? 1 : SequenceMatcher::REQUIRED_CONFIRMS);
        }
    }
    [[nodiscard]] bool sync_always() const noexcept { return sync_always_; }

    // FULL teardown: wipe the stream config too (n_ = 0). Only valid as the first half of a rebuild —
    // build_generic() calls this then re-adds every stream from config. NEVER call it standalone at
    // runtime: with n_ = 0, on_edge() rejects every edge and the decoder is dead until the next rebuild.
    // NOTE: the CYCLE survives a reset — it is engine configuration, not decode state, and
    // build_generic() sets it before re-adding the streams anyway.
    void reset_all() noexcept {
        for (int i = 0; i < MAX_STREAMS; ++i) st_[i] = Stream{};
        n_ = 0; level_ = SyncLvl::NONE; angle_ = 0; velocity_ = 0;
        rev_ = 0; rev_known_ = false; prev_fine_ = 0; primed_fine_ = false; has_crank_ = false;
        rep_ = 0; fine_period_ = ANGLE_360; fine_repeats_ = 2;
    }

    // RUNTIME sync loss that KEEPS the stream config — for a forced desync (lost-trigger watchdog) where
    // the wheel hasn't changed and we want the decoder to re-acquire from the next real teeth. Returns
    // every matcher to ACQUIRING (their wheel config survives — see each matcher's reset()) and clears
    // the per-stream tooth timing + the top-level sync, but leaves n_ and the stream configs intact.
    // ISR-safe: plain field writes, no allocation, no rebuild.
    void drop_sync() noexcept {
        for (int i = 0; i < n_; ++i) {
            Stream& s = st_[i];
            s.gap.reset(); s.seq.reset(); s.width.reset();          // matchers -> ACQUIRING (config kept)
            s.primed = false; s.last = 0; s.last_period_ = 0;       // per-tooth timing; fresh period next edge
            s.lead_primed_ = false; s.lead_tick_ = 0;               // WIDTH lead state (keep lead_rising_ config)
        }
        level_ = SyncLvl::NONE; angle_ = 0; velocity_ = 0;
        phase_marked_ = false; phase_expect_teeth_ = 0;
        anchored_ = false; pending_anchor_ = false;
        // has_crank_ IS NOT DECODE STATE AND IS NOT CLEARED HERE. It says whether the TUNE gave this
        // wheel a crank-rate stream, and only add_gap/add_seq/add_width ever set it — a sync loss
        // cannot change the answer, because the wheel has not changed. Clearing it here left the
        // decoder believing it had no crank at all until the next rebuild, and the branch that reads
        // it first is the one that matters most: "cam-only sensor: cam is everything" grants FULL
        // PHASE SYNC off a single cam edge. So one stray cam edge after a sync loss handed the
        // decoder complete position, named the cam the velocity source, and let get_rpm_x10() go on
        // reporting the free-running grid's speed — an engine at a frozen rpm with a dead crank.
        // (Three other readers were wrong the same way: the cam could no longer queue a crank
        // anchor, the "nothing locked" reset test changed meaning, and the cam's position
        // cross-check switched itself off.) reset_all() still clears it — that IS the rebuild, and
        // build_generic re-adds every stream immediately after.
        rev_ = 0; rev_known_ = false; prev_fine_ = 0; primed_fine_ = false;
        rep_ = 0; fine_period_ = ANGLE_360; fine_repeats_ = 2;
    }

    // add_gap/add_seq COPY the cell into per-stream storage, so callers may pass stack
    // temporaries or a shadow config that later moves (matchers point at our own arrays).
    // `slot` is WHICH stream this is, chosen by the caller — the config index, which is the role.
    // These used to append (n_++), which made contiguity load-bearing and undocumented: on_edge is
    // handed the CONFIG index by the position HAL, so the moment a config had a hole (an engine with
    // phasers on the intakes alone leaves the exhaust slots empty) the edges landed on the wrong
    // decoder stream, or on none. n_ is now the high-water mark rather than a fill pointer.
    int add_gap(int slot, StreamRepeats repeats, uint16_t slots, const uint8_t* gi, const uint8_t* gr,
                uint8_t ngap, uint8_t win) noexcept {
        if (slot < 0 || slot >= MAX_STREAMS) return -1;
        const int i = slot; Stream& s = st_[i];
        if (i >= n_) n_ = i + 1;
        s.enabled = true; s.kind = PrimKind::GAP; set_repeats(s, repeats);
        if (ngap > MAX_ANOMALIES) ngap = MAX_ANOMALIES;
        for (uint8_t k = 0; k < ngap; ++k) { s.gidx_[k] = gi[k]; s.gratio_[k] = gr ? gr[k] : 2; }
        // A CRANK-rate wheel repeats every revolution whatever the engine; a CAM-rate (phase)
        // stream repeats once per ENGINE CYCLE — which is the whole point of it, and is 1080 deg on
        // a rotary, not 720.
        s.window_pct_ = win ? win : 25;
        s.gap.configure(s.period_,
                        slots, s.gidx_, s.gratio_, ngap, win);
        s.gap.set_even_confirms(sync_always_ ? 1 : GapMatcher::REQUIRED_CONFIRMS);
        if (repeats > REPEATS_PHASE) has_crank_ = true;
        return i;
    }
    int add_seq(int slot, StreamRepeats repeats, const AngleDeg10* cell, uint8_t ncell, uint8_t win) noexcept {
        if (slot < 0 || slot >= MAX_STREAMS) return -1;
        const int i = slot; Stream& s = st_[i];
        if (i >= n_) n_ = i + 1;
        s.enabled = true; s.kind = PrimKind::SEQUENCE; set_repeats(s, repeats);
        if (ncell > MAX_PATTERN_TEETH) ncell = MAX_PATTERN_TEETH;
        for (uint8_t k = 0; k < ncell; ++k) s.cell_[k] = cell[k];
        s.window_pct_ = win ? win : 25;
        s.seq.configure(s.cell_, ncell, win);
        s.seq.set_uniform_confirms(sync_always_ ? 1 : SequenceMatcher::REQUIRED_CONFIRMS);
        if (repeats > REPEATS_PHASE) has_crank_ = true;
        return i;
    }
    // WIDTH stream: identify a reference pulse by angular width. Fed BOTH edges via on_edge's
    // `rising` flag (leading edge opens the window, the other closes it → high-time).
    int add_width(int slot, StreamRepeats repeats, AngleDeg10 min_deg10, AngleDeg10 max_deg10,
                  AngleDeg10 target_angle, bool leading_rising = true) noexcept {
        if (slot < 0 || slot >= MAX_STREAMS) return -1;
        const int i = slot; Stream& s = st_[i];
        if (i >= n_) n_ = i + 1;
        s.enabled = true; s.kind = PrimKind::WIDTH; s.lead_rising_ = leading_rising; set_repeats(s, repeats);
        s.window_pct_ = 25;
        s.width.configure(min_deg10, max_deg10, target_angle);
        if (repeats > REPEATS_PHASE) has_crank_ = true;
        return i;
    }

    // What a CAM slot is, physically: where its reference sits with the phaser parked, and whether a
    // phaser can move it. Separate from add_seq/add_width because it applies to any primitive and is
    // meaningless on a crank slot. Called from build_generic.
    // THE MATCH WINDOW FOLLOWS RPM. A starter turns an engine unevenly — it slows into every
    // compression and surges over the top — so tooth-to-tooth speed swings hardest exactly where the
    // decoder has least history to predict from. By 6000 rpm the rotating inertia has made
    // consecutive teeth nearly identical and a tight band costs nothing. One constant cannot serve
    // both ends: tight enough to catch a marginal wheel at speed is tight enough that the engine
    // never starts. Blended linearly between the two rpm marks below, which are fixed because they
    // describe an engine rather than a wheel.
    void set_rpm(uint32_t rpm) noexcept {
        static constexpr uint32_t kCrankRpm = 400;    // at or below: fully the cranking window
        static constexpr uint32_t kRunRpm   = 1500;   // at or above: fully the running window
        for (int i = 0; i < n_; ++i) {
            Stream& s = st_[i];
            if (!s.enabled) continue;
            const uint8_t crank = s.win_crank_ ? s.win_crank_ : s.win_run_;
            uint8_t pct;
            if      (rpm <= kCrankRpm) pct = crank;
            else if (rpm >= kRunRpm)   pct = s.win_run_;
            else {
                const uint32_t span = kRunRpm - kCrankRpm;
                const uint32_t up   = rpm - kCrankRpm;
                pct = static_cast<uint8_t>(crank - ((crank - s.win_run_) * up) / span);
            }
            if      (s.kind == PrimKind::GAP)      s.gap.set_window_pct(pct);
            else if (s.kind == PrimKind::SEQUENCE) s.seq.set_window_pct(pct);
        }
    }

    void set_window_pcts(int slot, uint8_t run_pct, uint8_t crank_pct) noexcept {
        if (slot < 0 || slot >= MAX_STREAMS) return;
        st_[slot].win_run_   = run_pct   ? run_pct   : 50;
        st_[slot].win_crank_ = crank_pct ? crank_pct : st_[slot].win_run_;
    }

    void set_phase_meta(int slot, AngleDeg10 nominal, bool phased, AngleDeg10 authority,
                        AngleDeg10 allowance = 0) noexcept {
        if (slot < 0 || slot >= MAX_STREAMS) return;
        st_[slot].nominal_   = nominal;
        st_[slot].phased_    = phased;
        st_[slot].authority_ = authority;
        st_[slot].allowance_ = allowance;
    }

    // One edge on `idx` at absolute tick. `rising` is the captured edge polarity — used only by
    // WIDTH streams (which need both edges); GAP/SEQUENCE see one configured edge per tooth.
    void on_edge(uint8_t idx, uint32_t tick, bool rising = true) noexcept {
        // IT ANSWERS "DID *THIS* EDGE ADVANCE THE POSITION STREAM", so it is cleared HERE, once, for
        // every edge — not inside fuse(), which four of the returns below never reach.
        //
        // It was cleared in fuse(), and while it only gated the PLL feed that was merely untidy: an
        // edge that skipped fuse() fed the VirtualTrigger a stale angle at a fresh tick, and the slew
        // clamp absorbed it. Then the stall watchdog was hung on the same flag (the decoder's own
        // clock), and a leftover `true` stopped meaning "re-aim the grid" and started meaning THE
        // ENGINE IS STILL TURNING: last_fine_tick_ re-stamped, trigger_absent_ cleared, the deadline
        // re-armed — by an edge that carried no tooth.
        //
        // The four that never reach fuse() are all ordinary, not exotic: a WIDTH cam's LEADING edge
        // (every cam pulse, on every phase-synced engine), a WIDTH edge with no lead primed, any
        // stream's first edge — and drop_sync() clears `primed` on every stream, so that one recurs
        // at every re-acquisition — and a zero-length interval. Measured: with the crank dead and
        // stray edges arriving on one other stream faster than a tooth was due, the decoder held
        // PHASE sync and a frozen rpm for as long as the edges kept coming, and never once reported
        // SIGNAL_ABSENT. That is the exact failure the decoder's clock was added to end.
        fine_advanced_ = false;
        // `enabled` is not decoration now that slots can have holes: an engine with phasers on the
        // intakes alone configures slots 2 and 4 and leaves 3 empty, so an index inside n_ is not
        // proof of a configured stream. Without this an edge would be fed to a default-constructed
        // Stream and matched against a wheel nobody described.
        if (idx >= n_ || !st_[idx].enabled) return;
        // EVERY accepted edge, counted and never reset. tooth_counter_ below is a POSITION (it zeroes
        // at each cycle boundary, and only advances once a stream is locked); this is evidence that the
        // wheel is turning AT ALL, which is a different question and one asked before sync exists — a
        // fuel pump runs from the first tooth, long before anything is fired.
        ++edges_total_;
        Stream& s = st_[idx];
        if (s.kind == PrimKind::WIDTH) {                 // measure high-time, feed at the trailing edge
            if (rising == s.lead_rising_) { s.lead_tick_ = tick; s.lead_primed_ = true; return; }
            if (!s.lead_primed_) return;
            const uint32_t w = tick - s.lead_tick_;
            // The crank rate as a RATIO. fine_pitch_/velocity_ are the last fine tooth's pitch and
            // period; passing them unreduced lets the matcher divide exactly once (see its feed()).
            s.width.feed(w, velocity_, fine_pitch_);
            fuse(idx, tick);
            return;
        }
        if (!s.primed) { s.last = tick; s.primed = true; return; }
        const uint32_t T = tick - s.last;
        if (T == 0) return;
        // NO FILTER HERE, DELIBERATELY. An earlier version rejected an edge that arrived too soon to
        // be a tooth and carried on decoding, which is the wrong shape of answer: it treats noise as
        // something to be tolerated rather than something that means the trigger cannot be trusted.
        // There is no acceptable amount of noise on a trigger input. Every edge goes to the matcher,
        // which judges it against the interval the schedule predicted, and one that does not belong
        // drops the lock.
        s.last = tick;
        if (s.kind == PrimKind::GAP)           s.gap.feed(T);
        else if (s.kind == PrimKind::SEQUENCE) s.seq.feed(T);
        s.last_period_ = T;
        fuse(idx, tick);
        note_error(s);   // tally any tolerated miss this stream's matcher reported (after fuse → tooth_counter_ current)
    }

    [[nodiscard]] SyncLvl    level()     const noexcept { return level_; }
    [[nodiscard]] AngleDeg10 angle()     const noexcept { return angle_; }    // 0..7200
    [[nodiscard]] uint32_t   velocity()  const noexcept { return velocity_; } // fine-stream period
    [[nodiscard]] bool       rev_known() const noexcept { return rev_known_; }
    // WHICH revolution of the engine cycle the decoder believes it is in (0..revs_-1). Read-only —
    // the engine-cycle capture needs it to tell a real cycle boundary from the cam's phase
    // correction, which steps angle_ back by a whole revolution and is not a boundary at all.
    [[nodiscard]] uint8_t    rev()       const noexcept { return rev_; }
    // Sync-health readout (the task derives error % + raises the DTC). Counts every TOLERATED gap-miss.
    [[nodiscard]] uint16_t   teeth_last_cycle()  const noexcept { return teeth_last_cycle_; }
    [[nodiscard]] uint16_t   errors_last_cycle() const noexcept { return errors_last_cycle_; }
    [[nodiscard]] uint32_t   error_total()       const noexcept { return err_total_; }
    [[nodiscard]] uint16_t   last_error_tooth()  const noexcept { return last_err_tooth_; }
    [[nodiscard]] uint32_t   edges_total()       const noexcept { return edges_total_; }
    // FREE-RUNNING totals. errors_last_cycle() is snapshotted at a cycle boundary that is itself
    // counted in teeth, so when the teeth stop it never updates again — useless for reporting the
    // very fault that stopped them. These just count, and the task layer publishes them.
    [[nodiscard]] uint32_t   noise_total()       const noexcept { return noise_total_; }
    [[nodiscard]] uint32_t   missed_total()      const noexcept { return missed_total_; }
    // Phase downgrades. A count rather than a flag because the fault that matters on a VVT engine is
    // not "phase is down now" but "phase keeps going down" — a position check that disagrees with the
    // phaser drops and re-acquires every cycle, which reads as healthy in any single sample.
    [[nodiscard]] uint32_t   phase_lost_total()   const noexcept { return phase_lost_total_; }
    // WHICH STREAM last raised a fault. A V engine has a cam per bank and its own chain per bank, so
    // "the trigger is unhappy" is not an actionable fault — "bank 2's cam has moved" is. 0xFF = none.
    [[nodiscard]] uint8_t    last_error_stream()  const noexcept { return last_err_stream_; }
    // How far this cam is from its parked angle, measured against the CRANK. The real displacement,
    // as opposed to stream_angle() - nominal, which for a WIDTH cam subtracts one constant from
    // another and reports a number that never moves however far the phaser travels.
    [[nodiscard]] bool       phase_resid_valid(uint8_t i) const noexcept {
        return i < MAX_STREAMS && phase_resid_ok_[i]; }
    [[nodiscard]] AngleDeg10 phase_residual(uint8_t i) const noexcept {
        return i < MAX_STREAMS ? phase_resid_[i] : 0; }

    // WHEN THE NEXT EVENT ON THIS STREAM IS DUE, in ticks after the last one, from the decoder's own
    // schedule: the interval just measured, scaled by the ratio of the next slot's angular span to
    // the one just traversed. 0 while unlocked (nothing to predict from) or for WIDTH (identified by
    // pulse length, not by when it arrives).
    //
    // Angular, deliberately. The tooth after a gap is due ONE PITCH later even though the interval
    // just measured was a whole gap, so any "about the same as last time" estimate is wrong exactly
    // at the gap, which is the one place on the wheel where being wrong is expensive.
    [[nodiscard]] uint32_t expected_next_ticks(uint8_t idx) const noexcept {
        if (idx >= n_) return 0;
        const Stream& s = st_[idx];
        if (!s.enabled || s.last_period_ == 0) return 0;
        const AngleDeg10 pl = s.pitch(), pn = s.next_pitch();
        if (pl <= 0 || pn <= 0) return 0;
        return static_cast<uint32_t>((static_cast<uint64_t>(s.last_period_) *
                                      static_cast<uint32_t>(pn)) / static_cast<uint32_t>(pl));
    }
    // The same question for the stream that carries POSITION — the one whose silence means the
    // engine has stopped, as opposed to a cam-rate stream that is quiet between pulses by design.
    [[nodiscard]] uint32_t expected_fine_ticks() const noexcept {
        return (fine_idx_ < n_) ? expected_next_ticks(fine_idx_) : 0; }
    [[nodiscard]] uint8_t  fine_index() const noexcept { return fine_idx_; }

    // Record an anomaly the DECODER could not see for itself because it is an absence — a tooth that
    // never arrived, a wheel that stopped. The owner of the clock calls this.
    void note_kind(TriggerErrorKind kind) noexcept { record_error(kind); }
    [[nodiscard]] uint8_t    last_error_kind()   const noexcept { return last_err_kind_; }
    // PLL feed: after on_edge(), true if THIS edge was the fine (velocity) source — the caller
    // then feeds the VirtualTrigger (angle, tick, velocity, fine_pitch). pll_angle() is the
    // angle in the PLL's current cycle (mod 360° at CRANK, full 720° at PHASE).
    [[nodiscard]] bool       fine_advanced() const noexcept { return fine_advanced_; }
    [[nodiscard]] AngleDeg10 fine_pitch()    const noexcept { return fine_pitch_; }
    // The match window of the stream that carries position, so the deadline for a tooth that never
    // arrives can use the SAME tolerance the matcher applies to one that does.
    // HALF A TOOTH PITCH, IN TICKS, on the stream that carries position. This is the tolerance the
    // matcher allows an edge that DOES arrive, so the deadline for one that never does is judged by
    // the same rule: a tooth is invalid precisely when nothing lands between half and one and a half
    // pitches of where it was due.
    [[nodiscard]] uint32_t   fine_tol_ticks() const noexcept {
        return (fine_idx_ < n_) ? (st_[fine_idx_].pitch_ticks() / 2u) : 0u; }
    [[nodiscard]] AngleDeg10 pll_angle()     const noexcept {
        return (level_ == SyncLvl::PHASE) ? angle_ : static_cast<AngleDeg10>(angle_ % ANGLE_360); }
    // Revolutions per engine cycle (1 / 2 / 3) — the scheduler's grid uses it to size the cycle.
    [[nodiscard]] uint8_t revs_per_cycle() const noexcept { return revs_; }

    // Per-stream readout for VVT: a CAM stream that is locked AND absolute reports its
    // measured absolute angle (0..7200). The caller diffs it against the cam's nominal
    // angle to get the cam advance. Returns false for streams that aren't yet resolved.
    [[nodiscard]] bool stream_locked_absolute(uint8_t i) const noexcept {
        return i < n_ && st_[i].enabled && st_[i].locked() && st_[i].absolute(); }
    [[nodiscard]] AngleDeg10 stream_angle(uint8_t i) const noexcept {
        return i < n_ ? st_[i].angle() : 0; }

private:
    struct Stream {
        bool enabled = false, primed = false;
        StreamRepeats repeats = 2;          // per engine cycle; period_ = cycle / repeats
        AngleDeg10    period_ = ANGLE_360;  // this pattern's angular span
        PrimKind   kind = PrimKind::GAP;
        GapMatcher gap; SequenceMatcher seq; WidthMatcher width;
        uint32_t   last = 0, last_period_ = 0;
        uint8_t    window_pct_ = 25;        // this stream's match tolerance (phase timeout reads it)
        uint8_t    win_run_   = 50;         // match tolerance at speed, % of one tooth pitch
        uint8_t    win_crank_ = 75;         // ...and while cranking, blended by rpm (see set_rpm)
        AngleDeg10 nominal_   = 0;          // where this cam's reference sits with the phaser PARKED
        AngleDeg10 authority_ = 0;          // how far a phaser may move it from there (crank degrees)
        AngleDeg10 allowance_ = 0;          // FIXED cams: mechanical slop the trigger layer cannot derive
        bool       phased_    = false;      // is a phaser able to move it at all?
        bool       lead_rising_ = true, lead_primed_ = false;
        uint32_t   lead_tick_ = 0;
        uint8_t    gidx_[MAX_ANOMALIES] = {}, gratio_[MAX_ANOMALIES] = {};   // GAP cell (copied)
        AngleDeg10 cell_[MAX_PATTERN_TEETH] = {};                            // SEQUENCE cell (copied)
        bool       locked()   const noexcept {
            return kind == PrimKind::GAP ? gap.is_locked()
                 : kind == PrimKind::SEQUENCE ? seq.is_locked() : width.is_locked(); }
        bool       absolute() const noexcept {
            return kind == PrimKind::GAP ? gap.is_absolute()
                 : kind == PrimKind::SEQUENCE ? seq.is_absolute() : width.is_absolute(); }
        AngleDeg10 angle()    const noexcept {
            return kind == PrimKind::GAP ? gap.angle()
                 : kind == PrimKind::SEQUENCE ? seq.angle() : width.angle(); }
        AngleDeg10 pitch()    const noexcept {
            return kind == PrimKind::GAP ? gap.pitch()
                 : kind == PrimKind::SEQUENCE ? seq.pitch() : 0; }
        // Ticks per one tooth pitch (0 = no estimate). GAP knows it exactly; for SEQUENCE the cells
        // differ so the last interval is the best single number available.
        uint32_t pitch_ticks() const noexcept {
            return kind == PrimKind::GAP ? gap.pitch_ticks() : last_period_; }
        // Span of the interval expected NEXT (0 = unlocked, or WIDTH: no prediction to make).
        AngleDeg10 next_pitch() const noexcept {
            return kind == PrimKind::GAP ? gap.next_pitch()
                 : kind == PrimKind::SEQUENCE ? seq.next_pitch() : 0; }
        // IS THIS EVENT THE PATTERN'S SYNC? A cam may carry any number of teeth, but only one place
        // in its pattern identifies the cycle, and that is the thing that has to keep landing where
        // the crank says it should. For a SEQUENCE that is the cell origin; for a WIDTH stream the
        // reference pulse is the only pulse the matcher accepts at all, so every one of them is it.
        bool at_origin() const noexcept {
            return kind == PrimKind::SEQUENCE ? (seq.index() == 0)
                 : kind == PrimKind::WIDTH    ? width.matched()
                                              : (gap.index() == 0); }
        // Present teeth in ONE period of this stream (what actually arrives, gaps excluded).
        uint16_t present_teeth() const noexcept {
            return kind == PrimKind::GAP ? gap.present() : 0; }
        // The engine angle of the edge being processed RIGHT NOW. For GAP and SEQUENCE that is
        // angle(), which advances per event. WIDTH is the exception: it is fed at the trailing edge
        // but reports the leading edge's angle, so anchoring on angle() lands a whole pulse early.
        AngleDeg10 event_angle() const noexcept {
            return kind == PrimKind::WIDTH ? width.edge_angle() : angle(); }
    };

    bool any_locked() const noexcept {
        for (int i = 0; i < n_; ++i) if (st_[i].enabled && st_[i].locked()) return true;
        return false;
    }

    // A stream's period falls out of its repeat count. Clamped to at least one repeat and to the
    // cycle, because a zero would divide by nothing and a period longer than the cycle cannot exist.
    void set_repeats(Stream& s, StreamRepeats r) noexcept {
        s.repeats = r ? r : REPEATS_PHASE;
        s.period_ = static_cast<AngleDeg10>(cycle_ / s.repeats);
        if (s.period_ == 0) s.period_ = cycle_;
    }

    // `tick` is the timestamp of the edge that caused this fuse. It is passed rather than read
    // back off the stream because a WIDTH stream never assigns s.last (it returns at the
    // trailing edge without touching it), so the cam that most needs an accurate anchor instant
    // is exactly the one whose stored tick is stale.
    void fuse(uint8_t idx, uint32_t tick) noexcept {
        Stream& s = st_[idx];

        // --- PHASE stream (repeats == 1: the pattern spans the whole cycle) ---
        //
        // Taken when the matcher is LOCKED, whether or not it resolved an ABSOLUTE origin. A wheel
        // with no anomaly — an even distributor — locks relative: it knows its pitch and counts, but
        // nothing in it says which pulse is the first. That is not a defect in the decoder, it is the
        // wheel: a uniform pattern carries no cylinder identity and no amount of decoding invents one.
        // Ford put a narrower signature pulse in the PIP distributor precisely because a uniform one
        // cannot be identified.
        //
        // We take it anyway and call the arbitrary origin the cycle origin. The phase is ASSERTED, not
        // measured, and it is still worth having: sequential injection on an unidentified phase does
        // not match the engine's real cylinder order, but it splits delivery across the cycle instead
        // of dumping a whole cycle's fuel in one pulse — which is a real reduction in peak demand on
        // the fuel system. Ignition is unaffected: a distributor's rotor does the sequencing
        // mechanically, so the ECU only ever needed to know that a pulse happened.
        if (s.repeats == REPEATS_PHASE && s.locked()) {
            // rpm-band gate: outside the trusted band (set by the HAL from live rpm) ignore the cam edge
            // entirely — for acquisition AND correction. rev_known_ is left as-is, so an already-locked
            // phase is retained on crank counting; a VR cam's low-rpm glitch can neither resolve nor
            // mis-correct the revolution. Band disabled (default) leaves phase_stream_valid_ = true.
            if (!phase_stream_valid_) return;
            const AngleDeg10 A = s.angle();              // aligned absolute 0..cycle
            uint8_t this_rev = static_cast<uint8_t>(A / ANGLE_360);
            if (this_rev >= revs_) this_rev = static_cast<uint8_t>(revs_ ? revs_ - 1 : 0);

            // A SECOND CAM DOES NOT GET TO SAY WHICH REVOLUTION IT IS. It is still decoded, still
            // position-checked against its own bank's nominal, still measured for VVT — but the cycle
            // has one spokesman, or the answer depends on which edge happened to land last.
            //
            // THE CROSS-CHECK IS THE POSITION CHECK, not a comparison of the cams to each other.
            // Comparing the revolution each cam reports would false-fault every normal V engine:
            // cams sit at different points in the cycle by design, so bank 1 firing in revolution 0
            // and bank 2 firing in revolution 1 is the arrangement working, not a fault. What every
            // cam CAN be held to is the same engine position — each asserts "the engine is at my
            // nominal, give or take my phaser" against the crank's own answer. A bank whose chain has
            // moved fails that while the other bank passes, which is the per-bank attribution wanted,
            // and it needs no cam-to-cam comparison at all.
            if (static_cast<int>(idx) != nominated_phase_stream()) {
                if (s.at_origin()) check_phase_position(s, idx, /*demote*/false, tick);
                return;
            }
            // WHICH revolution of the cycle we are in. Two for a four-stroke, ONE for a two-stroke,
            // THREE for a rotary — so this is a counter, not the bit it used to be: a rotary's three
            // e-shaft revolutions cannot be told apart by a single flag, and without that its three
            // rotor faces are indistinguishable.
            // Where the crank knows its own position, the revolution comes from the crank (below), and
            // only on an edge whose angle IS now. A WIDTH cam reports its configured target on every
            // edge, so on a decoy pulse "where the cam is" says nothing — and taking the revolution
            // from it moved the whole engine whenever a decoy sat in the other revolution.
            const bool crank_decides = any_crank_absolute() && primed_fine_ && fine_period_ > 0 &&
                                       fine_repeats_ > 1 && cycle_ > 0;
            const bool angle_is_now  = s.at_origin() || s.kind != PrimKind::WIDTH;
            const uint8_t keep_rev = rev_, keep_rep = rep_;
            rev_ = this_rev;
            // WHICH repeat of the periodic stream's own period the cycle is at. For an ordinary crank
            // wheel the period IS a revolution and this equals rev_; for a SYMMETRICAL wheel it is
            // finer — a Renix pattern recurring every 180 deg has four, and knowing the revolution
            // would still leave it a half-period out.
            if (fine_period_ > 0) {
                rep_ = static_cast<uint8_t>(A / fine_period_);
                if (fine_repeats_ && rep_ >= fine_repeats_)
                    rep_ = static_cast<uint8_t>(fine_repeats_ - 1);
                // ASK THE CRANK, NOT JUST THE CAM'S NAMEPLATE. A is where the cam is CONFIGURED to be,
                // and the division above picks the repeat from that alone. A cam edge set near a
                // revolution boundary (358 deg), or moved across one by its phaser, arrives after the
                // crank has already wrapped — the crank reads 2 deg, the nameplate says revolution 0,
                // and the engine is placed 360 deg out: PHASE_LOST a cycle later, re-acquire, repeat.
                // Where the crank knows its own position, take the repeat that puts the CRANK's angle
                // nearest the cam's: right for any cam error under half a period.
                if (crank_decides && !angle_is_now) {
                    rev_ = keep_rev; rep_ = keep_rep;           // a decoy: it moves nothing
                } else if (crank_decides) {
                    int32_t best = -1, best_d = 0;
                    for (uint8_t r = 0; r < fine_repeats_; ++r) {
                        int32_t d = (static_cast<int32_t>(prev_fine_) + r * fine_period_ - A) % cycle_;
                        if (d < 0) d += cycle_;
                        if (d > cycle_ / 2) d = cycle_ - d;
                        if (best < 0 || d < best_d) { best = r; best_d = d; }
                    }
                    rep_ = static_cast<uint8_t>(best);
                    rev_ = static_cast<uint8_t>(revs_ ? ((static_cast<uint32_t>(rep_) * fine_period_ +
                                                         static_cast<uint32_t>(prev_fine_)) / ANGLE_360) % revs_ : 0);
                }
            }
            rev_known_ = true;
            // WHERE ON THE CRANK THE CAM SYNC LANDED. Not when — where. The cam's sync recurs once
            // per engine cycle, so on a 60-2 four-stroke it must fall every 116 present crank teeth,
            // at the same place every time. Counting teeth rather than microseconds costs no timer
            // (the crank IS the clock), is rpm-invariant by construction, ignores however many other
            // teeth the cam happens to carry, and catches the thing a time window cannot see at all:
            // a sync that arrives PUNCTUALLY BUT IN THE WRONG PLACE. Half a cycle out is a slipped
            // chain or an inverted cam; "roughly on time" says nothing about it.
            if (s.at_origin()) {
                check_phase_position(s, idx, /*demote*/true, tick);
                phase_mark_   = crank_teeth_;    // liveness is measured on the cycle's spokesman
                phase_marked_ = true;
            }
            if (!has_crank_) {                            // cam-only sensor: cam is everything
                prev_fine_ = static_cast<AngleDeg10>(A % ANGLE_360); primed_fine_ = true;
                velocity_  = s.last_period_; fine_pitch_ = s.pitch(); fine_advanced_ = true;
                fine_idx_  = idx;            // whose prediction the tooth deadline is armed from
                angle_ = A; level_ = SyncLvl::PHASE; return;
            }
            // THE CAM IS THE ONLY ABSOLUTE REFERENCE AN EVEN WHEEL HAS. Where the crank can fix its
            // own position the cam adds only WHICH REVOLUTION; where it cannot, the cam must supply
            // the within-revolution angle too, or nothing in the engine ever supplies it. Queued
            // rather than applied: the cam edge falls between two crank teeth, and the anchor is
            // taken on the next one so the position advances in whole teeth.
            // A PHASED CAM CANNOT ANCHOR. An anchor has to be a fixed reference and a phaser is a
            // controlled variable — anchoring a featureless crank to one would inject the phaser's
            // current displacement into the engine's position, silently, and change it every time the
            // cam moved. A crank with no unique feature whose only absolute source is phased can
            // never know where it is; that configuration belongs in the reject list, not running.
            if (s.absolute() && !s.phased_ && has_crank_ && !any_crank_absolute()) {
                pending_anchor_       = true;
                pending_anchor_angle_ = s.event_angle();   // where we are NOW, not where the pulse began
                pending_anchor_tick_  = tick;              // and WHEN, so the carry to the next tooth is measured
            }
            // Recompute the angle from the crank ONLY where the crank can state its own position.
            // On a relative wheel prev_fine_ is the arbitrary origin the matcher happened to lock,
            // so doing this there would overwrite the anchored angle with the very number the anchor
            // exists to replace — on every cam edge, for ever.
            if (anchored_ && any_crank_absolute())
                angle_ = static_cast<AngleDeg10>(prev_fine_ + rep_ * fine_period_);
            if (any_crank_locked() && anchored_) level_ = SyncLvl::PHASE;
            return;
        }

        // --- PERIODIC stream (repeats >= 2): position within its own period ---
        // Its pattern recurs more than once per cycle, so it can say where it is inside one repeat
        // and nothing more; which repeat needs a phase stream. For the common crank wheel the period
        // IS a revolution, which is why the rev counter below reads in 360s.
        if (s.repeats > REPEATS_PHASE && s.locked()) {
            // A SECOND CRANK STREAM IS NOT A SECOND CLOCK. It does not advance the position, supply
            // the velocity or set the fine pitch — one stream does all of that or they fight over it.
            // What it CAN do, if it can state its own position, is anchor: that is exactly what a
            // Motronic flywheel's TDC mark and a GM 3800's low-resolution track are for, and both sit
            // beside a fine track that cannot locate itself at all.
            if (static_cast<int>(idx) != nominated_crank_stream()) {
                if (s.absolute() && s.at_origin()) {
                    pending_anchor_       = true;
                    pending_anchor_angle_ = s.event_angle();
                    pending_anchor_tick_  = tick;
                }
                return;
            }
            // Tooth-error accounting (sync-health): count every crank tooth for the error-% denominator;
            // per-matcher misses (gap/sequence) are tallied by note_error() after each feed, snapshot per
            // rev here. A TOLERATED miss is still recorded — never silently forgotten (EngineProtection
            // turns errors into a DTC).
            ++tooth_counter_; ++teeth_cycle_; ++crank_teeth_;
            // HOW MANY CRANK TEETH A CYCLE HOLDS, from the wheel itself rather than from config:
            // the present teeth in one of this stream's periods, times the periods in a cycle.
            if (const uint16_t per = s.present_teeth())
                phase_expect_teeth_ = static_cast<uint16_t>(per * (s.repeats ? s.repeats : 1));
            // THE CAM IS OVERDUE, measured on the crank. Past its slot by more than a tooth it has
            // not merely drifted, it has not come — and phase that is not still being proven is
            // phase the decoder is only remembering. Costs PHASE and nothing else: the crank is
            // proving itself tooth by tooth, so the engine keeps running wasted-spark / batch.
            if (rev_known_ && phase_marked_ && phase_expect_teeth_ && phase_stream_valid_ &&
                static_cast<uint16_t>(crank_teeth_ - phase_mark_) > phase_expect_teeth_ + 1) {
                record_error(TriggerErrorKind::PHASE_LOST);
                rev_known_    = false;
                phase_marked_ = false;
                if (level_ == SyncLvl::PHASE) level_ = SyncLvl::CRANK;
            }
            // The fine source is whichever periodic stream is feeding us; remember its shape so the
            // phase stream can place it. This used to be hardcoded to 360, which is only true when
            // the pattern happens to recur once per revolution.
            fine_period_  = s.period_;
            fine_repeats_ = s.repeats;
            const AngleDeg10 nf = static_cast<AngleDeg10>(s.angle() % s.period_);
            if (primed_fine_ && nf < prev_fine_) {            // wrapped its period → next repeat
                rep_ = static_cast<uint8_t>(s.repeats ? (rep_ + 1) % s.repeats : 0);
                rev_ = static_cast<uint8_t>(revs_ ? static_cast<uint8_t>(
                           (static_cast<uint32_t>(rep_) * s.period_) / ANGLE_360) % revs_ : 0);
                teeth_last_cycle_  = teeth_cycle_;            // snapshot the just-completed period
                errors_last_cycle_ = errors_cycle_;
                teeth_cycle_ = 0; errors_cycle_ = 0; tooth_counter_ = 0;
            }
            prev_fine_ = nf; primed_fine_ = true;
            velocity_  = s.last_period_; fine_pitch_ = s.pitch(); fine_advanced_ = true;
            fine_idx_  = idx;                // whose prediction the tooth deadline is armed from
            // ---- POSITION: set by an ANCHOR, advanced by INCREMENTS ---------------------------
            // A stream that is locked but not ABSOLUTE knows how fast the engine is turning and how
            // far it has moved. It does not know where the engine is, and no amount of counting will
            // tell it. This used to read `angle_ = nf + rep_*period` for every wheel, which takes the
            // stream's position within its own pattern to BE the engine's position — true only for a
            // wheel with a unique feature. On an even wheel the pattern origin is whichever tooth
            // happened to lock, so the reported angle was out by an arbitrary number of teeth,
            // for ever, at PHASE, with full confidence. Measured on a 24-tooth even wheel: 45 deg on
            // a clean run, 165 deg after one missing tooth re-locked it somewhere else.
            if (s.absolute()) {
                // The wheel names its own position every tooth. Nothing to carry, nothing to drift.
                anchored_ = true;
                angle_    = static_cast<AngleDeg10>(nf + rep_ * s.period_);
            } else if (sync_always_) {
                // Every tooth is a sync tooth: this one anchors, and the revolution is not a question.
                anchored_  = true;
                rev_known_ = true;
                angle_     = static_cast<AngleDeg10>(nf + rep_ * s.period_);
            } else if (pending_anchor_) {
                // An absolute source (a fixed-phase cam, or an absolute crank-rate reference) said
                // where the engine was, at a KNOWN INSTANT inside the pitch that ends at THIS tooth.
                //
                // THE POINT IS NOT ACCURACY, IT IS ORDER-INDEPENDENCE. This used to add half a pitch,
                // reasoning that the sub-tooth remainder was unknowable. The remainder is knowable —
                // it is elapsed time against the pitch that just closed — and guessing it made the
                // anchor depend on WHICH tooth applied it. When the reference edge lands on a crank
                // tooth, whether the ISR services the cam first or the tooth first decides between
                // applying on that tooth or the next, and those answers are a WHOLE PITCH apart:
                // measured at 15.0 deg on a 24-tooth even wheel and 2.0 deg on a 180-slit Nissan,
                // flipped by a single tick. A constant offset a tuner can cancel with a timing light;
                // an offset that alternates between two values, they cannot.
                //
                // Measuring the carry makes both orderings agree, because applying one tooth later
                // also measures one pitch more of travel.
                AngleDeg10 adv;
                const uint32_t d_raw = tick - pending_anchor_tick_;
                if (s.last_period_ == 0 || fine_pitch_ <= 0 || d_raw / 2u >= s.last_period_) {
                    // No period to scale against, or the reference is far older than the pitch that
                    // just closed and so was not inside it. Better a coarse true angle than a precise
                    // invented one — fall back to centring, which is what this always did.
                    adv = static_cast<AngleDeg10>(fine_pitch_ > 0 ? fine_pitch_ / 2 : s.pitch() / 2);
                } else {
                    // Clamped to one pitch: a reference exactly on the previous tooth is a full
                    // pitch of travel and no more, and the clamp is also what bounds the product.
                    const uint32_t d = d_raw > s.last_period_ ? s.last_period_ : d_raw;
                    // 32-BIT ON PURPOSE, exactly as EventScheduler::angle_at and for the same reason:
                    // a uint64_t divide pulls __aeabi_uldivmod (~40-90 cycles of SOFTWARE divide) onto
                    // the priority-1 capture path that stamps every trigger edge. The clamp bounds the
                    // product by last_period_ * fine_pitch_ — 1.8e9 at the 30 rpm floor on the coarsest
                    // even crank there is (2 teeth, a 180 deg pitch), inside 2^32. The M7's hardware
                    // UDIV does the rest.
                    adv = static_cast<AngleDeg10>((d * static_cast<uint32_t>(fine_pitch_)) / s.last_period_);
                }
                angle_ = wrap_cycle(static_cast<int32_t>(pending_anchor_angle_) + adv);
                pending_anchor_ = false;
                anchored_       = true;
            } else if (anchored_) {
                angle_ = wrap_cycle(static_cast<int32_t>(angle_) + s.pitch());
            }
            // The level asserts what is KNOWN, not that something locked.
            level_ = !anchored_ ? SyncLvl::NONE
                   : rev_known_ ? SyncLvl::PHASE
                                : SyncLvl::CRANK;
            return;
        }

        // THE HIERARCHY. Losing the crank takes phase with it and everything else: without a crank
        // stream there is no position at all, and a cam that is still locked can tell you WHICH
        // revolution of a cycle you cannot otherwise locate. This used to test any_locked(), which a
        // still-locked cam satisfies — so a crank that unlocked left level_ reading PHASE, unchanged,
        // because neither branch above runs for an unlocked stream. Phase sync outliving the crank
        // sync it is measured against is the most dangerous state the decoder can be in: it reports
        // full confidence in a position nothing is still measuring.
        if (has_crank_ ? !any_crank_locked() : !any_locked()) {
            // RESET TO THE INITIAL STATE, not "drop the level and keep the rest". This cleared the
            // level and the anchor and left the last ANGLE and VELOCITY standing — the same shape as
            // every other stale value this decoder has been cured of, and the same excuse for it
            // ("nothing reads them once the level is NONE"). Something always does eventually.
            // Whatever was believed about position is discarded rather than repaired: an anomaly
            // says the belief was wrong, and there is nothing in it worth carrying forward.
            level_ = SyncLvl::NONE; rev_known_ = false; primed_fine_ = false; phase_marked_ = false;
            anchored_ = false; pending_anchor_ = false;
            angle_ = 0; velocity_ = 0; fine_pitch_ = 0;
        }
    }



    // Can any crank-rate stream fix position by itself? A wheel with a unique feature can; an even
    // one cannot, and that difference is the whole of the model. Every matcher has always computed
    // it (is_absolute()) and the fusion has always thrown it away.
    AngleDeg10 wrap_cycle(int32_t a) const noexcept {
        const int32_t c = cycle_ ? cycle_ : ANGLE_720;
        while (a < 0)  a += c;
        while (a >= c) a -= c;
        return static_cast<AngleDeg10>(a);
    }

    // WHICH CAM SPEAKS FOR THE CYCLE. Lowest enabled, locked phase-rate slot — and since the slot
    // index IS the role, that is Cam Intake B1 wherever it exists, falling back in a defined order
    // rather than to whichever edge happened to arrive last. On a V engine with a cam per bank, both
    // can identify the cycle and both are moving independently under their own phasers; letting the
    // most recent edge win means the answer silently depends on scheduling.
    // WHO PROVIDES THE ANGLE. One crank stream is the clock — it supplies velocity, it advances the
    // position, and its pitch is what everything is measured in. Lowest enabled, locked crank slot,
    // and since the slot IS the role that is Crank Primary, which is what "primary" means.
    //
    // Every locked crank stream used to do all of this, so with two of them the velocity, the fine
    // pitch and the position increments came from whichever edge landed last — two clocks fighting
    // over one position. A second crank sensor is common and it is not a second clock: on a Motronic
    // flywheel it is a single TDC mark, on a GM 3800 a coarse asymmetric track. Its job is to ANCHOR,
    // which is a different capability, and it is handled as one.
    int nominated_crank_stream() const noexcept {
        for (int i = 0; i < n_; ++i)
            if (st_[i].enabled && st_[i].repeats > REPEATS_PHASE && st_[i].locked()) return i;
        return -1;
    }

    int nominated_phase_stream() const noexcept {
        for (int i = 0; i < n_; ++i)
            if (st_[i].enabled && st_[i].repeats == REPEATS_PHASE && st_[i].locked()) return i;
        return -1;
    }

    bool any_crank_absolute() const noexcept {
        for (int i = 0; i < n_; ++i)
            if (st_[i].enabled && st_[i].repeats > REPEATS_PHASE &&
                st_[i].locked() && st_[i].absolute()) return true;
        return false;
    }

    bool any_crank_locked() const noexcept {
        for (int i = 0; i < n_; ++i)
            if (st_[i].enabled && st_[i].repeats > REPEATS_PHASE && st_[i].locked()) return true;
        return false;
    }

    // Tally a tolerated matcher miss (GAP → gap-presence, SEQUENCE → window violation) into the per-rev
    // error counters. Covers crank AND cam streams; WIDTH is a pure identifier with no miss to report.
    // The kind names WHAT WENT WRONG. It used to name which primitive noticed — GAP always reported
    // GAP_MISMATCH, SEQUENCE always TOOTH_WINDOW — which made TOOTH_WINDOW unreachable on every
    // missing-tooth wheel there is, and the P-code gated on it dead code. GapMatcher now says which
    // of its two faults it saw, and this passes that through.
    // WHERE DOES THE CRANK SAY WE ARE, at this cam's sync? One question serving the position check
    // and the VVT measurement both, because the residual between the crank's position and where this
    // cam's reference is parked IS the cam's displacement. A fixed cam must land within a tooth of
    // nominal; a phased one within its phaser's travel, because there that movement is the phaser
    // doing its job and a fixed expectation would drop phase continuously on a healthy VVT engine.
    //
    // Run for EVERY locked cam, not only the cycle's spokesman: a chain is per bank, so each bank's
    // cam answers for its own, and the fault names the stream that raised it.
    //
    // nominal_ == 0 means "not stated" and disables the check rather than faulting every tune that
    // never filled it in.
    // `demote` says whether failing this check costs PHASE. It does for the cycle's spokesman, whose
    // own trustworthiness is what phase rests on. It does NOT for another bank's cam: the engine's
    // position still comes from the crank and the spokesman, both of which are fine, so a jumped
    // chain on bank 2 is a mechanical fault to report — loudly, naming the bank — and not a reason to
    // stop injecting sequentially on a position that is still correct. Whether THAT should cut the
    // engine is a protection policy, decided from the fault, not something the decoder rules on by
    // throwing away knowledge it still has.
    void check_phase_position(const Stream& s, uint8_t idx, bool demote, uint32_t tick) noexcept {
        if (!has_crank_ || !anchored_ || !phase_stream_valid_ || s.nominal_ == 0) return;
        // THE CRANK ANGLE AT THE CAM EDGE, not at the last tooth before it. angle_ only advances on
        // a crank tooth, so comparing the cam against it quantised this measurement to a whole crank
        // pitch — and it rounded in the FAULT'S FAVOUR. On a 36-1 that meant 2, 5 and 9 degrees of
        // chain stretch all measured 0.0 and were invisible, while 45 degrees measured 40.0 and
        // passed a 40 degree band. Interpolating with the same elapsed-time arithmetic the anchor
        // uses makes the residual mean what it says, which is the whole point of recording it.
        AngleDeg10 crank_now = angle_;
        if (fine_idx_ < MAX_STREAMS && velocity_ > 0 && fine_pitch_ > 0) {
            const uint32_t d_raw = tick - st_[fine_idx_].last;
            if (d_raw / 2u < velocity_) {            // plausible: inside the pitch that is open
                const uint32_t d = d_raw > velocity_ ? velocity_ : d_raw;   // clamp; also the bound
                crank_now = wrap_cycle(static_cast<int32_t>(angle_)
                          + static_cast<int32_t>((d * static_cast<uint32_t>(fine_pitch_)) / velocity_));
            }
        }
        // nominal_ names where the REFERENCE edge sits, the same edge the matcher's angle() reports.
        // A WIDTH stream is fed at the trailing edge, a whole pulse later, so compare like with like
        // or every width cam reads a pulse-width out.
        const int32_t ref = static_cast<int32_t>(s.nominal_)
                          + (static_cast<int32_t>(s.event_angle()) - static_cast<int32_t>(s.angle()));
        int32_t resid = static_cast<int32_t>(crank_now) - ref;
        const int32_t half = cycle_ / 2;
        while (resid >  half) resid -= cycle_;
        while (resid < -half) resid += cycle_;
        // THIS RESIDUAL IS THE CAM'S DISPLACEMENT, and it is worth keeping whether or not it passes
        // the band: how far the cam has moved is the measurement, and "too far" is a judgement about
        // it. Recorded here because this is the only place that computes it against the CRANK's
        // position rather than against what the matcher was told to assert — which is why it works
        // for a WIDTH stream, whose angle() is the configured target and therefore never moves.
        if (idx < MAX_STREAMS) { phase_resid_[idx] = static_cast<AngleDeg10>(resid);
                                 phase_resid_ok_[idx] = true; }
        // TWO DIFFERENT THINGS BOUND TWO DIFFERENT CAMS. A phaser's travel is authority the ECU
        // commands. A fixed cam's wander is slop it does not: chain stretch, gear lash, torsional
        // wind-up under load — engine properties nothing in the trigger layer can derive, so they
        // are configured rather than computed.
        //
        // The fixed band is the LARGER of the decoder's own measurement floor and that configured
        // allowance. Deriving it from fine_pitch_ ALONE was the structural defect: pitch is a
        // property of the crank wheel, not of the cam drive being checked, so a coarse crank
        // silently disabled cam validation entirely — a 6g72's three teeth make a 120 deg band, wide
        // enough for the cam to slip a whole tooth and pass.
        //
        // THE FLOOR IS DELIBERATELY STILL A PITCH, and is not tightened here even though the anchor
        // is now exact enough to justify it. Tightening is what manufactures false PHASE_LOST on
        // running engines, and a spurious phase loss is worse than a wide band. phase_resid_[]
        // already records the displacement on every cam edge; the number should be chosen from that
        // distribution across real engines and real thermal and load conditions, not guessed here.
        // Until then allowance defaults to 0 and this evaluates exactly as it always did.
        const int32_t floor = fine_pitch_ ? fine_pitch_ : ANGLE_360 / 36;
        const int32_t band  = s.phased_ ? s.authority_
                                        : (floor > s.allowance_ ? floor : s.allowance_);
        if (resid <= band && resid >= -band) return;
        record_error(TriggerErrorKind::PHASE_LOST, idx);
        if (!demote) return;
        rev_known_ = false;
        if (level_ == SyncLvl::PHASE)
            level_ = any_crank_locked() ? SyncLvl::CRANK : SyncLvl::NONE;
    }

    void note_error(const Stream& s) noexcept {
        TriggerErrorKind kind = TriggerErrorKind::NONE;
        if      (s.kind == PrimKind::GAP      && s.gap.errored()) kind = s.gap.err_kind();
        else if (s.kind == PrimKind::SEQUENCE && s.seq.errored()) kind = TriggerErrorKind::TOOTH_WINDOW;
        if (kind == TriggerErrorKind::NONE) return;
        record_error(kind);
    }

    void record_error(TriggerErrorKind kind, uint8_t stream) noexcept {
        last_err_stream_ = stream;
        record_error(kind);
    }

    void record_error(TriggerErrorKind kind) noexcept {
        ++errors_cycle_; ++err_total_;
        if      (kind == TriggerErrorKind::NOISE_EDGE)   ++noise_total_;
        else if (kind == TriggerErrorKind::MISSED_TOOTH) ++missed_total_;
        else if (kind == TriggerErrorKind::PHASE_LOST)   ++phase_lost_total_;
        last_err_tooth_ = tooth_counter_;
        last_err_kind_  = static_cast<uint8_t>(kind);
    }

    Stream     st_[MAX_STREAMS];
    uint8_t    n_ = 0;
    SyncLvl    level_ = SyncLvl::NONE;
    AngleDeg10 angle_ = 0;
    uint32_t   velocity_ = 0;
    volatile bool phase_stream_valid_ = true;
    bool       sync_always_ = false;   // one even wheel in distributor mode (set_sync_always)   // rpm-band gate for the cam stream (see set_phase_stream_valid)
    AngleDeg10 cycle_ = ANGLE_720;   // engine cycle span (set_cycle); cam-stream period
    uint8_t    revs_  = 2;           // revolutions per cycle = cycle_/360 (1 two-stroke, 3 rotary)
    uint8_t    rev_ = 0;             // WHICH revolution of the cycle we are in (0..revs_-1)
    // Which repeat of the FINE stream's period we are in, and that stream's shape. For an ordinary
    // crank wheel period == 360 and rep_ == rev_; a symmetrical wheel needs the finer index or the
    // assembled angle is a whole period out and confidently wrong.
    uint8_t    rep_ = 0;
    AngleDeg10 fine_period_ = ANGLE_360;
    uint8_t    fine_repeats_ = 2;
    bool       rev_known_ = false, primed_fine_ = false, has_crank_ = false;
    AngleDeg10 prev_fine_ = 0;
    AngleDeg10 fine_pitch_ = 0;   // fine-stream pitch this edge (PLL feed)
    uint8_t    fine_idx_   = 0xFF; // WHICH stream is the fine (position) source — the one whose
                                   // silence means the engine stopped. 0xFF until one advances.
    uint32_t   noise_total_ = 0, missed_total_ = 0, phase_lost_total_ = 0;
    AngleDeg10 phase_resid_[MAX_STREAMS] = {};    // measured cam displacement, per stream
    bool       phase_resid_ok_[MAX_STREAMS] = {};
    uint8_t    last_err_stream_ = 0xFF;   // which stream raised the last fault (see last_error_stream)   // free-running; see noise_total()/missed_total()
    // Phase liveness, counted in CRANK TEETH — the primary is the clock, so this needs no timer.
    // ANCHOR STATE. Position is established or it is not; there is no third answer.
    bool       anchored_ = false;             // has any absolute source fixed the engine angle?
    bool       pending_anchor_ = false;       // an absolute source spoke; apply on the next fine tooth
    AngleDeg10 pending_anchor_angle_ = 0;     // the engine angle it named
    uint32_t   pending_anchor_tick_  = 0;     // and the tick it named it AT (the carry is measured)
    uint16_t   crank_teeth_ = 0;         // free-running crank tooth count (wraps; only deltas are read)
    uint16_t   phase_mark_ = 0;          // the count at which the cam's sync last landed
    uint16_t   phase_expect_teeth_ = 0;  // how many crank teeth a whole engine cycle holds
    bool       phase_marked_ = false;
    bool       fine_advanced_ = false;
    // Sync-health counters (lean integers, ISR-written; the task reads + derives error %).
    uint16_t   tooth_counter_ = 0, teeth_cycle_ = 0, errors_cycle_ = 0;
    uint32_t   edges_total_ = 0;   // free-running: "teeth are arriving", sync or no sync
    uint16_t   teeth_last_cycle_ = 0, errors_last_cycle_ = 0, last_err_tooth_ = 0;
    uint32_t   err_total_ = 0;
    uint8_t    last_err_kind_ = 0;   // TriggerErrorKind of the last error
};
