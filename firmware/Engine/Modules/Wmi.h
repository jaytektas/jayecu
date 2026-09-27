#pragma once
#include "../EngineModule.h"
#include "../../../generated/modules/wmi_config.h"

// Wmi — water/methanol injection. Above the boost (MAP) threshold at load, run the WMI pump with a duty
// ramping from min_duty at min_map_kpa to 100% at full_map_kpa. Publishes wmi_active (pump enable -> an
// Outputs pin) + wmi_duty (a PWM %). A measured-flow failsafe is a later refinement.
class Wmi : public EngineModule {
public:
    void init(const WmiConfig& cfg)            { cfg_ = &cfg; }
    void on_config_change(const WmiConfig& cfg) { cfg_ = &cfg; }
    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;
    [[nodiscard]] float duty() const { return duty_; }
private:
    const WmiConfig* cfg_ = nullptr;
    float duty_ = 0.0f;
};
