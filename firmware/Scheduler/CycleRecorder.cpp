#include "CycleRecorder.h"
#include <cstddef>
#include <cstring>

// ---------------------------------------------------------------------------
// enable / reset (task context)
// ---------------------------------------------------------------------------
void CycleRecorder::enable() noexcept {
    enabled_ = true;      // idempotent; history is deliberately kept
}

void CycleRecorder::reset() noexcept {
    enabled_ = false;     // stop the writers FIRST, then clear
    w_.store(0, std::memory_order_relaxed);
    trig_seq_ = grid_seq_ = 0;
    stall_w_  = 0;
    sel_valid_ = false;
    dropped_   = 0;
}

// ---------------------------------------------------------------------------
// on_cycle_boundary — LABELS ONLY.
//
// It records the rpm, sync level and span used to caption a capture, and counts cycles. It does not
// gate recording and does not decide which cycle an edge belongs to. That decision is what broke the
// previous design: it was made in this ISR (priority 2) and consumed in a higher-priority one.
//
// cycle_seq is a LABEL and is honestly so: it is incremented here, so an edge recorded microseconds
// either side of the boundary can be captioned with a neighbouring number. It identifies a frame for
// the user ("cycle 4821") and reveals how many cycles were skipped between captures. Nothing depends
// on it for correctness — membership comes from the angles.
// ---------------------------------------------------------------------------
void CycleRecorder::on_cycle_boundary(uint16_t index, AngleDeg10 cycle_angle,
                                      uint32_t rpm_x10, uint8_t sync_level) noexcept {
    if (index != 0) return;
    cycle_angle_ = cycle_angle;
    rpm_x10_     = rpm_x10;
    sync_level_  = sync_level;
}

// ---------------------------------------------------------------------------
// append — claim a slot, then fill it.
//
// Two interrupt priorities share this index (capture at 1, TIM5 at 2), so the claim is an atomic
// fetch-add: LDREX/STREX, where the preempting interrupt clears the exclusive monitor and the
// preempted claim simply retries. A plain read-modify-write would let both writers take the same slot
// and silently lose an edge — the exact class of failure this whole rewrite exists to remove.
//
// The store that follows the claim is not part of the atomic, but each writer owns its own slot, so
// they cannot collide. Only the newest entries can be claimed-but-unwritten, and the reader skips
// READ_GUARD of them.
//
// A full ring overwrites its oldest entry — that is what makes it a ring, and the reader only ever
// looks at the newest complete cycle.
// ---------------------------------------------------------------------------
void CycleRecorder::append(const CycleEdge& e) noexcept {
    const uint32_t slot = w_.fetch_add(1, std::memory_order_relaxed);
    ring_[slot % RING_MAX] = e;
}

// ---------------------------------------------------------------------------
// on_trigger_lost — write the gap into the ring.
//
// A barrier held in a variable would have to be compared against every window and would only
// remember the most recent stall. Recorded as an entry it is ordered with the data by construction,
// so any number of stalls are handled by the same backward walk that finds the markers.
// ---------------------------------------------------------------------------
void CycleRecorder::on_trigger_lost() noexcept {
    if (!enabled_) return;
    append(CycleEdge{ 0, 0, pack_flags(CycleSignal::Stall, false, false, 0) });
    stall_w_ = w_.load(std::memory_order_relaxed);
}

void CycleRecorder::record_output(CycleSignal sig, uint8_t channel,
                                  AngleDeg10 angle, bool active) noexcept {
    if (!enabled_) return;
    // Outputs are scheduled against the grid and written by the same ISR, so they carry the grid's
    // cycle number. They never open a cycle themselves.
    append(CycleEdge{ static_cast<uint16_t>(angle_wrap(angle, cycle_angle_)),
                      channel, pack_flags(sig, active, false, grid_seq_) });
}

void CycleRecorder::record_trigger(CycleSignal sig, uint8_t stream,
                                   AngleDeg10 angle, bool rising, bool cycle_start) noexcept {
    if (!enabled_) return;
    // Increment BEFORE stamping: the edge that opens a cycle belongs to the cycle it opens.
    if (cycle_start) ++trig_seq_;
    append(CycleEdge{ static_cast<uint16_t>(angle_wrap(angle, cycle_angle_)),
                      stream, pack_flags(sig, rising, cycle_start, trig_seq_) });
}

