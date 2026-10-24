#include "StartControl.h"
#include "ElectronicThrottle.h"
#include "well_known_signals.h"
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../../generated/signal_enums.h"   // EngineRunState
#include "../../Platform/platform_hal.h"
#include "../../../generated/signal_ids.h"
#include "../../Diagnostics/DtcManager.h"
#include "../../Diagnostics/Dtc.h"
#include "../../Diagnostics/TextLog.h"           // g_text_log — why a start was slow or dropped

extern bool g_system_active;   // key-on

// How long a granted request waits for the engine to start turning before it stops standing in for the
// button. The starter output engages the frame crank_req goes true unless one of its own interlocks
// (neutral, clutch) says no; a request held past that would crank the moment the clutch went down,
// with nobody's finger on the button.
static constexpr uint32_t GRANT_HOLD_MS = 1500;

// The rail did not reach Crank Above Pressure in Longest Pressure Wait. Level 1: recorded and on the lamp,
// never a limp — the engine was cranked anyway, and may well have started. Latched (an event, not a
// condition anything re-asserts) until a start reaches pressure.
constexpr uint16_t P_START_RAIL_LOW = 0x0087;
constexpr uint8_t  S_START_RAIL_LOW = DTC_SEV_LEVEL1;

void StartControl::update(const EnginePosition& /*pos*/, SignalBus& bus, EngineFrame& /*frame*/) {
    const uint32_t now = platform_get_tick_ms();
    const bool sw      = bus.get_bool(SIG_START_SW);
    const bool press   = sw && !sw_prev_;
    sw_prev_ = sw;
    const auto run = static_cast<EngineRunState>(static_cast<int>(bus.get(wk::engine_state, 0.0f)));

    // OFF: the button straight through, so a starter output reading crank_req cranks exactly as one reading
    // start_sw would, and nothing asks for the pump. A code this module raised goes with it (enabled ->
    // disabled edge), as every module's does.
    if (!cfg_ || !cfg_->enabled) {
        if (was_enabled_ && dtc_) dtc_->heal(P_START_RAIL_LOW);
        was_enabled_ = false;
        state_ = IDLE;
        bus.set_bool(SIG_START_PUMP, false, now, ttl());
        bus.set_bool(SIG_CRANK_REQ, sw, now, ttl());
        bus.set(SIG_START_STATE, static_cast<float>(IDLE), true, now, ttl());
        return;
    }
    was_enabled_ = true;

    // ENDS: key off or the engine running. Each leaves the next press to start from scratch.
    if (!g_system_active || run == EngineRunState::RUNNING) state_ = IDLE;

    if (state_ == IDLE && press && g_system_active && run != EngineRunState::RUNNING) {
        state_ = WAIT_THROTTLE; press_ms_ = now; follow_sw_ = false;
    }

    if (state_ == WAIT_THROTTLE || state_ == PRIMING) {
        const uint32_t waited = now - press_ms_;
        const bool throttle_ok = !etb_ || etb_->start_proven();
        bool fuel_ok = true, rail_read = false;
        if (cfg_->prime_enable) {
            // A READING RAIL decides, and decides at once when a warm rail is still up; a rail with no
            // reading (no sensor, unwired, failed) falls back to the clock, so a dead sensor never stops a
            // start. Either way the pump has been running since the press.
            const SignalId ps = (cfg_->press_src >= 0) ? static_cast<SignalId>(cfg_->press_src) : SIG_NONE;
            rail_read = bus.valid(ps);
            if (rail_read) {
                const float rail = bus.get(ps, 0.0f);
                const float need = static_cast<float>(cfg_->min_press_kpa) * 0.1f;
                const bool  up   = rail >= need;
                fuel_ok = up || waited >= cfg_->max_wait_ms;
                if (throttle_ok && fuel_ok) {               // the moment it is granted: say how the rail did
                    if (up) {
                        if (dtc_) dtc_->heal(P_START_RAIL_LOW);
                    } else {
                        if (dtc_) dtc_->raise(P_START_RAIL_LOW, DtcSource::MODULE, S_START_RAIL_LOW, now, DTC_TTL_LATCH);
                        g_text_log.printf("start: rail %d kPa after %u ms (wanted %d) - cranking anyway\n",
                                          static_cast<int>(rail), static_cast<unsigned>(waited), static_cast<int>(need));
                    }
                }
            } else {
                fuel_ok = waited >= cfg_->prime_ms;
            }
        }
        if (throttle_ok && fuel_ok) { state_ = CRANK; grant_ms_ = now; }
        else if (waited >= cfg_->give_up_ms) {
            g_text_log.printf("start: %s after %u ms - press dropped\n",
                              !throttle_ok ? "throttle not proven (key-on check)"
                              : rail_read  ? "rail not up" : "prime time not finished",
                              static_cast<unsigned>(waited));
            state_ = IDLE;
        }
        else state_ = throttle_ok ? PRIMING : WAIT_THROTTLE;
    }

    bool crank = false;
    if (state_ == CRANK) {
        // GRANTED until the starter has had its chance — the engine turning, or GRANT_HOLD_MS without it —
        // then the BUTTON again: hold it for another crank after the starter's rest, let go and it is over.
        if (!follow_sw_ && (run != EngineRunState::STOPPED || now - grant_ms_ >= GRANT_HOLD_MS)) follow_sw_ = true;
        crank = follow_sw_ ? sw : true;
        if (follow_sw_ && !sw) { state_ = IDLE; crank = false; }
    }

    bus.set_bool(SIG_START_PUMP, state_ != IDLE && cfg_->prime_enable, now, ttl());
    bus.set_bool(SIG_CRANK_REQ, crank, now, ttl());
    bus.set(SIG_START_STATE, static_cast<float>(state_), true, now, ttl());
}
