#pragma once

#include "../EngineModule.h"
#include "../../../generated/modules/dfco_config.h"

// ---------------------------------------------------------------------------
// Dfco — deceleration (overrun) fuel cut. On a closed throttle, warm engine, above rpm_high, cut fuel
// (economy, emissions, engine braking, decel-backfire control); resume below rpm_low or when the
// throttle opens (RPM hysteresis so it doesn't chatter around one RPM).
//
// It ORs its cut into the shared cut (frame/wk::fuel_cut) and never clears another module's cut, so it
// composes with the rev limiter / launch / overboost cuts. Publishes dfco_active for telemetry.
// ---------------------------------------------------------------------------

class Dfco : public EngineModule {
public:
    void init(const DfcoConfig& cfg)            { cfg_ = &cfg; active_ = false; }
    void on_config_change(const DfcoConfig& cfg) { cfg_ = &cfg; }
    void on_engine_stop() override               { active_ = false; }

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

    [[nodiscard]] bool active() const { return active_; }

private:
    const DfcoConfig* cfg_    = nullptr;
    bool              active_ = false;   // hysteresis state (cut latched between rpm_low..rpm_high)
};
