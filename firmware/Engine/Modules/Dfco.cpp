#include "Dfco.h"
#include "well_known_signals.h"
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../../generated/signal_ids.h"
#include "../../Platform/platform_hal.h"   // platform_get_tick_ms

void Dfco::update(const EnginePosition& pos, SignalBus& bus, EngineFrame& /*frame*/) {
    const uint32_t now = platform_get_tick_ms();
    if (!cfg_ || !cfg_->enabled) {
        active_ = false;
        bus.set_bool(SIG_DFCO_ACTIVE, false, now, ttl());
        return;   // do NOT touch wk::fuel_cut — other modules own it when we're disabled
    }

    const float rpm = pos.rpm;
    const float tps = bus.get(wk::tps, 100.0f);    // absent -> assume open throttle (no cut)
    const float clt = bus.get(wk::clt, -100.0f);   // absent -> assume cold (no cut)

    // Overrun preconditions: closed throttle AND a warm engine (never cut cold / at part throttle).
    const bool allowed = (tps < static_cast<float>(cfg_->max_tps) * 0.1f)
                      && (clt > static_cast<float>(cfg_->min_clt));

    // RPM hysteresis: enter the cut above rpm_high, leave it below rpm_low; hold in between.
    if (allowed && rpm > static_cast<float>(cfg_->rpm_high))      active_ = true;
    else if (!allowed || rpm < static_cast<float>(cfg_->rpm_low)) active_ = false;
    // else: hold active_ (the hysteresis band)

    if (active_) bus.set_bool(wk::fuel_cut, true, now, ttl());   // validity-OR: publish only while cutting
    bus.set_bool(SIG_DFCO_ACTIVE, active_, now, ttl());
}
