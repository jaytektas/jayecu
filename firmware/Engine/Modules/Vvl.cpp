#include "Vvl.h"
#include "well_known_signals.h"
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../Platform/platform_hal.h"
#include "../../../generated/signal_ids.h"

void Vvl::update(const EnginePosition& pos, SignalBus& bus, EngineFrame& /*frame*/) {
    const uint32_t now = platform_get_tick_ms();
    if (!cfg_ || !cfg_->enabled) {
        engaged_ = false;
        bus.set_bool(SIG_VVL_ACTIVE, false, now, ttl());
        return;
    }
    const float rpm  = pos.rpm;
    const float map  = bus.get(wk::map, 0.0f);
    const float clt  = bus.get(wk::clt, -100.0f);
    const bool  warm = clt >= static_cast<float>(cfg_->min_clt_c) * 0.1f;
    const bool  load_ok = map >= static_cast<float>(cfg_->min_load_kpa) * 0.1f;

    if (!engaged_) {
        // engage once above on_rpm with load + warm-up satisfied
        if (warm && load_ok && rpm >= static_cast<float>(cfg_->on_rpm)) engaged_ = true;
    } else {
        // hold until we drop below off_rpm, load falls away, or the engine goes cold
        if (!warm || !load_ok || rpm < static_cast<float>(cfg_->off_rpm)) engaged_ = false;
    }
    bus.set_bool(SIG_VVL_ACTIVE, engaged_, now, ttl());
}
