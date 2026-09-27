#pragma once
#include "../EngineModule.h"
#include "../../../generated/modules/torque_model_config.h"

// TorqueModel — table-based engine brake-torque estimate. A calibratable torque map (rpm x load)
// gives reference brake torque at each operating point; power
// follows as P = T*omega. Publishes engine_torque_nm + engine_power_kw for telemetry/logging and as
// the building block a future torque-based throttle map would invert. Disabled -> publishes zeros.
class TorqueModel : public EngineModule {
public:
    void init(const TorqueModelConfig& cfg)            { cfg_ = &cfg; }
    void on_config_change(const TorqueModelConfig& cfg) { cfg_ = &cfg; }
    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;
private:
    const TorqueModelConfig* cfg_ = nullptr;
    float torque_nm_ = 0.0f;
    float power_kw_  = 0.0f;
};
