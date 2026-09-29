#pragma once

#include "../EngineModule.h"
#include "../../../generated/modules/fuel_calculator_config.h"

// ---------------------------------------------------------------------------
// FuelTrim — produces the SLOW fuel corrections OFF the per-cycle hot path. FuelCalculator's per-
// cycle compute aggregates a fixed set of fuel_corr_* bus signals; the half driven by SLOW sensors
// — coolant, IAT, ethanol, baro, gear + the 4 generic tables — change far slower than the engine
// cycle, so recomputing their tables every cycle was 50–65% of the per-cycle fuel cost (the profiled
// "corr" stage). The inputs are genuinely slow: a typical IAT thermistor's thermal time constant is
// ~15 s (≈0.01 Hz), so recomputing at per-cycle rate (13–133 Hz) is heavy oversampling.
//
// Runs in the 1 kHz MODULE phase: re-evaluates exactly ONE table per frame (round-robin), caches the
// result, and RE-PUBLISHES all corrections every frame from the cache — so each fuel_corr_* stays
// fresh at 1 kHz (no staleness for the per-cycle aggregator's neutral-default reads) while the heavy
// interpolation is distributed one-per-frame. The full set refreshes every N ms. FuelCalculator just
// reads the signals off the bus, unchanged. Same shape as IgnitionTrim — "coolant asks here, fuel
// decides there."
//
// SLOW only: cranking + post-start (lifecycle-coupled) and map / rev-limit / dead-time (rpm/MAP-
// driven, genuinely fast) stay per-cycle in FuelCalculator.
// ---------------------------------------------------------------------------

class FuelTrim : public EngineModule {
public:
    void init(const FuelCalculatorConfig& cfg);
    void on_config_change(const FuelCalculatorConfig& cfg);
    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

private:
    static constexpr int N = 12;  // warmup(clt), iat, fuelcomp (stage 1), baro, gear, generic1..4, fuelcomp stages 2..4
    const FuelCalculatorConfig* cfg_ = nullptr;
    float   pct_[N] = {};         // last raw table value (%) of each slow correction
    uint8_t rr_ = 0;              // round-robin: which table we refresh this frame
};
