#include "SegmentTimer.h"

void SegmentTimer::configure(const AngleDeg10* tdcs, uint8_t ncyl, AngleDeg10 cycle) noexcept {
    reset();
    nbound_ = 0;
    cycle_  = (cycle > 0) ? cycle : 7200;
    available_ = false;
    teeth_per_seg_ = 0.0f;
    if (!tdcs || ncyl == 0 || ncyl > MAX_CYL) return;

    // Boundaries are the firing TDCs, SORTED BY ANGLE. Cylinder order is firing order, which is not
    // angle order — so sorting once here is what lets the ISR do a single forward walk instead of
    // searching every tooth.
    for (uint8_t c = 0; c < ncyl; ++c) {
        const AngleDeg10 a = angle_wrap(tdcs[c], cycle_);
        uint8_t i = nbound_;
        while (i > 0 && bound_angle_[i - 1] > a) {        // insertion sort; ncyl <= 12
            bound_angle_[i] = bound_angle_[i - 1];
            bound_cyl_[i]   = bound_cyl_[i - 1];
            --i;
        }
        bound_angle_[i] = a;
        bound_cyl_[i]   = c;
        ++nbound_;
    }

    pitch_seen_ = 0;   // availability re-decides on the first tooth that reports a pitch
}

void SegmentTimer::reset() noexcept {
    open_idx_   = UNSET;
    open_tick_  = 0;
    last_angle_ = -1;
    head_ = tail_ = 0;
    segments_ = 0;
}

void SegmentTimer::on_tooth(AngleDeg10 angle, uint32_t tick, AngleDeg10 pitch) noexcept {
    if (nbound_ == 0) return;

    // AVAILABILITY, decided from the wheel the engine actually has. Teeth across one segment =
    // (teeth per cycle) / (segments per cycle). Below the floor the answer would be dominated by
    // where the teeth happen to fall relative to TDC rather than by what the engine did, so it
    // reports nothing at all. Re-decided whenever the measured pitch changes, so a reconfigure onto
    // a different wheel cannot leave a stale verdict behind.
    // A pitch that MOVES is normal, not a reconfiguration: the fine stream reports a wider span
    // across a missing-tooth gap, so on a 60-2 the value changes twice a revolution. Treating every
    // change as new geometry restarted the walk on those teeth and no segment ever closed — the walk
    // was abandoned more often than a segment is long. So the pitch only ever feeds the availability
    // VERDICT, and only a change in that verdict (a genuinely different wheel) resets the walk.
    if (pitch > 0 && pitch != pitch_seen_) {
        pitch_seen_ = pitch;
        const float tps = (static_cast<float>(cycle_) / static_cast<float>(pitch))
                        / static_cast<float>(nbound_);
        const bool now_ok = tps >= MIN_TEETH_PER_SEGMENT;
        // Keep the COARSEST pitch seen as the availability basis: the widest gap is the honest
        // worst case for how finely this wheel can time a segment.
        if (tps < teeth_per_seg_ || teeth_per_seg_ == 0.0f) teeth_per_seg_ = tps;
        if (now_ok != available_) {
            available_ = now_ok;
            open_idx_ = UNSET;      // the span in flight was measured under the old verdict
            last_angle_ = -1;
        }
    }
    if (!available_) return;
    const AngleDeg10 a = angle_wrap(angle, cycle_);

    if (last_angle_ < 0) {                 // first tooth after a reset: nothing to span yet
        last_angle_ = a;
        return;
    }

    // Which boundaries lie in (last_angle_, a]? Wrap-aware, and a tooth can cross more than one on a
    // coarse wheel — each crossing closes the segment that was open and opens the next.
    const AngleDeg10 prev = last_angle_;
    last_angle_ = a;
    const bool wrapped = (a < prev);

    for (uint8_t i = 0; i < nbound_; ++i) {
        const AngleDeg10 b = bound_angle_[i];
        const bool crossed = wrapped ? (b > prev || b <= a)      // spans the cycle origin
                                     : (b > prev && b <= a);
        if (!crossed) continue;

        if (open_idx_ != UNSET) {
            // Close the segment that has been running since the previous boundary. The elapsed count
            // is unsigned-safe across a timebase wrap by construction.
            const uint32_t elapsed = tick - open_tick_;
            const uint8_t  h   = head_;
            const uint8_t  nxt = static_cast<uint8_t>((h + 1u) & (RING - 1u));
            if (nxt == tail_) {
                dropped_++;                 // consumer behind — drop, never stall the capture ISR
            } else {
                ring_[h].cyl   = bound_cyl_[open_idx_];
                ring_[h].ticks = elapsed;
                head_ = nxt;
            }
            segments_++;
        }
        open_idx_  = i;
        open_tick_ = tick;
    }
}

bool SegmentTimer::pop(Segment& out) noexcept {
    const uint8_t t = tail_;
    if (t == head_) return false;
    out   = ring_[t];
    tail_ = static_cast<uint8_t>((t + 1u) & (RING - 1u));
    return true;
}
