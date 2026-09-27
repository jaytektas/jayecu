#pragma once
#include "SchedulerTypes.h"
#include "../Comms/OmniProtocol.h"
#include <atomic>
#include <cstdint>
#include <cstddef>

// ---------------------------------------------------------------------------
// CycleRecorder — one engine cycle of DELIVERED edges, in engine degrees.
//
// This records what the scheduler ACTUALLY DID, at the instant it did it: every coil and injector
// edge is written from the same choke point that drives the pin (EventScheduler::drive_ign /
// drive_inj), every trigger tooth from the capture path that feeds the PLL, and every virtual tooth
// from the DCO grid. Nothing here recomputes a schedule, so there is no model of the engine to be
// wrong — if a dwell landed 3 degrees late because the alarm was late, the record says 3 degrees late.
//
// THE ISRs DECIDE NOTHING — and this is the second design, because the first got it wrong.
//
// Version 1 gated recording on a `state_` flag flipped at the cycle boundary by the GRID ISR
// (priority 2) and read by the CAPTURE ISR (priority 1), which preempts it. A crank tooth arriving
// inside that window is attributed to the outgoing cycle. Measured on a 60-2 at 3000 rpm over 312
// captures: 309 of 116 teeth, 2 of 117, 1 of 115 — and in every bad one the disputed edge was the
// tooth at angle ~0, which follows virtual tooth 0 by 5.6 us at 3000 rpm and under 2 us at 9000. A
// 60-2 produces exactly 116 teeth per cycle. 115 is not "close": it is a tooth in the wrong cycle.
//
// Masking cannot fix it, because a late grid ISR is indistinguishable from an early edge. So the
// ISRs no longer decide anything: they append (angle, channel, flags) and nothing else. WHICH CYCLE
// an edge belongs to is decided at READ time, in task context, from the recorded angles — a cycle
// boundary is where the angle wraps high to low, and each ring is append-ordered, so that boundary
// is unambiguous in the data. Selection cannot race because nothing preempts the reader.
//
// A ring therefore holds SEVERAL cycles, and the reader returns the most recent COMPLETE one: the
// span between the last two wraps. The newest partial cycle is deliberately withheld — it is still
// being written, and half a cycle drawn as a whole one is exactly the confident lie this design
// exists to prevent.
//
// COST. Recording is a bounds check and a store per edge while enabled, and a single flag test when
// not. There is no per-cycle work, no boundary handling on the hot path, and nothing to outrun the
// link: the host reads at its own pace and always gets a whole cycle.
//
// LOGICAL STATE, NOT PIN LEVEL. An edge records `active` — coil energized (dwelling), injector
// energized (open) — not the electrical level. The scheduler already knows the configured polarity,
// so an active-low igniter and an active-high one produce the identical record. Shipping the raw
// level would make every consumer re-derive polarity, and the one that forgot would draw the inverse
// of the truth.
//
// ONE RING, NOT THREE — and this is the third design, because the second got THIS wrong.
//
// Version 2 kept a ring per lane and found each lane's cycle from its own angle wraps. That works
// only where a lane is dense enough to make a boundary visible. It is not: an output lane holds two
// coil edges per cycle, so at a 33.0 deg dwell start and 35.5 deg end the step across the boundary is
// 2.5 deg, indistinguishable from motion WITHIN a cycle. Three rings have no shared ordering key, so
// "the same cycle" cannot be identified in a sparse lane at all. Separate rings were the bug.
//
// So all three sources append to ONE ring. Append order is real time order, and every edge is stamped
// with the angle at the instant it happened, so the merged sequence ascends through a cycle and drops
// by nearly the whole span at the boundary — regardless of how sparse any individual lane is. The
// reader finds the last two boundaries once, and every lane between them is by construction the same
// cycle. There is nothing left to keep in agreement.
//
// CYCLES ARE CUT BY IDENTITY, NOT BY ANGLE — and this is the last thing that had to change, because
// cutting by angle is exact for at most one stream at a time.
//
// Selecting the window by "where the angle drops by nearly a span" measured 99.73% of captures at the
// 116 teeth a 60-2 must produce, with 5 of 117 and 5 of 115 in 3753 — and the equal counts are the
// tell. Real tooth 0 and virtual tooth 0 both sit at 0 deg and arrive within microseconds, from two
// different interrupt priorities, so which lands first JITTERS. A tooth is stamped with the firing
// clock's belief (see EnginePositionHal), which near the boundary reads 719.9 deg as readily as 0.1,
// so that one tooth falls either side of a virtual-tooth cut. A window bounded by one stream's marker
// gains a tooth when the ordering flips at one end and loses one when it flips at the other. It is
// not noise to be tolerated: 115 is a tooth in the wrong cycle.
//
// No single cut point can be exact for two streams that both sit at 0 deg. So each lane is cut at its
// OWN once-per-cycle marker, flagged on the edge by the very ISR that records it — no cross-priority
// flag, so nothing to race:
//
//   trigger lane (crank + cam) — the edge at which the DECODER'S COUNTED angle wraps. Counted, not
//                                interpolated, so the wrap is exact and cannot jitter.
//   grid lane (virtual + outputs) — virtual tooth index 0. Outputs are scheduled against the grid,
//                                so they are cut with it.
//
// Every stream is then a whole number of its own periods, by construction, whatever the microsecond
// ordering between them happens to be. The two windows differ by a couple of ring entries and describe
// the same revolution. Nothing depends on the recorded angles any more — they are the data, not the
// index.
//
// The price is that two interrupt priorities now share one index: trigger edges arrive from the
// capture ISR (prio 1) and outputs/virtual teeth from TIM5 (prio 2, which prio 1 preempts). The slot
// is therefore CLAIMED with an atomic fetch-add — LDREX/STREX on this core, so a preempted claim
// retries rather than two writers taking the same slot and losing an edge. The claim is atomic; the
// 4-byte store that follows is not, so at most one claim per priority level can be outstanding at any
// instant. The reader skips the newest READ_GUARD entries for that reason, and never returns one.
// ---------------------------------------------------------------------------

