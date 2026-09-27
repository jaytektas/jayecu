#pragma once

#include "../EngineModule.h"
#include "../../../generated/modules/pit_limiter_config.h"

// ---------------------------------------------------------------------------
// PitLimiter — vehicle-speed limiter (pit lane / rolling limit). While engaged (an arm signal, or always
// when enabled) it cuts fuel/ignition above speed_kph with a hysteresis band, so the car holds the pit
// limit. Distinct from the rev limiter (RPM) and launch (2-step). ORs into wk::fuel_cut/ign_cut.
// ---------------------------------------------------------------------------

class PitLimiter : public EngineModule {
public:
    void init(const PitLimiterConfig& cfg)            { cfg_ = &cfg; active_ = false; }
    void on_config_change(const PitLimiterConfig& cfg) { cfg_ = &cfg; }
    void on_engine_stop() override                     { active_ = false; }

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

    [[nodiscard]] bool active() const { return active_; }

private:
    const PitLimiterConfig* cfg_ = nullptr;
    bool active_ = false;
};
