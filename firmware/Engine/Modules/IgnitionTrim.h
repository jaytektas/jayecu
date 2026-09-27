#pragma once

#include "../EngineModule.h"
#include "../../../generated/modules/ignition_config.h"

// ---------------------------------------------------------------------------
// IgnitionTrim — produces the SLOW ignition-advance corrections OFF the per-cycle
// hot path. The commanded advance is base_map (rpm×load, fast — stays in
// Ignition) PLUS these trims: coolant, IAT, gear, the four generic
// tables, and post-start. Their inputs change far slower than the engine cycle,
// so recomputing them per-cycle is wasted work — and a 933 µs lump.
//
// This runs in the 1 kHz MODULE phase and recomputes exactly ONE table per frame
// (round-robin), sums the cached contributions, and publishes wk::ign_advance_trim.
// The heavy interpolation is DISTRIBUTED — one per frame, full refresh every N
// frames — never the whole stack at once. Ignition just reads the one
// number. "Coolant asks (here), ignition decides (there)."
//
// Slow trims only: the rpm-driven rev-limit trim and the base map stay per-cycle
// in Ignition (their inputs are genuinely fast).
// ---------------------------------------------------------------------------

class IgnitionTrim : public EngineModule {
public:
    void init(const IgnitionConfig& cfg);
    void on_config_change(const IgnitionConfig& cfg);
    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

private:
    static constexpr int N = 9;   // clt, iat, fuelcomp, gear, gen1..4, post_start
    const IgnitionConfig* cfg_ = nullptr;
    float   contrib_[N] = {};      // last value of each slow correction (deg)
    uint8_t rr_ = 0;               // round-robin: which one we refresh this frame
};
