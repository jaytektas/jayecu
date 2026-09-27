#pragma once

#include "../EngineModule.h"
#include "../CutDuty.h"
#include "../../../generated/modules/launch_config.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// Launch — the second RPM limiter that holds the engine at the line, and the calibration it runs on
// while it does. Armed by a CONDITION; disarmed it does nothing at all and the primary RevLimiter
// still owns the hard over-rev cut.
//
// ARMING IS ONE EXPRESSION (`arm_expr`; empty = standing still with the throttle open). It replaced a signal
// selector plus a speed gate plus a throttle gate — three fields spelling out one sentence that still
// could not say "the launch switch AND the brake released". Everything the reference offers as five
// wiring positions and four validations is one line here. It fails to FALSE, which for arming is the
// safe direction, and a program that will not compile arms nothing and raises LAUNCH_EXPR.
//
// THE LIMIT IS A TABLE. `end_rpm_table` is the RPM at which the cut is total — the "End RPM". Its
// axes are load and road speed and both ship OFF, so out of the box it is the single number the old
// `launch_rpm` scalar was; switch one on and the limit moves with boost, or climbs as the car rolls.
//
// HOW IT CUTS. Hard Cut is the classic two-step: everything at the End RPM, released a resume band
// below it. Soft Cut ramps a cut PERCENTAGE in across `cut_range_rpm` below the End RPM, delivered
// frame by frame through CutDuty exactly as RevLimiter's soft band is — the engine leans on the
// limiter instead of bouncing off it. With cut_method Both, `cut_adder_rpm` staggers the second cut
// that many RPM above the leading one (`cut_lead`), which is the reference's five cut methods
// expressed as the three-option enum six modules share plus the two facts that actually differ.
//
// WHILE IT HOLDS, THE ENGINE RUNS ON THE LAUNCH MAPS. `ign_advance_table` is published as
// wk::launch_ign_adv and is the ABSOLUTE advance — Ignition takes it in place of the main map the way
// the cranking table does, with every retard still subtracting underneath — and `fuel_corr_table`
// becomes wk::fuel_corr_launch, one more multiplier in FuelCalculator's chain. Both are published
// neutral (0 deg / 1.0x) whenever launch is not active, so nothing of a launch survives it.
//
// Publishes wk::launch_active, wk::launch_end_rpm, wk::launch_cut_pct. Resets on engine stop.
// ---------------------------------------------------------------------------

class DtcManager;

class Launch : public EngineModule {
public:
    // What a cut cuts, in schema order — the SAME order in every module that cuts.
    enum CutMethod : uint8_t { CutFuel = 0, CutIgnition = 1, CutBoth = 2 };
    enum CutType   : uint8_t { HardCut = 0, SoftCut = 1 };
    enum CutLead   : uint8_t { LeadIgnition = 0, LeadFuel = 1 };

    void init(const LaunchConfig& cfg);
    void on_config_change(const LaunchConfig& cfg);
    void on_engine_stop() override;
    void set_dtc(DtcManager* d) noexcept { dtc_ = d; }

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

private:
    // One cut channel's state: the hysteresis latch a hard cut needs and the accumulator a soft one
    // spreads its duty through. Fuel and ignition each own one, because with a stagger they are at
    // different thresholds and cannot share a decision.
    struct Channel {
        CutDuty duty;
        bool    latched = false;
    };

    void reset();
    void revalidate();
    // The do-nothing answer. Every exit takes it, so no stale cut, advance or fuel multiplier can
    // outlive the condition that produced it.
    void publish_idle(SignalBus& bus, uint32_t now);
    // Decide one cut channel at its own threshold. Returns the percentage being cut (0..100) and sets
    // `fire` for whether THIS frame cuts.
    float cut_channel(float rpm, float threshold, Channel& ch, bool& fire) const;

    const LaunchConfig* cfg_ = nullptr;
    DtcManager*         dtc_ = nullptr;

    Channel fuel_;
    Channel ign_;

    // The timeout is a state machine, not a comparison: once a launch has run out of time it stays
    // disarmed until the arm condition goes false and true again, so it cannot simply resume.
    bool     armed_prev_ = false;
    bool     timed_out_  = false;
    uint32_t armed_ms_   = 0;

    uint32_t cfg_gen_seen_ = 0xFFFFFFFFu;   // forces a revalidate on the first update
    bool     expr_bad_     = false;
};
