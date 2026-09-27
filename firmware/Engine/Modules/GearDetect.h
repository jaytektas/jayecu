#pragma once

#include "../EngineModule.h"
#include "../../../generated/modules/gear_detect_config.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// GearDetect — derive the current gear from the rpm/speed ratio (rpm / vehicle_spd), matched to each
// gear's configured ratio within tol_pct. Publishes `gear` (1..gear_count, or 0 = unknown/neutral below
// the speed floor). The gear signal is already used as a fuel/ignition/boost/launch table axis but had
// no producer — this closes that dangling axis.
// ---------------------------------------------------------------------------

class GearDetect : public EngineModule {
public:
    void init(const GearDetectConfig& cfg)            { cfg_ = &cfg; gear_ = 0; }
    void on_config_change(const GearDetectConfig& cfg) { cfg_ = &cfg; }
    void on_engine_stop() override                     { gear_ = 0; }

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

    [[nodiscard]] uint8_t gear() const { return gear_; }

private:
    const GearDetectConfig* cfg_  = nullptr;
    uint8_t                 gear_ = 0;
};