enum class CycleSignal : uint8_t {
    Coil     = 0,
    Injector = 1,
    Crank    = 2,
    Cam      = 3,
    // The VIRTUAL grid: one mark per DCO tooth, carrying the angle the firing clock BELIEVED it was
    // at. Distinct from Crank on purpose — a crank mark is a real tooth arriving, a virtual mark is
    // the PLL's opinion between them, and the gap between the two IS the PLL error. Overlaying them
    // on one lane would hide exactly the thing worth seeing.
    Virtual  = 4,
    // A DISCONTINUITY, not an edge: the trigger was lost here. Recorded so the gap is a fact IN the
    // data rather than something the reader has to be told out of band. Two things depend on it:
    // a window may never SPAN one (the teeth either side belong to different runs of the engine, and
    // splicing them makes a frame that is exact-looking and false), and a window entirely BEFORE one
    // is the last cycle the engine actually turned — still worth showing, but never as though it
    // were live.
    Stall    = 5,
};

// flags bit 4: this edge OPENS its lane's cycle. Set by the recording ISR itself, from data only that
// ISR has — the decoder's counted wrap, or grid index 0 — so it is a fact about the edge rather than a
// state flag shared between two interrupt priorities.
inline constexpr uint8_t CYCLE_FLAG_START = 0x10;

// flags bits 5-7: the low 3 bits of the cycle this edge belongs to, stamped AT WRITE TIME by the
// lane's own ISR from that lane's own counter. A reader never has to infer which cycle an edge is in.
//
// It must be per-lane and incremented by the writing ISR, for the same reason the markers are. The
// old cycle_seq_ was incremented in on_cycle_boundary — the GRID ISR at priority 2 — while teeth are
// written by the capture ISR at priority 1 which preempts it. Stamping edges from that counter would
// have handed the boundary tooth whichever value won the race, which is the exact defect this design
// was rebuilt to remove.
//
// Three bits wrap every 8 cycles. That is ample: the reader only ever resolves the newest complete
// cycle, one or two behind the live counter, so the full sequence number is recovered exactly.
inline constexpr uint8_t CYCLE_SEQ_SHIFT = 5;
inline constexpr uint8_t CYCLE_SEQ_MASK  = 0x07;   // after shifting
inline uint8_t cycle_seq3_of(uint8_t flags) noexcept {
    return static_cast<uint8_t>((flags >> CYCLE_SEQ_SHIFT) & CYCLE_SEQ_MASK);
}

