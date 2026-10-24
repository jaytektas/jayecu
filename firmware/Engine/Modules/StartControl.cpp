#include "StartControl.h"
#include "ElectronicThrottle.h"
#include "well_known_signals.h"
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../../generated/signal_enums.h"   // EngineRunState
#include "../../Platform/platform_hal.h"
#include "../../../generated/signal_ids.h"

extern bool g_system_active;   // key-on

// How long a granted request waits for the engine to start turning before it stops standing in for the
// button. The starter output engages the frame crank_req goes true unless one of its own interlocks
// (neutral, clutch) says no; a request held past that would crank the moment the clutch went down,
// with nobody's finger on the button.
static constexpr uint32_t GRANT_HOLD_MS = 1500;

void StartControl::update(const EnginePosition& /*pos*/, SignalBus& bus, EngineFrame& /*frame*/) {
    const uint32_t now = platform_get_tick_ms();
    const bool sw      = bus.get_bool(SIG_START_SW);
    const bool press   = sw && !sw_prev_;
    sw_prev_ = sw;
    const auto run = static_cast<EngineRunState>(static_cast<int>(bus.get(wk::engine_state, 0.0f)));

    // ENDS: key off, the engine running, or no config. Each leaves the next press to start from scratch.
    if (!cfg_ || !g_system_active || run == EngineRunState::RUNNING) state_ = IDLE;

    if (state_ == IDLE && press && cfg_ && g_system_active && run != EngineRunState::RUNNING) {
        state_ = WAIT_THROTTLE; press_ms_ = now; follow_sw_ = false;
    }

    if (state_ == WAIT_THROTTLE || state_ == PRIMING) {
        const uint32_t waited = now - press_ms_;
        const bool throttle_ok = !etb_ || etb_->start_proven();
        bool fuel_ok = true;
        if (cfg_->prime_enable) {
            // A READING RAIL decides, and decides at once when a warm rail is still up; a rail with no
            // reading (no sensor, unwired, failed) falls back to the clock, so a dead sensor never stops a
            // start. Either way the pump has been running since the press.
            const SignalId ps = (cfg_->press_src >= 0) ? static_cast<SignalId>(cfg_->press_src) : SIG_NONE;
            if (bus.valid(ps))
                fuel_ok = bus.get(ps, 0.0f) >= static_cast<float>(cfg_->min_press_kpa) * 0.1f
                       || waited >= cfg_->max_wait_ms;
            else
                fuel_ok = waited >= cfg_->prime_ms;
        }
        if (throttle_ok && fuel_ok)          { state_ = CRANK; grant_ms_ = now; }
        else if (waited >= cfg_->give_up_ms) state_ = IDLE;
        else                                 state_ = throttle_ok ? PRIMING : WAIT_THROTTLE;
    }

    bool crank = false;
    if (state_ == CRANK) {
        // GRANTED until the starter has had its chance — the engine turning, or GRANT_HOLD_MS without it —
        // then the BUTTON again: hold it for another crank after the starter's rest, let go and it is over.
        if (!follow_sw_ && (run != EngineRunState::STOPPED || now - grant_ms_ >= GRANT_HOLD_MS)) follow_sw_ = true;
        crank = follow_sw_ ? sw : true;
        if (follow_sw_ && !sw) { state_ = IDLE; crank = false; }
    }

    bus.set_bool(SIG_START_PUMP, state_ != IDLE && cfg_ && cfg_->prime_enable, now, ttl());
    bus.set_bool(SIG_CRANK_REQ, crank, now, ttl());
    bus.set(SIG_START_STATE, static_cast<float>(state_), true, now, ttl());
}
