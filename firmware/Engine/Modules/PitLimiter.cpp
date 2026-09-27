#include "PitLimiter.h"
#include "well_known_signals.h"
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../Platform/platform_hal.h"      // platform_get_tick_ms
#include "../../../generated/signal_ids.h"

void PitLimiter::update(const EnginePosition& /*pos*/, SignalBus& bus, EngineFrame& /*frame*/) {
    const uint32_t now = platform_get_tick_ms();
    if (!cfg_ || !cfg_->enabled) {
        active_ = false;
        bus.set_bool(SIG_PIT_LIMIT_ACTIVE, false, now, ttl());
        return;
    }

    const bool armed = (cfg_->arm_sig < 0) ? true
                     : bus.get_bool(static_cast<SignalId>(cfg_->arm_sig));   // unassigned (-1) = always engaged
    const float vss   = bus.get(wk::vehicle_spd, 0.0f);
    const float limit = static_cast<float>(cfg_->speed_kph);

    if (!armed)                                          active_ = false;
    else if (vss > limit)                               active_ = true;
    else if (vss < limit - static_cast<float>(cfg_->hyst_kph)) active_ = false;
    // else hold (hysteresis band)

    // validity-OR: publish the cut true only WHILE limiting; it expires (releases) when we stop.
    if (active_) {
        if (cfg_->cut_method == 0 || cfg_->cut_method == 2) bus.set_bool(wk::fuel_cut, true, now, ttl());
        if (cfg_->cut_method == 1 || cfg_->cut_method == 2) bus.set_bool(wk::ign_cut,  true, now, ttl());
    }
    bus.set_bool(SIG_PIT_LIMIT_ACTIVE, active_, now, ttl());
}
