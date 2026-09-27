#pragma once

#include "../EngineModule.h"
#include "well_known_signals.h"   // wk:: rename-safe signal roles
#include "../PiController.h"
#include "../../../generated/modules/alternator_config.h"

// ---------------------------------------------------------------------------
// Alternator — closed-loop alternator charge-voltage control.
//
// A PI loop on (target_voltage - battery) drives an abstract field demand (wk::alternator_duty, 0..100%).
// It never names a pin: a generic `outputs:` slot (cand = alternator_duty) realises the demand on a board
// pin, so the controller stays portable — exactly the Idle pattern.
//
//   error = target_voltage - battery        (low battery -> positive -> more field)
//   duty  = clamp( PI(error), 0, max_duty * soft_start_ramp )
//
// Engaged only while the engine is RUNNING and rpm >= min_rpm. Optional load-shed: above off_above_tps
// the field relaxes (frees a little power at WOT). A soft-start ramps the duty ceiling 0->max over
// soft_start_s after engaging so the electrical load comes on gently. When disengaged or disabled it
// publishes nothing, so the output failsafes to its default (regulator/no field).
// ---------------------------------------------------------------------------

class Alternator : public EngineModule {
public:
    void init(const AlternatorConfig& cfg);
    void on_config_change(const AlternatorConfig& cfg);

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

private:
    const AlternatorConfig* cfg_ = nullptr;
    PiController pi_;                 // integrator is the persistent field state
    uint32_t     last_ms_  = 0;       // for the PI dt
    uint32_t     engaged_since_ms_ = 0;   // soft-start anchor; 0 = currently disengaged

    void reset_state();
};
