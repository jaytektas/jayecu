#pragma once

#include "../EngineModule.h"
#include "../../../generated/modules/nitrous_config.h"

// ---------------------------------------------------------------------------
// Nitrous — single-stage dry nitrous control. Active when armed at (near) wide-open throttle inside the
// RPM window with adequate bottle pressure. Publishes nitrous_active (energise the solenoid via an
// Outputs pin) and nitrous_retard (Ignition subtracts it from the advance). Wet-fuel enrichment is a
// separate fuel-table concern. A conventional nitrous-controller shape.
// ---------------------------------------------------------------------------

class Nitrous : public EngineModule {
public:
    void init(const NitrousConfig& cfg)            { cfg_ = &cfg; active_ = false; }
    void on_config_change(const NitrousConfig& cfg) { cfg_ = &cfg; }
    void on_engine_stop() override                  { active_ = false; }

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

    [[nodiscard]] bool active() const { return active_; }

private:
    const NitrousConfig* cfg_ = nullptr;
    bool active_ = false;
};
