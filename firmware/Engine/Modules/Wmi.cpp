#include "Wmi.h"
#include "well_known_signals.h"
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../Platform/platform_hal.h"
#include "../../../generated/signal_ids.h"
#include <algorithm>

void Wmi::update(const EnginePosition& /*pos*/, SignalBus& bus, EngineFrame& /*frame*/) {
    const uint32_t now = platform_get_tick_ms();
    if (!cfg_ || !cfg_->enabled) {
        duty_ = 0.0f;
        bus.set_bool(SIG_WMI_ACTIVE, false, now, ttl());
        bus.set(SIG_WMI_DUTY, 0.0f, true, now, ttl());
        return;
    }
    const float map = bus.get(wk::map, 0.0f);
    const float tps = bus.get(wk::tps, 0.0f);
    const float min_map  = static_cast<float>(cfg_->min_map_kpa) * 0.1f;
    const float full_map = static_cast<float>(cfg_->full_map_kpa) * 0.1f;
    const float min_duty = static_cast<float>(cfg_->min_duty_pct) * 0.1f;

    const bool active = (map >= min_map) && (tps > static_cast<float>(cfg_->min_tps) * 0.1f);
    float duty = 0.0f;
    if (active) {
        const float span = (full_map > min_map) ? (full_map - min_map) : 1.0f;
        const float frac = std::clamp((map - min_map) / span, 0.0f, 1.0f);   // 0 at onset .. 1 at full
        duty = min_duty + frac * (100.0f - min_duty);
        duty = std::clamp(duty, 0.0f, 100.0f);
    }
    duty_ = duty;
    bus.set_bool(SIG_WMI_ACTIVE, active, now, ttl());
    bus.set(SIG_WMI_DUTY, duty, true, now, ttl());
}
