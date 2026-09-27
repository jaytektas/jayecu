#pragma once
#include "ITimerChannel.h"
#include "SchedulerTypes.h"

// ---------------------------------------------------------------------------
// VirtualTrigger — a software "eTPU angle clock".
//
// The decoder hands us irregular, arbitrary-rate real-tooth data points (one
// tooth, 360 slots, any missing-tooth pattern — it doesn't matter). We resample
// that onto a FIXED uniform angular grid (default 36 teeth/rev, 72 per 720°
// cycle = one virtual tooth every 10°), phase-locked to the real crank by a
// software PLL/DCO. The scheduler downstream only ever sees this clean uniform
// grid and never learns what the real wheel looked like.
//
//   real wheel (any) → DECODER → [VirtualTrigger: resample + phase-lock] → scheduler
//
//   denser than the grid  (e.g. 360 slots) → DECIMATE: refine velocity on every
//                                            real tooth, emit a virtual tooth
//                                            only every 10° (no ISR storm).
//   sparser than the grid (e.g. 1 tooth)   → INTERPOLATE: free-run the DCO on
//                                            the velocity estimate between teeth.
//
// THE CONTRACT (the whole point of the decoupling): every virtual tooth hands
// the scheduler a VirtualToothData. Get this struct right and decoder/scheduler
// evolve independently forever.
//
// Platform-agnostic: the only hardware it touches is one ITimerChannel used as
// the DCO (a "call me back at tick T" timer) plus the shared timebase. No MCU
// headers here.
//
// Rules baked in (from the design):
//   * SLEW, never slam — phase corrections are bounded per update; a correction
//     beyond physical limits means a noisy tooth and is clamped.
//   * LIVE prediction — ticks_per_vtooth is the single source of truth; the DCO
//     period AND every consumer read the freshest value at the moment of use.
//   * NO skip — the DCO is never re-timed into the past; if a correction would,
//     it fires immediately instead.
// ---------------------------------------------------------------------------

// The decoder→scheduler data point. One per virtual tooth.
struct VirtualToothData {
    AngleDeg10 angle;            // absolute crank angle of this virtual tooth (0..cycle_angle)
    uint16_t   index;            // virtual tooth number within the cycle (0..teeth_per_cycle-1)
    uint32_t   tick;             // timebase tick at which this virtual tooth fired
    uint32_t   ticks_per_vtooth; // LIVE angular velocity: ticks per one virtual-tooth angle
    bool       cycle_start;      // true on index 0 (start of the 720° cycle)
};

using VirtualToothCallback = void (*)(const VirtualToothData& vt, void* user_data);

class VirtualTrigger {
public:
    // dco: an ITimerChannel used purely as a "fire a callback at an absolute
    //   tick" source (no GPIO). cycle_angle: 3600 (CRANK/360°) or 7200 (720°).
    //   teeth_per_cycle: how many virtual teeth span the cycle (e.g. 72 → 10°).
    VirtualTrigger(IAlarmTimer& dco,
                   AngleDeg10     cycle_angle,
                   uint16_t       teeth_per_cycle) noexcept;

    void register_callback(VirtualToothCallback cb, void* user_data) noexcept;

    // Re-point the grid at a new cycle span / resolution (e.g. widen 3600/36 at
    // CRANK to 7200/72 at PHASE so the grid emits a full-cycle index and the
    // scheduler can tell the two engine revolutions apart). Resets the lock; the
    // grid re-anchors to the decoder's (now full-cycle) angle on the next tooth.
    void reconfigure(AngleDeg10 cycle_angle, uint16_t teeth_per_cycle) noexcept;

    // ---- Fed by the decoder, per processed real tooth (ISR context) ----------
    // angle:            absolute crank angle now (decidegrees)
    // tick:             timebase tick captured at this real tooth
    // real_tooth_ticks: period of the last real tooth (ticks) — the speed sample
    // real_tooth_angle: angular span of one real tooth (decidegrees)
    // last_was_gap: the decoder just snapped its angle across a missing-tooth gap
    //   (a position discontinuity) — re-anchor the grid to it rather than slew.
    void on_real_tooth(AngleDeg10 angle, uint32_t tick,
                       uint32_t real_tooth_ticks, AngleDeg10 real_tooth_angle,
                       bool last_was_gap) noexcept;

    // The engine lost sync — stop the virtual clock until re-acquired.
    void reset() noexcept;

    // ---- Lost-trigger detection: NOT HERE ------------------------------------
    // It used to be. on_dco_match() checked the time since the last real tooth and forced a sync
    // loss past a timeout — which worked only while the DCO was locked and emitting. Every sync
    // transition resets this clock (EnginePositionHal::handle_sync_level_change -> reconfigure()),
    // so between a CRANK<->PHASE change and the next real tooth there was no detection at all, and
    // an ECU could hold PHASE sync and a plausible RPM indefinitely with no wheel connected.
    //
    // "Is the engine turning" is the DECODER's question, not the firing clock's, and it is now
    // answered by the decoder's own deadline (EnginePositionHal::on_tooth_deadline) off a timer
    // nothing else resets. Deliberately not reintroduced here: two implementations of one decision,
    // one of them disarmable by another layer, is how the original fault survived.

    // ---- DCO match (ISR context) — call from the dco timer's match callback --
    void on_dco_match() noexcept;

    // ---- Telemetry (task context) -------------------------------------------
    [[nodiscard]] bool     is_locked()         const noexcept { return locked_; }
    [[nodiscard]] uint32_t ticks_per_vtooth()  const noexcept { return ticks_per_vtooth_; }
    [[nodiscard]] AngleDeg10 vtooth_angle()     const noexcept { return vt_angle_step_; }
    // Phase error of the last real-tooth correction (ticks, pre-slew) — a lock
    // quality meter: small + centred on 0 ⇒ tracking well.

private:
    void   arm_dco(uint32_t tick) noexcept;
    AngleDeg10 wrap(int32_t a) const noexcept;

    IAlarmTimer&         dco_;
    VirtualToothCallback cb_;
    void*                cb_data_;

    AngleDeg10       cycle_angle_;       // 3600 (CRANK/360°) or 7200 (PHASE/720°)
    uint16_t         teeth_per_cycle_;   // 36 (CRANK) or 72 (PHASE)
    AngleDeg10       vt_angle_step_;     // cycle_angle / teeth_per_cycle (e.g. 100 = 10°)

    volatile uint32_t ticks_per_vtooth_; // LIVE velocity — single source of truth
    AngleDeg10        next_vt_angle_;     // angle of the next virtual tooth to fire
    uint32_t          next_vt_tick_;      // its currently-scheduled tick
    volatile bool     locked_;
    volatile uint16_t vt_emitted_;        // count of virtual teeth fired (telemetry)
    volatile int32_t  last_err_;          // last pre-slew phase error (telemetry)
    volatile uint32_t last_inst_ = 0;     // ticks per vtooth over the LAST real tooth (grid cadence)

};