// Which window an edge is selected by. The two lanes are cut independently; see the note above.
enum class CycleLane : uint8_t {
    Trigger = 0,   // Crank, Cam       — cut where the decoder's COUNTED angle wraps
    Grid    = 1,   // Virtual, outputs — cut at virtual tooth 0 (outputs are scheduled off the grid)
};
inline CycleLane cycle_lane_of(uint8_t flags) noexcept {
    const uint8_t sig = static_cast<uint8_t>((flags >> 1) & 7);
    return (sig == static_cast<uint8_t>(CycleSignal::Crank) ||
            sig == static_cast<uint8_t>(CycleSignal::Cam))
           ? CycleLane::Trigger : CycleLane::Grid;
}

#pragma pack(push, 1)
// One recorded edge. 4 bytes, laid out identically on the MCU and the host — this
// struct IS the wire format, so there is no serializer to disagree with the reader.
struct CycleEdge {
    uint16_t angle;     // engine angle, decidegrees, 0 <= angle < cycle_angle
    uint8_t  channel;   // logical channel: coil/injector index, or trigger role slot
    uint8_t  flags;     // bit0 = active; bits 1-3 = CycleSignal; bit4 = opens its lane's cycle;
                        // bits 5-7 = that cycle's number, low 3 bits (see CYCLE_SEQ_SHIFT)
};
static_assert(sizeof(CycleEdge) == 4, "CycleEdge is the wire format");

// Page header prefixing every read reply.
struct CycleHeader {
    uint16_t magic;        // 'C' | 'Y' << 8
    uint8_t  version;      // 2
    uint8_t  state;        // CycleRecorder::State at the time of the read
    uint16_t cycle_angle;  // the span these angles live in: 3600 / 7200 / 10800
    uint16_t total;        // edges in the selected cycle (all lanes)
    uint16_t first;        // index of the first edge in THIS page
    uint16_t count;        // edges in this page
    uint32_t rpm_x10;      // rpm at the boundary that closed the selected cycle
    uint8_t  sync_level;   // SyncLevel: CRANK means angles are folded to 360
    uint8_t  dropped;      // 1 = the writers lapped this capture mid-read; the page may be torn
    uint16_t cycle_seq;    // engine cycles counted since boot — a LABEL, see the .cpp
};
static_assert(sizeof(CycleHeader) == 20, "CycleHeader is the wire format");
#pragma pack(pop)

inline constexpr uint16_t CYCLE_MAGIC   = 0x5943;   // 'C' | 'Y'<<8, little-endian on the wire
// 2 adds cycle_seq in what version 1 sent as a reserved zero. Bumped rather than quietly reused:
// a reader that trusts a zero it was told to ignore is exactly the bug that field would cause.
// 3 adds State::Stale and CycleSignal::Stall. A version-2 reader would render state 4 as an unknown
// number and call a dead engine's last frame live, so the version moves with it.
inline constexpr uint8_t  CYCLE_VERSION = 3;

class CycleRecorder {
public:
    // Reported to the host so an empty reply is explicable rather than mysterious.
    enum class State : uint8_t {
        Idle      = 0,   // recording disabled
        Armed     = 1,   // enabled; nothing recorded yet
        Recording = 2,   // enabled and filling, not yet a whole cycle
        Complete  = 3,   // a complete cycle is available, and the engine is still turning
        // A complete cycle is available but the TRIGGER HAS BEEN LOST since it was captured. The
        // frame is real and worth looking at — it is the last cycle the engine turned — but it is
        // history. Without this the reader served that frame forever with its own rpm in the header,
        // so a stalled engine looked like a running one: measured on the bench as state=Complete and
        // rpm 3024.8 held indefinitely while the engine read 0 and sync 0.
        Stale     = 4,
    };

