#include "AntiLag.h"
#include "well_known_signals.h"
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../Platform/platform_hal.h"
#include "../../../generated/signal_ids.h"

void AntiLag::update(const EnginePosition& pos, SignalBus& bus, EngineFrame& /*frame*/) {
    const uint32_t now   = platform_get_tick_ms();
    const float    dt_ms = (last_ms_ != 0) ? static_cast<float>(now - last_ms_) : 0.0f;
    last_ms_ = now;

    if (!cfg_ || !cfg_->enabled) {
        active_ = false; held_ms_ = 0.0f;
        bus.set_bool(SIG_ANTILAG_ACTIVE, false, now, ttl());
        bus.set(SIG_ANTILAG_RETARD, 0.0f, true, now, ttl());
        return;
    }

    const bool armed = (cfg_->arm_sig < 0) ? true
                     : bus.get_bool(static_cast<SignalId>(cfg_->arm_sig));
    const bool off_throttle = bus.get(wk::tps, 100.0f) < static_cast<float>(cfg_->max_tps) * 0.1f;
    const bool cond = armed && off_throttle && (pos.rpm > static_cast<float>(cfg_->min_rpm));

    if (!cond) {
        active_ = false; held_ms_ = 0.0f;                          // throttle opened / disarmed -> re-arm
    } else if (held_ms_ < static_cast<float>(cfg_->max_time_ms)) {
        active_ = true; held_ms_ += dt_ms;                         // holding ALS (up to the time limit)
    } else {
        active_ = false;                                           // max time hit -> stop (turbo protect)
    }

    bus.set_bool(SIG_ANTILAG_ACTIVE, active_, now, ttl());
    bus.set(SIG_ANTILAG_RETARD, active_ ? static_cast<float>(cfg_->retard_deg) * 0.1f : 0.0f, true, now, ttl());
}
