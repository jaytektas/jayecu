#pragma once

#include "../EngineModule.h"
#include "well_known_signals.h"   // wk:: rename-safe signal roles
#include "../PiController.h"
#include "../../Integration/OutputGate.h"   // the scramble button's hold/maximum/rest, already written
#include "../../../generated/modules/boost_config.h"

// ---------------------------------------------------------------------------
// Boost — closed-loop wastegate boost control.
//
// Reads rpm + map (manifold pressure) off the bus and PUBLISHES an abstract wastegate-solenoid demand
// (wk::wastegate_duty, 0..100%). It never names a pin — a Generic output row (cand=wastegate_duty)
// realises the demand on its own board pin, so the controller stays portable. This is the
// Alternator / Idle pattern.
//
//   Open loop  (mode 0): duty = base_duty_table(rpm[,2nd axis])
//   Closed loop(mode 1): duty = base_duty_table(...)  +  PID( boost_target - map ), once it has TAKEN OVER
//
// THE HANDOVER is separate from activation and the two are often confused. Activation asks whether the
// module acts at all; the handover asks when the closed loop takes over from the feed-forward. Below the
// control point (map < target - control_point_kpa), or before the start delay has run, the PID is held
// frozen and the output rests on the base duty — or on full duty with spool_assist, which is the fastest
// way to target and the least forgiving. Without this the loop engaged at activation_kpa and ran on the
// whole error through the entire spool, so the integrator sat pinned at its anti-windup clamp and was
// still pinned when boost arrived: maximum duty at the exact moment it should have been backing off.
//
// The integrator is FROZEN below the control point, not reset. A reset would wipe the trim every time
// the driver lifted or changed gear; freezing keeps what the loop has learned and still lets nothing
// accumulate while the feed-forward is doing the work.
//
//   boost_target = boost_target_table(rpm[,2nd axis])   (kPa, published as wk::boost_target)
//        + scramble bump (scramble_kpa) while the scramble input is asserted
//        + protection pull-back: target *= (1 + prot_boost_corr_pct/100)   (-100 -> kill boost)
//
// Gating: engine rpm >= activation_rpm AND map >= activation_kpa (don't fight the wastegate spring at
// low load — below activation the wastegate stays at its spring pressure, so we publish nothing / 0).
//
// SAFETY:
//  - frame.prot_boost_corr_pct (from EngineProtection) scales the target down (and at -100 zeroes it),
//    pulling the closed-loop demand back. The feed-forward base duty is scaled by the same factor so an
//    open-loop tune is protected too.
//  - Overboost backstop: if map exceeds overboost_limit_kpa, request a fuel/ign cut (configurable which)
//    as a hard last resort — independent of, and in addition to, the EngineProtection P0234 DTC path.
//
// When disabled or below activation it publishes nothing for wastegate_duty so the output failsafes to
// its default (wastegate closed / spring pressure), and clears the published telemetry target.
// ---------------------------------------------------------------------------

class DtcManager;

class Boost : public EngineModule {
public:
    void init(const BoostConfig& cfg);
    void on_config_change(const BoostConfig& cfg);
    void set_dtc(DtcManager* d) noexcept { dtc_ = d; }

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

private:
    void overboost(SignalBus& bus, float map, float target, uint32_t now);   // every path — see Boost.cpp
    bool ob_cutting_ = false;          // overboost backstop is cutting (hysteresis state)
    const BoostConfig* cfg_ = nullptr;
    DtcManager*        dtc_ = nullptr;
    bool was_enabled_ = false;   // enabled->disabled edge: heal our DTCs once (DtcManager::heal)
    PiController pi_;                 // closed-loop trim; integ is the persistent wastegate state
    uint32_t     last_ms_  = 0;       // for the PI/D dt
    float        last_err_ = 0.0f;    // for the derivative term
    // WHEN ACTIVATION BEGAN — the start delay is measured from here, so it has to survive the frames
    // between. 0 = not active. Cleared the moment the activation gate drops, which is what makes the
    // delay a per-spool wait rather than a once-per-boot one.
    uint32_t     active_since_ms_ = 0;
    // THE SCRAMBLE BUTTON IS A GATE, and OutputGate is the gate. Hold / maximum / rest are exactly its
    // min_on / max_on / rearm, down to the rule that the lockout only follows a maximum-time trip — so
    // this is the same three timers an output slot already has rather than a second set that drifts.
    OutputGate   scramble_;
    // THE LEARNED TRIM lives in the battery-backed learned region, not in the config: it is something
    // the engine found out, not something anybody tuned, and it must survive a reflash of the tune.
    // int16 at 0.1 %, one cell per base-duty cell — see the schema note on why it borrows those axes.
    int16_t*     ltt_ = nullptr;
    int          ltt_dwell_cell_ = -1;
    uint32_t     ltt_dwell_since_ms_ = 0;
    // THE INNER POSITION LOOP, for a motorised gate. Its own controller and its own clock: it is a
    // servo problem underneath a pressure problem, and the two run at the same cadence but measure
    // different things. `gate_cmd_` is the slew-limited command, which has to persist to be a ramp.
    PiController gate_pi_;
    float        gate_cmd_     = -1.0f;   // < 0 = no command yet, so the first one starts where it is
    float        gate_last_err_ = 0.0f;
    // ITS OWN CLOCK, not the outer loop's. last_ms_ only advances while the pressure loop is running,
    // so an open-loop tune driving a motorised gate would have left the servo with dt == 0 for ever
    // and a valve that never moved.
    uint32_t     gate_last_ms_ = 0;

    void reset_state();
    // The inner position loop for a motorised gate: turns the outer loop's answer into a valve
    // position and drives the motor to it. Solenoid mode never calls it.
    void drive_gate(SignalBus& bus, uint32_t now, float demand);
};
