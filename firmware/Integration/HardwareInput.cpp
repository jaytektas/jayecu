#include "HardwareInput.h"
#include "../Platform/platform_hal.h"        // platform_read_ain_raw / platform_get_tick_ms
#include "../../generated/signal_ids.h"       // SIG_HW_AV* / SIG_HW_AT* (via the generated publish body)

// Raw-input freshness: a few 1 kHz frames of grace, matching the INPUT-phase cadence — a slot decays
// via expire_stale() if HardwareInput ever stops refreshing it. Same discipline as module publishes.
static constexpr uint32_t HW_INPUT_TTL_MS = TTL_FRAME_MS;

void HardwareInput::update(const EnginePosition& /*pos*/, SignalBus& bus, EngineFrame& /*frame*/) {
    const uint32_t now_ms = platform_get_tick_ms();
    // Board-generated: one bus.set_u32(SIG_HW_*, platform_read_ain_raw(pool), ...) per analog input.
    #include "../../generated/hw_input_publish.inc"
}
