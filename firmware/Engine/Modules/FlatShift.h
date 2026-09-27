#pragma once

#include "../EngineModule.h"
#include "../../../generated/modules/flat_shift_config.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// FlatShift — clutchless / full-throttle upshift cut. While the shift trigger (clutch switch or a shift
// button, a bus signal) is active at high RPM + throttle, cut ignition (or fuel/both) to unload the
// drivetrain so the next gear engages without lifting. The cut is time-limited per shift (max_cut_ms):
// a stuck trigger releases the cut so it can't hold indefinitely.
// ORs into frame/wk::ign_cut (+ fuel_cut), never clearing another module's cut.
// ---------------------------------------------------------------------------

class FlatShift : public EngineModule {
public:
    void init(const FlatShiftConfig& cfg)            { cfg_ = &cfg; reset(); }
    void on_config_change(const FlatShiftConfig& cfg) { cfg_ = &cfg; }
    void on_engine_stop() override                    { reset(); }

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

    [[nodiscard]] bool active() const { return active_; }

private:
    void reset() { active_ = false; held_ms_ = 0.0f; last_ms_ = 0; }

    const FlatShiftConfig* cfg_ = nullptr;
    bool     active_  = false;
    float    held_ms_ = 0.0f;   // continuous cut time this shift
    uint32_t last_ms_ = 0;
};
