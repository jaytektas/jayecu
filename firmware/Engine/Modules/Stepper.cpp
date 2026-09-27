#include "Stepper.h"
#include "../../Diagnostics/DtcManager.h"
#include "../../../generated/module_dtc.h"
#include "../../Diagnostics/Dtc.h"
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../../generated/signal_ids.h"
#include "../../Platform/platform_hal.h"
#include <algorithm>
#include <cmath>

namespace { constexpr float TWO_PI = 6.28318530718f; }

// HOMING. A stepper has no position sensor, so at power-up (and whenever it is re-enabled) nobody
// knows where the valve is. The standard answer: assume it is fully open, plus a margin, and drive it
// closed that far — into its mechanical stop, where the extra steps simply slip — then call that 0.
// This used to set pos_ = 0 and trust it, and "park" on engine stop only zeroed the counter without
// moving the valve, so every stop added the valve's opening to an untracked offset and idle air drifted
// until the valve hit an end stop.
void Stepper::start_homing() noexcept {
    const int32_t range = cfg_ ? static_cast<int32_t>(cfg_->range_steps) : 0;
    pos_     = range + range / 10 + 1;      // "fully open + 10%": the whole travel, and then some
    homing_  = true;
}

void Stepper::init(const StepperConfig& cfg) noexcept {
    cfg_ = &cfg;
    was_enabled_ = false;
    start_homing();
}

void Stepper::on_config_change(const StepperConfig& cfg) noexcept {
    cfg_ = &cfg;
}

