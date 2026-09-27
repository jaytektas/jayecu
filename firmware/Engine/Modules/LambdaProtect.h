#pragma once

#include "../EngineModule.h"
#include "../../../generated/modules/lambda_protect_config.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// LambdaProtect — lean-protection fuel cut. If measured lambda stays leaner than max_lambda while under
// load (TPS + RPM above the monitor thresholds) for longer than timeout_ms, cut fuel to stop a lean
// burndown. Cutting fuel IS the protection: no fuel -> no lean combustion. The cut latches until the
// driver lifts (load drops out of the monitor window) — a fuel cut keeps lambda reading lean, so it
// can't self-clear. ORs into wk::fuel_cut, never clearing another cut.
// ---------------------------------------------------------------------------

class LambdaProtect : public EngineModule {
public:
    void init(const LambdaProtectConfig& cfg)            { cfg_ = &cfg; reset(); }
    void on_config_change(const LambdaProtectConfig& cfg) { cfg_ = &cfg; }
    void on_engine_stop() override                        { reset(); }

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

    [[nodiscard]] bool cut() const { return cut_; }

private:
    void reset() { cut_ = false; bad_ms_ = 0.0f; last_ms_ = 0; }

    const LambdaProtectConfig* cfg_ = nullptr;
    bool     cut_     = false;
    float    bad_ms_  = 0.0f;   // accumulated lean-under-load time
    uint32_t last_ms_ = 0;
};
