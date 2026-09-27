#include "Alternator.h"
#include "../EngineStateMachine.h"          // EngineRunState
#include "../../Platform/platform_hal.h"    // platform_get_tick_ms
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include <algorithm>

void Alternator::reset_state() {
    pi_.reset();
    last_ms_ = 0;
    engaged_since_ms_ = 0;
}

void Alternator::init(const AlternatorConfig& cfg) {
    cfg_ = &cfg;
    reset_state();
}

void Alternator::on_config_change(const AlternatorConfig& cfg) {
    cfg_ = &cfg;
    reset_state();   // a retune shouldn't carry stale wind-up
}

void Alternator::update(const EnginePosition& pos, SignalBus& bus, EngineFrame& frame) {
    (void)pos;
    (void)frame;
    if (!cfg_ || !cfg_->enabled) { reset_state(); return; }

    const uint32_t now  = platform_get_tick_ms();
    const auto state    = static_cast<EngineRunState>(static_cast<int>(bus.get(wk::engine_state, 0.0f)));
    const float rpm     = bus.get(wk::rpm, 0.0f);
    const float tps     = bus.get(wk::tps, 0.0f);

    // Gate: engine caught + above the enable RPM, and not shedding for power at high throttle.
    const bool shed = cfg_->off_above_tps > 0 && tps >= static_cast<float>(cfg_->off_above_tps) * 0.1f;
    const bool run  = (state == EngineRunState::RUNNING) && rpm >= static_cast<float>(cfg_->min_rpm);
    if (!run || shed) {
        // Disengaged: relax the field and reset so charging re-engages cleanly (and the ramp restarts).
        pi_.reset();
        engaged_since_ms_ = 0;
        last_ms_ = 0;
        return;                          // publish nothing -> the output failsafes
    }

    // Soft-start: ramp the duty ceiling from 0 to max over soft_start_s after engaging.
    if (engaged_since_ms_ == 0) engaged_since_ms_ = now;
    const float soft_s = cfg_->soft_start_s * 0.1f;
    const float ramp   = soft_s > 0.0f
        ? std::clamp((now - engaged_since_ms_) / 1000.0f / soft_s, 0.0f, 1.0f) : 1.0f;
    const float ceil   = static_cast<float>(cfg_->max_duty_pct) * 0.1f * ramp;

    pi_.kp      = cfg_->kp * 0.001f;     // %/V
    pi_.ki      = cfg_->ki * 0.001f;     // %/V/s
    pi_.out_min = 0.0f;
    pi_.out_max = ceil;

    // NO VOLTAGE, NO FIELD. A missing battery reading read as 0 V: the error became the whole target and
    // the field went to its ceiling — full charge into a battery nobody was measuring. Without the
    // measurement there is no closed loop to run, so the field is released (the output failsafes) exactly
    // as when the engine is not running, and the loop restarts cleanly when the reading returns.
    if (!bus.valid(wk::battery)) {
        pi_.reset();
        last_ms_ = 0;
        return;                          // publish nothing -> the output failsafes
    }
    const float vbat   = bus.get(wk::battery, 0.0f);
    const float target = cfg_->target_voltage * 0.01f;   // scaled volts
    const float error  = target - vbat;                  // low battery -> more field
    // OVER-VOLTAGE BACKSTOP. Well above target (1.5 V: a stuck field, a failed regulator loop) the field
    // is cut outright rather than trimmed, and the integrator is dropped so it cannot hold it on.
    if (vbat > target + 1.5f) {
        pi_.reset();
        last_ms_ = now;
        bus.set(wk::alternator_duty, 0.0f, true, now, ttl());
        return;
    }

    const float dt   = last_ms_ ? (now - last_ms_) / 1000.0f : 0.0f;
    last_ms_ = now;
    const float duty = (dt > 0.0f) ? pi_.step(error, dt) : std::clamp(pi_.integ, 0.0f, ceil);

    bus.set(wk::alternator_duty, std::clamp(duty, 0.0f, ceil), true, now, ttl());
}