void Stepper::update(const EnginePosition& /*pos*/, SignalBus& bus, EngineFrame& /*frame*/) {
    const uint32_t now_ms = platform_get_tick_ms();
    if (!cfg_ || !cfg_->enabled) {
        if (was_enabled_) {
            bus.set(SIG_STEP_EN_A, 0.0f, true, now_ms, ttl());
            bus.set(SIG_STEP_EN_B, 0.0f, true, now_ms, ttl());
            bus.set(SIG_STEP_DEMAND_A, 0.0f, true, now_ms, ttl());
            bus.set(SIG_STEP_DEMAND_B, 0.0f, true, now_ms, ttl());
            bus.set(SIG_STEP_ENABLE, 0.0f, true, now_ms, ttl());
            bus.set(SIG_STEP_PULSE, 0.0f, true, now_ms, ttl());
            start_homing();              // position is unknown once the driver lets go — re-home on enable
        // Heal what we raised: this return skips the heal() in the enabled path — see DtcManager::heal.
            if (dtc_) dtc_->heal(ModuleDtc::STEP_IN);
            was_enabled_ = false;
        }
        return;
    }

    was_enabled_ = true;

    // --- Position demand + input-signal DTC (both modes) ---
    const SignalId input_id = static_cast<SignalId>(cfg_->input_sig);
    if (dtc_) {
        if (!bus.valid(input_id))
            dtc_->raise(ModuleDtc::STEP_IN, DtcSource::MODULE, ModuleDtc::STEP_IN_SEV, now_ms, dtc_ttl());
        else
            dtc_->heal(ModuleDtc::STEP_IN);
    }
    float demand = bus.get(input_id, 0.0f);
    demand = std::clamp(demand, 0.0f, 100.0f);
    // While homing the target is the closed stop, whatever is asked for; the valve is driven there from
    // "fully open + margin", and only then does the demand take over.
    // The service that arrives at the stop holds there; the demand takes over from the next one.
    const bool arrived = homing_ && pos_ <= 0;
    if (arrived) { homing_ = false; pos_ = 0; }
    const int32_t target = (homing_ || arrived) ? 0 : static_cast<int32_t>(
        std::lround(demand / 100.0f * static_cast<float>(cfg_->range_steps)));

    // --- Step-rate limit. step_period_ms is the time per FULL step. H-Bridge advances microsteps, so
    //     the per-microstep interval is step_period/microstep; Step/Direction advances driver steps
    //     (period per pulse). Clamp to >= 1 ms — the 1 kHz service can't emit faster than it runs. ---
    const bool     stepdir  = (cfg_->driver_mode != 0);
    const uint16_t period   = cfg_->step_period_ms ? cfg_->step_period_ms : 1;
    const uint16_t divisor  = stepdir ? 1 : (cfg_->microstep ? cfg_->microstep : 1);
    uint32_t       interval = period / divisor;
    if (interval < 1u) interval = 1u;
    const bool due = static_cast<uint32_t>(now_ms - last_step_ms_) >= interval;

    if (stepdir) {
        // --- Step/Direction: emit STEP pulses toward the target; DIR/ENABLE are held levels. The
        //     external driver owns microstepping — one pulse = one driver step. Coils stay off. ---
        const int32_t err = target - pos_;
        bool dir_fwd = (err >= 0);
        if (cfg_->dir_invert) dir_fwd = !dir_fwd;
        bus.set(SIG_STEP_DIR, dir_fwd ? 1.0f : 0.0f, true, now_ms, ttl());
        bus.set(SIG_STEP_ENABLE, 1.0f, true, now_ms, ttl());
        // A STEP IS AN EDGE. The pulse goes high for one service and is held low for at least the next,
        // whatever the rate: with step_period at the service period, `due` was true every service and
        // the line sat high — no falling edge, no steps counted by the driver, a position that drifted.
        float pulse = 0.0f;
        if (err != 0 && due && !pulse_high_) {
            pos_ += (err > 0) ? 1 : -1;
            last_step_ms_ = now_ms;
            pulse = 1.0f;
        }
        pulse_high_ = pulse > 0.0f;
        bus.set(SIG_STEP_PULSE, pulse, true, now_ms, ttl());
        bus.set(SIG_STEP_EN_A, 0.0f, true, now_ms, ttl());     // H-bridge coils off in this mode
        bus.set(SIG_STEP_EN_B, 0.0f, true, now_ms, ttl());
        return;
    }

    // --- H-Bridge: rate-limited microstep integrator, then render the two coils (cos/sin). ---
    if (pos_ != target && due) {
        int32_t step = target - pos_;
        const int32_t lim = static_cast<int32_t>(cfg_->max_step_per_update);
        if (step >  lim) step =  lim;
        if (step < -lim) step = -lim;
        pos_ += step;
        last_step_ms_ = now_ms;
    }

    bus.set(SIG_STEP_EN_A, 1.0f, true, now_ms, ttl());
    bus.set(SIG_STEP_EN_B, 1.0f, true, now_ms, ttl());
    bus.set(SIG_STEP_ENABLE, 0.0f, true, now_ms, ttl());   // external Step/Dir driver stays disabled in H-Bridge mode

    // Electrical angle: 4 full steps = one electrical revolution = 4*microstep microsteps.
    const uint16_t ms = cfg_->microstep > 0 ? cfg_->microstep : 1;
    const int32_t  er    = 4 * static_cast<int32_t>(ms);
    const int32_t  phase = ((pos_ % er) + er) % er;
    float theta = (static_cast<float>(phase) / static_cast<float>(er)) * TWO_PI;
    if (cfg_->dir_invert) theta = -theta;   // reverse rotation: sin flips, cos unchanged

    const bool moving = (pos_ != target);
    const float current = static_cast<float>(moving ? cfg_->move_current_pct : cfg_->hold_current_pct) * 0.1f;

    // Coil A ~ cos, coil B ~ sin. Signed duty: magnitude → PWM, sign → DIR (IHBridge::drive convention).
    bus.set(SIG_STEP_DEMAND_A, std::cos(theta) * current, true, now_ms, ttl());
    bus.set(SIG_STEP_DEMAND_B, std::sin(theta) * current, true, now_ms, ttl());
}

void Stepper::on_engine_stop() {
    // The valve is where it is. A stop does not move it, so the tracked position stays true — the idle
    // demand goes on positioning it for the next start. (This used to zero the counter without a step.)
    last_step_ms_ = 0;
}
