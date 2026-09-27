#pragma once

#include "../EngineModule.h"
#include "../CutDuty.h"
#include "../../../generated/modules/rev_limiter_config.h"

// ---------------------------------------------------------------------------
// RevLimiter — safety-first rev limit with soft-cut and hysteresis.
//
// Runs FIRST in the pipeline so later modules can check frame.fuel_cut /
// frame.ign_cut and frame.effective_rpm_limit.
//
// Soft cut:  RPM between soft_limit and hard_limit → progressively cut fuel
//            duty cycle (0→100% cut window over the band).
// Hard cut:  RPM >= hard_limit → full cut via cut_method.
// Hysteresis: cut releases when RPM drops below hard_limit - resume_band.
//
// Outputs (soft_cut_pct / fuel_cut / ign_cut) are published to the SignalBus;
// rpm is the decoder's channel (this module consumes it, never publishes it).
// ---------------------------------------------------------------------------

class RevLimiter : public EngineModule {
public:
    void init(const RevLimiterConfig& cfg);
    void on_config_change(const RevLimiterConfig& cfg);
    void on_engine_stop() override;

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

private:
    const RevLimiterConfig* cfg_    = nullptr;
    bool                    in_cut_ = false;   // hysteresis state
    CutDuty                 soft_duty_;        // the soft band's cut, spread across frames not thresholded
};
