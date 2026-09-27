#pragma once
#include "../EngineModule.h"
#include "../PiController.h"
#include "../../../generated/modules/cruise_control_config.h"
#include "../../../generated/signal_enums.h"
#include "../../Diagnostics/DtcManager.h"
#include <cstdint>

// CruiseControl — closed-loop road-speed hold.
//
// WHAT THE DRIVER PRESSES IS A CONDITION, not a switch id. Each action carries its own expression
// (`cruise_sw == 2`, `switch_1`, a CAN flag, `cruise_sw == 2 and vehicle_spd > 40`), so one button can
// mean several things by appearing in several of them — the same position in Set When and Speed Down
// When is a Set/Coast button, with no table of button functions anywhere. The VM that answers those
// conditions is PURE and STATELESS by design, so it tells this module only what is true RIGHT NOW:
// every edge, every tap-versus-hold decision and every timeout lives here.
//
// THE STATE IS THE MODULE. Off -> Disabled <-> Ready <-> Cruising, with Fault blocking engagement.
// Cancel keeps the set speed so Resume has something to return to; Disable and Fault throw it away.
// Off is not Disabled: "switched off in the tune" and "the driver's main switch is off" are different
// facts, and a dash lamp that conflates them lies about which one you are looking at.
//
// It publishes cruise_demand as a floor UNDER the pedal (ElectronicThrottle takes the higher of the
// two), so the driver always overrides by pressing harder, and this module never touches an output.
class CruiseControl : public EngineModule {
public:
    // The conditions, in one order used by the expression cache, the edge state and the dispatch.
    enum Btn : uint8_t { BTN_ENABLE, BTN_DISABLE, BTN_TOGGLE, BTN_SET,
                         BTN_RESUME, BTN_CANCEL, BTN_UP, BTN_DOWN, BTN_COUNT };

    // Why cruise will not engage, one bit each, published as cruise_inhibit. A refusal with no reason
    // attached is indistinguishable from a module that is not running, which is the complaint this
    // channel exists to answer. More bits arrive with the rest of the interlocks.
    enum Inhibit : uint32_t {
        INH_BRAKE        = 1u << 0,
        INH_CLUTCH       = 1u << 1,
        INH_HANDBRAKE    = 1u << 2,
        INH_SPEED_LOW    = 1u << 3,
        INH_SPEED_HIGH   = 1u << 4,
        INH_VSS_INVALID  = 1u << 5,
        INH_VSS_STALE    = 1u << 6,
        INH_VSS_IMPL     = 1u << 7,
        INH_WHEEL_DIFF   = 1u << 8,
        INH_SW_INVALID   = 1u << 9,
        INH_EXPR_BAD     = 1u << 10,
        INH_RUNAWAY      = 1u << 11,
        INH_RPM_LOW      = 1u << 12,
        INH_RPM_HIGH     = 1u << 13,
        INH_GEAR         = 1u << 14,
        INH_PEDAL_FAULT  = 1u << 15,
        INH_UNMONITORED  = 1u << 17,
        INH_NO_BRAKE     = 1u << 18,
    };

    // Reasons that mean the ECU no longer trusts its own picture, rather than the driver having asked
    // for something. These go to Fault, which BLOCKS re-engagement until every condition has cleared
    // and every button has been released — a plain Cancel does neither.
    static constexpr uint32_t kFaultMask =
        INH_VSS_INVALID | INH_VSS_STALE | INH_VSS_IMPL | INH_WHEEL_DIFF | INH_SW_INVALID |
        INH_EXPR_BAD | INH_RUNAWAY | INH_PEDAL_FAULT | INH_UNMONITORED | INH_NO_BRAKE;

    // The inputs whose cancel is IMMEDIATE. A brake press means the driver is already decelerating, and
    // cruise_demand is a floor under the pedal that can only ever ADD throttle — so bleeding it away
    // over half a second, however gentle that is elsewhere, is the one place it is wrong.
    static constexpr uint32_t kHardCancel = INH_BRAKE | INH_CLUTCH | INH_HANDBRAKE;

    void init(const CruiseControlConfig& cfg) { cfg_ = &cfg; reset(); }
    void set_dtc(DtcManager* d) { dtc_ = d; }
    void on_engine_stop() override            { reset(); }
    void update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) override;

    [[nodiscard]] bool        active()  const { return state_ == CruiseState::CRUISING; }
    [[nodiscard]] CruiseState state()   const { return state_; }
    [[nodiscard]] uint32_t    inhibit() const { return inhibit_; }
    [[nodiscard]] float       target()  const { return target_kph_; }

private:
    void reset();
    // Re-validate every program when the tune changes. exec() assumes validate() has passed, and
    // on_config_change is dead API here, so the module watches g_config_generation itself.
    void revalidate();
    // One code per reason, raised and healed together so the table always matches inhibit_.
    void raise_codes(uint32_t now);
    void heal_all();

    const CruiseControlConfig* cfg_ = nullptr;
    PiController pi_;

    CruiseState state_      = CruiseState::OFF;
    float       target_kph_ = 0.0f;      // kept across a Cancel so Resume has somewhere to go
    uint32_t    inhibit_    = 0;
    uint32_t    last_ms_    = 0;

    // Per condition: the level last frame, when this press began, and whether it has already been
    // acted on as a HOLD — which is what stops one press from being reported as a tap as well.
    bool     prev_[BTN_COUNT]       = {};
    uint32_t press_ms_[BTN_COUNT]   = {};
    bool     long_fired_[BTN_COUNT] = {};

    uint32_t cfg_gen_seen_ = 0xFFFFFFFFu;   // forces a revalidate on the first update
    bool     expr_bad_[BTN_COUNT] = {};

    // The published floor, and the slew that governs it. ramp_valid_ is the "snap on the first frame"
    // flag: engagement puts the floor exactly where the pedal already was, and the rate limit applies
    // to everything AFTER that — a ramped hand-over would drop the throttle on the way in.
    DtcManager* dtc_ = nullptr;
    bool  was_enabled_ = false;      // heal on the enabled->disabled EDGE — see DtcManager::heal

    // Things that must be true for a while before they count. A single bad frame is noise; latching a
    // fault on one is how a module ends up permanently faulted by a glitch it never saw again.
    // WHY A FAULT LATCHES. Some causes stop being measurable the moment they take effect — the runaway
    // timer only runs while cruising, so one frame after it trips there is nothing left to measure and
    // the fault would clear itself, rearm, and let cruise straight back in. The reason is held here
    // until the driver has released everything, which is what makes Fault different from Cancel.
    uint32_t latched_fault_ = 0;
    uint32_t runaway_ms_ = 0;        // how long the speed error has been beyond the limit
    uint32_t sw_bad_ms_  = 0;        // how long the stalk has read in no calibrated band
    float    last_speed_    = 0.0f;  // for the plausibility step check
    bool     have_last_speed_ = false;

    float last_err_   = 0.0f;
    float ramped_     = 0.0f;
    bool  ramp_valid_ = false;
    float decay_rate_ = 0.0f;   // %/s, set when a soft cancel starts bleeding the floor away
};
