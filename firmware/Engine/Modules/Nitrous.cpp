#include "Nitrous.h"
#include "well_known_signals.h"
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../Platform/platform_hal.h"      // platform_get_tick_ms
#include "../../../generated/signal_ids.h"

void Nitrous::update(const EnginePosition& pos, SignalBus& bus, EngineFrame& /*frame*/) {
    const uint32_t now = platform_get_tick_ms();
    if (!cfg_ || !cfg_->enabled) {
        active_ = false;
        bus.set_bool(SIG_NITROUS_ACTIVE, false, now, ttl());
        bus.set(SIG_NITROUS_RETARD, 0.0f, true, now, ttl());
        return;
    }

    const bool armed = (cfg_->arm_sig < 0) ? true
                     : bus.get_bool(static_cast<SignalId>(cfg_->arm_sig));   // unassigned (-1) = always armed
    const float rpm = pos.rpm;
    const float tps = bus.get(wk::tps, 0.0f);

    bool pressure_ok = true;
    if (cfg_->pressure_src >= 0) {   // optional bottle-pressure safety
        const float p = bus.get(static_cast<SignalId>(cfg_->pressure_src), 0.0f);
        pressure_ok = p >= static_cast<float>(cfg_->min_pressure_kpa);   // whole kPa (a bottle, not a manifold)
    }

    active_ = armed
           && rpm >= static_cast<float>(cfg_->min_rpm)
           && rpm <= static_cast<float>(cfg_->max_rpm)
           && tps >  static_cast<float>(cfg_->min_tps) * 0.1f
           && pressure_ok;

    bus.set_bool(SIG_NITROUS_ACTIVE, active_, now, ttl());                        // solenoid enable
    bus.set(SIG_NITROUS_RETARD, active_ ? static_cast<float>(cfg_->retard_deg) * 0.1f : 0.0f, true, now, ttl());
}