    // Ring capacity, in edges across ALL lanes. It must hold more than two cycles, because the reader
    // returns the last COMPLETE one and needs the boundary on both sides of it, plus margin so the
    // writers do not overtake a capture while the host is still paging it out.
    //
    // Sized for the DENSEST wheel this firmware supports. A Nissan optical CAS is a 360-slit fine
    // track plus a slot per cylinder on a disc turning at CAM speed — one disc revolution is one
    // engine cycle — so both edges of everything is 732 trigger records in a SINGLE cycle. Add
    // MAX_VTEETH grid marks and 68 output edges (12 coils + 22 injectors, two edges each) and the
    // worst case is ~908 per cycle. 4096 holds four and a half of those, or thirty-five 60-2 cycles.
    //
    // A POWER OF TWO deliberately: the slot index is (w % RING_MAX) on the append path, which runs in
    // the priority-1 capture ISR. At 3072 the compiler emitted a udiv + mls there; at 4096 it is a
    // single AND. Changing this to a non-power-of-two silently puts a divide back in that ISR.
    static constexpr uint16_t RING_MAX = 4096;
    static_assert((RING_MAX & (RING_MAX - 1)) == 0, "RING_MAX must be a power of two — see above");
    // The reader ignores the newest few entries: a slot is claimed atomically but stored a few
    // instructions later, so an entry right at the head can be claimed-but-not-yet-written. Only one
    // claim per interrupt priority can be outstanding, so two would do; eight costs nothing.
    static constexpr uint16_t READ_GUARD = 8;
    // Edges per read page: what fits one Omni frame after the header. The host adds `count` to
    // `first` until a short page comes back — the same loop the DTC table read uses.
    //
    // Sized to the frame, not to a round number. A page is sizeof(CycleHeader) + PAGE_MAX*4 bytes and
    // must fit OMNI_MAX_PAYLOAD, so this is the largest value the wire allows. It matters because
    // every extra page is another round trip, and round trips are what decide whether a cycle is
    // captured at all: at 20833 rpm the engine finishes one every 5.8 ms while a read takes ~3 ms.
    //
    // At 240 a fully sequential twelve-cylinder frame measured 238 edges — two from spilling into a
    // second page purely by luck of the cylinder count.
    static constexpr uint16_t PAGE_MAX =
        static_cast<uint16_t>((Comms::OMNI_MAX_PAYLOAD - sizeof(CycleHeader)) / sizeof(CycleEdge));
    // How far BACK the host may reach: 0 is the newest complete cycle, 1 the one before it. The ring
    // already holds several cycles, and above about 12000 rpm the engine finishes them faster than a
    // round trip can fetch one — measured 61% coverage at 20833 rpm, with 386 cycles skipped while
    // the buffer held roughly twenty at any instant. Reading backwards lets a host collect a
    // CONTIGUOUS run per pass instead of only the newest, so the cycles it missed are the ones
    // already sitting in the ring rather than lost.
    //
    // Bounded so the marker walk and its scratch array stay bounded, and kept well inside what the
    // densest wheel can hold (a Nissan optical fills the ring in about 4.5 cycles).
    static constexpr uint8_t BACK_MAX = 3;

    // ---- Host / task side ---------------------------------------------------
    // Enable recording. Idempotent, and does NOT discard history: a reader that enables and reads
    // immediately gets the last complete cycle rather than waiting for a fresh one.
    void enable() noexcept;
    // Disable and discard everything.
    void reset() noexcept;
    [[nodiscard]] State   state()   const noexcept;
    [[nodiscard]] uint8_t dropped() const noexcept { return dropped_; }

    // Serialize the last COMPLETE cycle: header + up to PAGE_MAX edges from `first`. Returns bytes
    // written, or 0 if `out` cannot hold the header. TASK CONTEXT ONLY.
    // `back` selects which complete cycle: 0 = newest, 1 = the one before it, up to BACK_MAX. A
    // `back` the ring cannot satisfy yields an empty page rather than a nearer cycle silently
    // substituted — a host asking for cycle N-3 and being handed N-1 would draw a gap as continuity.
    [[nodiscard]] uint16_t serialize(uint8_t* out, uint32_t cap, uint16_t first,
                                     uint8_t back = 0) const noexcept;

    // ---- ISR side -----------------------------------------------------------
    // The only question an ISR asks. No cycle state, no boundary, no decision.
    [[nodiscard]] bool recording() const noexcept { return enabled_; }

    // The trigger was lost (stall / sync loss). Records the discontinuity so no later read can
    // splice across it. ISR context.
    void on_trigger_lost() noexcept;

    // Labels a capture with the engine's rpm / sync / span, and counts cycles. It does NOT gate
    // recording and does NOT decide membership — that is the entire point of this design.
    void on_cycle_boundary(uint16_t index, AngleDeg10 cycle_angle,
                           uint32_t rpm_x10, uint8_t sync_level) noexcept;