void CycleRecorder::record_virtual(AngleDeg10 angle, bool cycle_start) noexcept {
    if (!enabled_) return;
    // channel 0: there is exactly one grid. `active` is always true — a virtual tooth is an instant,
    // not a level, and the host draws event lanes as a comb.
    if (cycle_start) ++grid_seq_;
    append(CycleEdge{ static_cast<uint16_t>(angle_wrap(angle, cycle_angle_)),
                      0, pack_flags(CycleSignal::Virtual, true, cycle_start, grid_seq_) });
}

// ---------------------------------------------------------------------------
// lane_window — the newest COMPLETE cycle of ONE lane, bounded by that lane's own markers.
//
// An engine cycle is the physical trigger pattern occurring the stroke's number of times, and the
// decoder counts exactly that: the trigger lane is cut where its COUNTED angle wrapped, the grid lane
// at virtual tooth 0. Both markers are set by the ISR that recorded the edge, from data only it has,
// so there is no flag shared across interrupt priorities and nothing to race.
//
// Walking back from the newest entry, the first marker of this lane ENDS its last complete cycle and
// the second BEGINS it. Every other stream in that window is a whole number of its own periods,
// because the window is bounded by one period of a stream rather than by an angle guess.
//
// `w_snap` is the writer index sampled ONCE by the caller. The ISRs keep appending underneath, so the
// walk must never re-read w_: with a snapshot the window examined is fixed and concurrent appends
// land beyond it.
// ---------------------------------------------------------------------------
bool CycleRecorder::lane_window(uint32_t w_snap, CycleLane lane, uint8_t back,
                                uint32_t& begin, uint32_t& end, bool& stale) const noexcept {
    stale = false;
    // Back off from the head: those slots may be claimed but not yet stored.
    const uint32_t head = (w_snap > READ_GUARD) ? (w_snap - READ_GUARD) : 0u;
    if (head < 2) return false;
    // Everything before head - RING_MAX has been overwritten.
    const uint32_t oldest = (head > RING_MAX) ? (head - RING_MAX) : 0u;

    if (back > BACK_MAX) return false;
    const uint8_t want = static_cast<uint8_t>(back + 2);   // both ends of the chosen cycle
    uint32_t marks[BACK_MAX + 2];
    uint8_t  found = 0;
    for (uint32_t i = head; i-- > oldest; ) {
        const CycleEdge& e = at(i);
        if (((e.flags >> 1) & 7) == static_cast<uint8_t>(CycleSignal::Stall)) {
            // The engine stopped here. If a marker has already been found, the window being built
            // would reach back across the gap and join two different runs of the engine — refuse it.
            // If none has, everything from here back predates the stall, so the window that WILL be
            // found is the last cycle the engine turned: real, but history.
            if (found >= 1) return false;
            continue;
        }
        if ((e.flags & CYCLE_FLAG_START) && cycle_lane_of(e.flags) == lane) {
            marks[found++] = i;
            if (found == want) break;
        }
    }
    if (found < want) return false;
    begin = marks[back + 1];
    end   = marks[back];
    // Stale when the engine stopped at or after this window closed — read from stall_w_ rather than
    // the ring, so a stall sitting in the reader's guard band is still seen.
    stale = (stall_w_ >= end);
    return end > begin;
}

bool CycleRecorder::selected(uint32_t abs) const noexcept {
    const uint8_t flags = at(abs).flags;
    // A stall is a discontinuity, not an edge — it is never shipped.
    if (((flags >> 1) & 7) == static_cast<uint8_t>(CycleSignal::Stall)) return false;
    const uint8_t l = static_cast<uint8_t>(cycle_lane_of(flags));
    return abs >= sel_begin_[l] && abs < sel_end_[l];
}

// ---------------------------------------------------------------------------
// state — DERIVED, not stored, so there is nothing to keep in sync with reality.
// ---------------------------------------------------------------------------
CycleRecorder::State CycleRecorder::state() const noexcept {
    if (!enabled_) return State::Idle;
    const uint32_t w = w_.load(std::memory_order_relaxed);
    uint32_t b = 0, e = 0; bool st = false;
    // The TRIGGER lane decides: it is the engine's own pattern. A grid exists only once that pattern
    // has been decoded, so reporting Complete on the grid alone would promise a cycle the engine has
    // not actually turned.
    if (lane_window(w, CycleLane::Trigger, 0, b, e, st))
        return st ? State::Stale : State::Complete;
    return (w > 0) ? State::Recording : State::Armed;
}

