#pragma once

#include "../EngineModule.h"
#include "../PiController.h"
#include "../CutDuty.h"
#include "../../../generated/modules/traction_control_config.h"
#include "../../Diagnostics/DtcManager.h"
#include <cstdint>

// TractionControl — how much of the engine the road can actually take.
//
// SLIP IS A RATIO, so this module never needs to know the true road speed: an absolute calibration
// error cancels out of (driven − reference) / reference. What does NOT cancel is the two sources
// disagreeing with each other — a front pickup 5% out reads as 5% slip forever on a car doing nothing
// wrong — which is what TRACTION_AXLE_CAL exists to say.
//
// THE REFERENCE IS THE ROAD SPEED, and this module does not get a say in what that means. It briefly
// had its own Reference Speed setting — Undriven Axle / Vehicle Speed / Slowest Wheel — which was a
// SECOND answer to a question the ECU already answers once, in VehicleSpeed's Main Source. Two settings
// for one fact can disagree, and nothing would have said which was right.
//
// Nothing is lost, because the right Main Source is the same one for both jobs: on a two-wheel-drive car
// the undriven axle is the honest road speed AND the honest slip reference, and on four-wheel drive the
// only engine-independent speed is GPS — a pickup like any other. Where the two would differ (a gearbox
// pickup, which inherits the driven wheels' spin) there are no wheel speeds to compare it against
// anyway, so this module has nothing to do.
//
// THREE PATHS, EACH DOING WHAT IT IS GOOD AT. A throttle ceiling is smooth and slow, so it is the PI
// loop: an integral is what HOLDS a slip target rather than settling near it. Timing retard and cut are
// immediate and blunt, so they are tables read on slip error — the response is not linear in slip, and
// a table says what each amount is worth instead of pretending one gain fits all of it.
class TractionControl : public EngineModule {
public:
    // Which sensor measures the driven side, in schema order.
    enum DrivenSrc : uint8_t { DrivenWheels = 0, DriveTrain = 1 };
    // What a cut cuts, in schema order — the SAME order in every module that cuts.
    enum CutMethod : uint8_t { CutFuel = 0, CutIgnition = 1, CutBoth = 2 };

    void init(const TractionControlConfig& cfg)             { cfg_ = &cfg; reset(); }
    void on_config_change(const TractionControlConfig& cfg)  { cfg_ = &cfg; }
    void set_dtc(DtcManager* d)                              { dtc_ = d; }
    void on_engine_stop() override                           { reset(); }

    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

    [[nodiscard]] float cap()     const { return cap_; }        // published throttle ceiling (%)
    [[nodiscard]] float slip()    const { return slip_; }       // measured slip (%)
    [[nodiscard]] float retard()  const { return retard_; }     // timing pulled (deg)
    [[nodiscard]] float cut_pct() const { return cut_pct_; }    // cut duty asked for (%)
    [[nodiscard]] bool  cutting() const { return cutting_; }    // …and whether THIS frame is cut

private:
    void reset();
    void revalidate();
    // Publish the do-nothing answer. Every exit takes it, so there is no path on which a stale cap, a
    // stale retard or a stale cut survives the condition that produced it.
    void publish_neutral(SignalBus& bus, uint32_t now);

    const TractionControlConfig* cfg_ = nullptr;
    DtcManager* dtc_ = nullptr;
    PiController pi_;

    float cap_     = 100.0f;
    float slip_    = 0.0f;
    float retard_  = 0.0f;
    float cut_pct_ = 0.0f;
    bool  cutting_ = false;

    // The throttle REDUCTION (100 − cap), held across frames because giving it back is rate-limited:
    // handing full throttle to wheels that have only just stopped spinning is how the next slip starts.
    float reduction_ = 0.0f;
    uint32_t last_ms_ = 0;

    CutDuty cut_duty_;              // a cut percentage, spread across frames — see CutDuty.h

    uint32_t cfg_gen_seen_ = 0xFFFFFFFFu;   // forces a revalidate on the first update
    bool     expr_bad_ = false;

    // Slip seen while CRUISING and not intervening. Steady slip with the engine doing nothing about it
    // is not wheelspin — it is the two speed sources disagreeing, and it is worth a code because it
    // silently biases every slip measurement the module makes.
    float    cal_ema_    = 0.0f;
    uint32_t cal_bad_ms_ = 0;
    bool     cal_raised_ = false;

    bool was_enabled_ = false;      // heal on the enabled->disabled EDGE — see DtcManager::heal
};
