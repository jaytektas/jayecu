#pragma once

#include "SchedulerTypes.h"

#include <cstdint>

// ---------------------------------------------------------------------------
// SegmentTimer — how long the crank took to turn through each cylinder's power stroke.
//
// THE MISFIRE SIGNAL, and it is a mechanical one rather than a chemical one: a cylinder that fires
// accelerates the crank through its expansion stroke, one that misfires does not. Measure the time to
// cross a fixed angular SEGMENT and a misfire shows up as a segment that took too long. This is the
// OBD-II method, and it needs no extra sensor — the trigger wheel is already the instrument.
//
// WHY THE REAL TEETH AND NOT THE GRID. The obvious tap looks like VirtualTrigger's phase error, but
// the PLL exists to REJECT this signal: it slews toward each real tooth and folds the rest into its
// velocity estimate, so the residual is only what the filter failed to absorb and its gain depends on
// PLL tuning. Worse, the virtual grid FREE-RUNS on the DCO between real teeth — those timestamps are
// synthesised and contain no measurement at all. So this taps the real-tooth stream, upstream of the
// filter designed to hide what it is looking for.
//
// AVAILABILITY IS A PROPERTY OF THE WHEEL. Resolution is bounded by teeth per segment: a 36-1 gives
// ~9 teeth across a 4-cylinder segment, which is ample; a 4-tooth wheel on a 12-cylinder gives less
// than one, which is nothing. Below the floor this reports UNAVAILABLE and produces no segments at
// all, rather than emitting confident noise for a classifier to believe.
//
// ISR DISCIPLINE. on_tooth() runs in the capture ISR and does no deciding: it detects a boundary
// crossing, closes the elapsed segment, and pushes (cylinder, ticks) into a lock-free ring. Whoever
// consumes it decides what a long segment MEANS — the same split the knock window already uses.
// ---------------------------------------------------------------------------

class SegmentTimer {
public:
    static constexpr uint8_t MAX_CYL = 12;
    // Teeth a segment must contain before its timing means anything. Two boundaries plus something
    // in between is the bare minimum for the span to be measured rather than guessed; below this the
    // answer is dominated by where the teeth happen to fall relative to TDC.
    static constexpr float MIN_TEETH_PER_SEGMENT = 3.0f;

    struct Segment {
        uint8_t  cyl;      // the cylinder whose expansion stroke this segment covered
        uint32_t ticks;    // timebase ticks taken to cross it
    };

    // Declare the ENGINE: per-cylinder TDC angles in the same engine frame vtrig_feed delivers, and
    // the cycle span. The WHEEL is not declared here — tooth pitch is not known until teeth arrive
    // (GenericTrigger measures it), and configuring from a zero pitch at start() would pin the
    // feature off forever. on_tooth() carries the live pitch and availability re-decides itself
    // whenever it changes, which also makes a wheel swap self-correcting.
    void configure(const AngleDeg10* tdcs, uint8_t ncyl, AngleDeg10 cycle) noexcept;

    // Reset the walk — sync loss, a stall, a reconfigure. Any segment in flight is abandoned rather
    // than closed against a tick from before the discontinuity.
    void reset() noexcept;

    // One real tooth (CAPTURE ISR). `angle` is engine angle, `tick` the timebase stamp, `pitch` the
    // measured angular span of one tooth — which is what decides availability.
    void on_tooth(AngleDeg10 angle, uint32_t tick, AngleDeg10 pitch) noexcept;

    // Drain one completed segment (TASK). False when empty.
    [[nodiscard]] bool pop(Segment& out) noexcept;

    [[nodiscard]] bool     available()      const noexcept { return available_; }
    [[nodiscard]] float    teeth_per_seg()  const noexcept { return teeth_per_seg_; }
    [[nodiscard]] uint32_t segments()       const noexcept { return segments_; }   // closed since reset
    [[nodiscard]] uint32_t dropped()        const noexcept { return dropped_; }    // ring overflows
    // Segments per cycle == cylinders, taken from the scheduler at configure(). The authoritative
    // count for anyone consuming segments, so a consumer never has to source it separately and drift.
    [[nodiscard]] uint8_t  segment_count()  const noexcept { return nbound_; }

private:
    // Which cylinder's segment OPENS at each boundary, in ascending angle order. Built by configure()
    // so the ISR does no searching: it walks a sorted list and only ever looks at the next boundary.
    AngleDeg10 bound_angle_[MAX_CYL] = {};
    uint8_t    bound_cyl_[MAX_CYL]   = {};
    uint8_t    nbound_    = 0;
    AngleDeg10 cycle_     = 7200;

    bool       available_     = false;
    float      teeth_per_seg_ = 0.0f;
    AngleDeg10 pitch_seen_    = 0;      // last pitch availability was decided from

    // Walk state. `open_idx_` is the boundary the current segment started at; UNSET before the first
    // crossing, because a segment can only be timed from a boundary we actually saw.
    static constexpr uint8_t UNSET = 0xFF;
    uint8_t  open_idx_    = UNSET;
    uint32_t open_tick_   = 0;
    AngleDeg10 last_angle_ = -1;

    volatile uint32_t segments_ = 0;
    volatile uint32_t dropped_  = 0;

    static constexpr uint8_t RING = 16;
    Segment           ring_[RING] = {};
    volatile uint8_t  head_ = 0;   // producer (ISR)
    volatile uint8_t  tail_ = 0;   // consumer (task)
};
