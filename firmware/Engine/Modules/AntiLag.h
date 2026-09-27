#pragma once
#include "../EngineModule.h"
#include "../../../generated/modules/anti_lag_config.h"
#include <cstdint>

// AntiLag (ALS) — hold heavy ignition retard while armed OFF-throttle above the RPM floor, to keep the
// exhaust hot and the turbo spooled between shifts / on overrun. Time-limited per activation (max_time_ms)
// so it can't cook the turbo; re-arms when the throttle opens. Publishes antilag_retard (Ignition
// subtracts) + antilag_active. Wet fuel-add + throttle-crack are later refinements.
class AntiLag : public EngineModule {
public:
    void init(const AntiLagConfig& cfg)            { cfg_ = &cfg; reset(); }
    void on_config_change(const AntiLagConfig& cfg) { cfg_ = &cfg; }
    void on_engine_stop() override                  { reset(); }
    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;
    [[nodiscard]] bool active() const { return active_; }
private:
    void reset() { active_ = false; held_ms_ = 0.0f; last_ms_ = 0; }
    const AntiLagConfig* cfg_ = nullptr;
    bool     active_  = false;
    float    held_ms_ = 0.0f;
    uint32_t last_ms_ = 0;
};
