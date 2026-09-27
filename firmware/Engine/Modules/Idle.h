#pragma once

#include "../EngineModule.h"
#include "well_known_signals.h"   // wk:: rename-safe signal roles
#include "../PiController.h"
#include "../../../generated/modules/idle_config.h"

// ---------------------------------------------------------------------------
// Idle — Stage 1b: hardware-agnostic open-loop base + closed-loop PI(+D) idle.
//
// Reads rpm/clt/tps off the bus and PUBLISHES an abstract air demand (wk::idle_duty, 0..100%). It never
// names a pin — the generic outputs / HBridge pipeline realises the demand on a board pin, so the
// controller stays portable and the same demand can later bind to an ETB throttle target.
//
//   duty = clamp( base_duty(clt) + start_base_offset(run_time) + PI(+D) trim,
//                 min_output(rpm), max_duty )
//
// Closed loop (mode = Closed): target = target_rpm(clt) + start_target_offset(run_time); the error
// (target - rpm, published as wk::idle_rpm_error) drives a gain-scheduled PI(+D) trim whose P/I/D gains
// are read off their tables vs that error each step. Engaged only while idle_active, in Closed mode, and
// once RPM has fallen to within the activation offset of target. The integrator's authority is bound to
// the headroom between the min-output floor and max-duty, so anti-windup matches the final clamp.
// ---------------------------------------------------------------------------

class Idle : public EngineModule {
public:
    void init(const IdleConfig& cfg);
    void on_config_change(const IdleConfig& cfg);

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

private:
    const IdleConfig* cfg_ = nullptr;
    PiController pi_;                 // closed-loop trim; integ is the persistent state
    uint32_t     last_ms_  = 0;       // for the PI/D dt
    float        last_err_ = 0.0f;    // for the derivative term

    // Per idle-up slot: ramp fraction (0..1) + when its input first went active (0 = inactive).
    // The offset engages to 1 after on_delay_ms held and decays to 0 over decay_ms on release.
    struct IdleUpState { float frac = 0.0f; uint32_t active_since_ms = 0; };
    IdleUpState idle_up_[IDLE_IDLE_UP_COUNT];

    float follower_applied_ = 0.0f;  // throttle-follower dashpot: rises with TPS, decays on tip-out

    // Long-term trim: learned per-CLT base correction (%duty) in battery-backed NV. Steady PI trim
    // migrates here over dwell; base+LTT self-corrects and the PI recenters (idle analogue of LTFT).
    float*   ltt_              = nullptr;   // [IDLE_LTT_CLT_AXIS_ALLOC] learned %duty, in NV
    uint8_t  ltt_dwell_cell_   = 0xFF;
    uint32_t ltt_dwell_since_ms_ = 0;
    int  ltt_cell_index(float clt) const;   // nearest ltt_clt_axis breakpoint

    // Controller dynamics state
    float slewed_target_  = 0.0f;     // rate-limited effective target
    bool  slew_valid_     = false;    // false -> snap slewed_target_ to the raw target next frame
    float decel_applied_  = 0.0f;     // decaying tip-out air offset (%)
    bool  throttle_closed_prev_ = true;

    void reset_state();
};