    // An output edge, from the timer ISR (prio 2). `active` is the LOGICAL state. Outputs carry no
    // marker of their own — they are scheduled against the grid and are cut with it.
    void record_output(CycleSignal sig, uint8_t channel, AngleDeg10 angle, bool active) noexcept;
    // A trigger edge, from the capture ISR (prio 1). `cycle_start` marks the edge at which the
    // decoder's COUNTED angle wrapped — the trigger lane's once-per-cycle identity.
    void record_trigger(CycleSignal sig, uint8_t stream, AngleDeg10 angle, bool rising,
                        bool cycle_start) noexcept;
    // One virtual grid tooth, from the DCO match (shares the timer ISR's vector). `cycle_start` is
    // grid index 0.
    void record_virtual(AngleDeg10 angle, bool cycle_start) noexcept;

private:
    static uint8_t pack_flags(CycleSignal sig, bool active, bool cycle_start, uint16_t seq) noexcept {
        return static_cast<uint8_t>((active ? 1u : 0u) | (static_cast<uint8_t>(sig) << 1) |
                                    (cycle_start ? CYCLE_FLAG_START : 0u) |
                                    ((seq & CYCLE_SEQ_MASK) << CYCLE_SEQ_SHIFT));
    }

    // Claim the next slot atomically, then write it. w_ counts TOTAL appends — never wrapped — so the
    // reader knows both how much history is live (the newest RING_MAX) and where a capture sits in it.
    void append(const CycleEdge& e) noexcept;

    // The last two cycle-start markers of ONE lane, as absolute append indices [begin, end). False
    // until the ring holds two of them.
    // `stale` is set when the window lies entirely before a recorded stall — a real cycle, but one
    // the engine turned before it stopped.
    bool lane_window(uint32_t w_snap, CycleLane lane, uint8_t back, uint32_t& begin, uint32_t& end,
                     bool& stale) const noexcept;
    // True when the edge at absolute index `abs` belongs to the selected capture — i.e. it lies in
    // ITS OWN lane's window.
    [[nodiscard]] bool selected(uint32_t abs) const noexcept;
    // True while absolute index `abs` is still live — i.e. the writers have not lapped it.
    [[nodiscard]] bool live(uint32_t abs, uint32_t w_now) const noexcept {
        return w_now - abs <= RING_MAX;
    }
    [[nodiscard]] const CycleEdge& at(uint32_t abs) const noexcept { return ring_[abs % RING_MAX]; }

    volatile bool     enabled_     = false;
    mutable  uint8_t  dropped_     = 0;
    AngleDeg10        cycle_angle_ = ANGLE_720;
    uint32_t          rpm_x10_     = 0;
    uint8_t           sync_level_  = 0;
    // One counter per lane, each incremented ONLY by the ISR that owns that lane — trig_seq_ by the
    // capture ISR on the decoder's counted wrap, grid_seq_ by the timer ISR on virtual tooth 0.
    // Neither is read by the other, so neither can race. The header reports the TRIGGER lane's,
    // because the trigger pattern is what an engine cycle actually is.
    uint16_t          trig_seq_ = 0;
    uint16_t          grid_seq_ = 0;

    CycleEdge             ring_[RING_MAX];
    std::atomic<uint32_t> w_{ 0 };

    // The capture currently being paged out. A host reads a cycle over several round trips, so the
    // selection is LATCHED on the first page: re-selecting per page would let the engine advance
    // mid-read and splice two cycles into one picture. Cleared and re-taken whenever a read starts at
    // index 0. Mutable because serialize() is const to its caller — it reads the engine, it only
    // remembers which cycle it is reading.
    mutable uint32_t sel_begin_[2] = { 0, 0 };   // indexed by CycleLane
    mutable uint32_t sel_end_  [2] = { 0, 0 };
    mutable bool     sel_valid_    = false;
    mutable uint8_t  sel_back_     = 0;      // which cycle the latched selection is of
    mutable bool     sel_stale_    = false;

    // Append index just past the last recorded stall. The stall is ALSO an entry in the ring, which
    // is what stops a window spanning it, but the ring alone cannot answer "is the newest window
    // stale": the reader backs off READ_GUARD entries from the head, and when the engine stops the
    // stall IS the head with nothing following it, so it would sit inside that blind spot for ever.
    volatile uint32_t stall_w_ = 0;
};
