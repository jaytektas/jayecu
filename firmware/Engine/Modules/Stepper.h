#pragma once

class DtcManager;

#include "../EngineModule.h"
#include "../../../generated/modules/stepper_config.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// Stepper — bipolar stepper position controller. Maps a 0..100% position demand (any bus signal,
// e.g. idle_duty) to a target and drives the motor there, rate-limited to step_period_ms per full
// step so slow valves (a GM 4-wire IAC runs ~100-200 steps/s) don't miss steps. Two driver modes:
//
//   H-Bridge (driver_mode 0): microsteps the two coils (cos/sin) and publishes signed coil duties
//     SIG_STEP_DEMAND_A/B (+ SIG_STEP_EN_A/B). The two HBridge bridge[] slots consume them — wire
//     bridge[0].demand_sig = step_demand_a, enable_sig = step_en_a (and [1] for B). Uses both bridges.
//   Step/Direction (driver_mode 1): emits SIG_STEP_PULSE (one pulse per step) + held SIG_STEP_DIR /
//     SIG_STEP_ENABLE levels for an external driver IC (A4988/DRV8825). Wire each to an Outputs GPIO
//     slot. Uses no H-bridge, so it coexists with a dual-DBW ETB (which claims both bridges).
// ---------------------------------------------------------------------------

class Stepper : public EngineModule {
public:
    void init(const StepperConfig& cfg) noexcept;
    void on_config_change(const StepperConfig& cfg) noexcept;

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;
    void on_engine_stop() override;

    int32_t step_position() const noexcept { return pos_; }
    bool    homing() const noexcept { return homing_; }

    void set_dtc(DtcManager* d) noexcept { dtc_ = d; }

private:
    const StepperConfig* cfg_          = nullptr;
    int32_t              pos_          = 0;
    uint32_t             last_step_ms_ = 0;     // step-rate limit (step_period_ms) timebase
    bool                 was_enabled_  = false;
    bool                 homing_       = false;     // driving to the closed stop before trusting pos_
    bool                 pulse_high_   = false;     // Step/Dir: last service raised STEP (lower it next)
    void start_homing() noexcept;
    DtcManager*          dtc_          = nullptr;
};