// ---------------------------------------------------------------------------
// serialize — one page of the selected cycle, in the order the edges actually happened.
//
// The selection is LATCHED on the first page (first == 0) and reused for the rest, because a host
// pages a cycle out over several round trips and the engine keeps turning underneath. Re-selecting
// per page would splice the front of one cycle onto the back of another and present it as one
// picture. If the writers lap the latched capture before the host finishes reading it, that is
// REPORTED (dropped = 1) rather than served as a torn frame.
// ---------------------------------------------------------------------------
uint16_t CycleRecorder::serialize(uint8_t* out, uint32_t cap, uint16_t first,
                                 uint8_t back) const noexcept {
    if (!out || cap < sizeof(CycleHeader)) return 0;

    const uint32_t w_now = w_.load(std::memory_order_relaxed);

    // A new read, or the host moved to a different cycle mid-paging. Re-selecting on a changed
    // `back` matters: the latch exists so pages of ONE cycle stay consistent, not to pin the host to
    // the cycle it happened to ask for first.
    if (first == 0 || back != sel_back_) {
        uint32_t tb = 0, te = 0, gb = 0, ge = 0;
        bool ts = false, gs = false;
        const bool trig = lane_window(w_now, CycleLane::Trigger, back, tb, te, ts);
        const bool grid = lane_window(w_now, CycleLane::Grid,    back, gb, ge, gs);
        sel_back_ = back;
        sel_stale_ = ts;
        // The trigger lane is required — it IS the cycle. The grid may legitimately be absent (no
        // sync yet, or a capture that predates the first grid tooth); its lane then contributes
        // nothing rather than inventing a window.
        sel_valid_ = trig;
        sel_begin_[0] = tb; sel_end_[0] = te;
        sel_begin_[1] = grid ? gb : 0; sel_end_[1] = grid ? ge : 0;
        dropped_ = 0;
    } else if (sel_valid_ && !live(sel_begin_[0], w_now)) {
        sel_valid_ = false;                 // lapped mid-read — say so, do not serve a torn page
        dropped_   = 1;
    }

    // Walk the union of the two windows in RING order, so the page reads as the engine ran rather
    // than as lanes concatenated. Each entry is admitted only by its own lane's window.
    uint32_t scan_lo = 0, scan_hi = 0;
    if (sel_valid_) {
        scan_lo = sel_begin_[0];
        scan_hi = sel_end_[0];
        if (sel_end_[1] > sel_begin_[1]) {          // a grid window exists
            if (sel_begin_[1] < scan_lo) scan_lo = sel_begin_[1];
            if (sel_end_[1]   > scan_hi) scan_hi = sel_end_[1];
        }
    }

    uint16_t total = 0;
    for (uint32_t i = scan_lo; i < scan_hi; ++i) if (selected(i)) ++total;

    uint16_t room = static_cast<uint16_t>((cap - sizeof(CycleHeader)) / sizeof(CycleEdge));
    if (room > PAGE_MAX) room = PAGE_MAX;
    uint16_t count = 0;
    if (first < total) {
        count = static_cast<uint16_t>(total - first);
        if (count > room) count = room;
    }

    CycleHeader h{};
    h.magic       = CYCLE_MAGIC;
    h.version     = CYCLE_VERSION;
    h.state       = static_cast<uint8_t>(state());
    h.cycle_angle = static_cast<uint16_t>(cycle_angle_);
    h.total       = total;
    h.first       = first;
    h.count       = count;
    h.rpm_x10     = rpm_x10_;
    h.sync_level  = sync_level_;
    h.dropped     = dropped_;
    // The SELECTED cycle's own number, not the live counter. The opening edge carries its cycle's low
    // 3 bits, so the full number is recovered by walking the live counter back by the wrapped
    // difference — exact, because the selected cycle is only ever one or two behind.
    h.cycle_seq = 0;
    if (sel_valid_) {
        const uint8_t d = static_cast<uint8_t>((trig_seq_ - cycle_seq3_of(at(sel_begin_[0]).flags))
                                               & CYCLE_SEQ_MASK);
        h.cycle_seq = static_cast<uint16_t>(trig_seq_ - d);
    }
    std::memcpy(out, &h, sizeof(h));

    uint8_t*  p    = out + sizeof(CycleHeader);
    uint16_t  seen = 0, put = 0;
    for (uint32_t i = scan_lo; i < scan_hi && put < count; ++i) {
        if (!selected(i)) continue;
        if (seen++ < first) continue;       // skip the pages already sent
        const CycleEdge& ed = at(i);
        std::memcpy(p, &ed, sizeof(CycleEdge));
        p += sizeof(CycleEdge);
        ++put;
    }
    count = put;
    std::memcpy(out + offsetof(CycleHeader, count), &count, sizeof(count));
    return static_cast<uint16_t>(sizeof(CycleHeader) + count * sizeof(CycleEdge));
}
