#include "GearDetect.h"
#include "well_known_signals.h"
#include "../../Signal/EnginePosition.h"
#include "../../Signal/SignalBus.h"
#include "../EngineFrame.h"
#include "../../../generated/signal_ids.h"
#include "../../Platform/platform_hal.h"   // platform_get_tick_ms (publish TTL timestamp)
#include <cmath>

void GearDetect::update(const EnginePosition& pos, SignalBus& bus, EngineFrame& /*frame*/) {
    if (!cfg_ || !cfg_->enabled) return;   // no producer -> don't stomp a gear set elsewhere (Lua/CAN)

    const uint32_t now = platform_get_tick_ms();   // real tick for every fresh publish (ttl())
    const float rpm = pos.rpm;
    const float vss = bus.get(wk::vehicle_spd, 0.0f);

    // Below the RPM/speed floor the ratio is meaningless (coasting / stopped) -> unknown.
    if (rpm < static_cast<float>(cfg_->min_rpm) || vss < static_cast<float>(cfg_->min_vss)) {
        gear_ = 0;
        bus.set(SIG_GEAR, 0.0f, true, now, ttl());
        return;
    }

    const float measured = rpm / vss;                              // engine RPM per kph
    const float tol      = static_cast<float>(cfg_->tol_pct) * 0.01f;
    const uint8_t n = (cfg_->gear_count <= GEAR_DETECT_GEAR_RATIO_COUNT)
                    ? cfg_->gear_count : GEAR_DETECT_GEAR_RATIO_COUNT;

    uint8_t best     = 0;
    float   best_err = tol;                                        // must be within tolerance to count
    for (uint8_t i = 0; i < n; ++i) {
        const float expected = static_cast<float>(cfg_->gear_ratio[i].rpm_per_kph) * 0.1f;
        if (expected <= 0.0f) continue;                            // unused gear
        const float err = std::fabs(measured - expected) / expected;
        if (err < best_err) { best_err = err; best = static_cast<uint8_t>(i + 1); }   // 1-indexed
    }

    gear_ = best;
    bus.set(SIG_GEAR, static_cast<float>(best), true, now, ttl());
}
