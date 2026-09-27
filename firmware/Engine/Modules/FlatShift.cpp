#include "FlatShift.h"
#include "well_known_signals.h"
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../Platform/platform_hal.h"      // platform_get_tick_ms
#include "../../../generated/signal_ids.h"

void FlatShift::update(const EnginePosition& pos, SignalBus& bus, EngineFrame& /*frame*/) {
    const uint32_t now   = platform_get_tick_ms();
    const float    dt_ms = (last_ms_ != 0) ? static_cast<float>(now - last_ms_) : 0.0f;
    last_ms_ = now;

    if (!cfg_ || !cfg_->enabled) {
        active_ = false; held_ms_ = 0.0f;
        bus.set_bool(SIG_SHIFT_CUT_ACTIVE, false, now, ttl());
        return;
    }

    const bool trig = bus.get_bool(static_cast<SignalId>(cfg_->trigger_sig));
    const bool cond = (pos.rpm > static_cast<float>(cfg_->min_rpm))
                   && (bus.get(wk::tps, 0.0f) > static_cast<float>(cfg_->min_tps) * 0.1f);

    if (!trig) {
        active_ = false; held_ms_ = 0.0f;                          // trigger released -> re-arm
    } else if (cond && held_ms_ < static_cast<float>(cfg_->max_cut_ms)) {
        active_ = true; held_ms_ += dt_ms;                         // cutting (up to the max time)
    } else {
        active_ = false;                                           // conditions not met, or max time hit
    }

    // validity-OR: publish the cut true only WHILE flat-shifting; it expires (releases) when we stop.
    if (active_) {
        // 0 = Fuel, 1 = Ignition, 2 = Both — the one order every module that cuts now uses. This module
        // read it the other way round, so the same number meant a different cut here than on the rev
        // limiter; the schema default moved to 1 so a flat shift still cuts ignition.
        if (cfg_->cut_method == 0 || cfg_->cut_method == 2) bus.set_bool(wk::fuel_cut, true, now, ttl());
        if (cfg_->cut_method == 1 || cfg_->cut_method == 2) bus.set_bool(wk::ign_cut,  true, now, ttl());
    }
    bus.set_bool(SIG_SHIFT_CUT_ACTIVE, active_, now, ttl());
}
