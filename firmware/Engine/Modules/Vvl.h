#pragma once
#include "../EngineModule.h"
#include "../../../generated/modules/vvl_config.h"

// VVL — variable valve lift (VTEC-style) high-lift cam engagement. Discrete solenoid: engage above on_rpm
// once past the load + warm-up floors; disengage below off_rpm. RPM hysteresis (on_rpm > off_rpm) stops
// solenoid chatter around the crossover. Publishes vvl_active — drive the solenoid via a generic output.
class Vvl : public EngineModule {
public:
    void init(const VVLConfig& cfg)            { cfg_ = &cfg; engaged_ = false; }
    void on_config_change(const VVLConfig& cfg) { cfg_ = &cfg; }
    void on_engine_stop() override              { engaged_ = false; }
    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;
    [[nodiscard]] bool engaged() const { return engaged_; }
private:
    const VVLConfig* cfg_ = nullptr;
    bool engaged_ = false;
};
