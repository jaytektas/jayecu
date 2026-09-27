#include "../platform_can.h"
#include "Stm32CanChannel.h"
#include "stm32f7xx_hal.h"   // CAN1 / CAN2 register blocks

// The bxCAN channels live here, at the platform edge — main never names CAN1/CAN2 or the
// bxCAN HAL. CAN1 = PD0/PD1, CAN2 = PB12/PB13 (AF9). Both default to 500 kbit (OBD-II); a CAN
// sensor on 1 Mbit needs that bus reconfigured — per-bus bitrate is a follow-on.
static Stm32CanChannel g_can0(CAN1);
static Stm32CanChannel g_can1(CAN2);

void platform_can_init() {
    g_can0.begin(500000);
    g_can1.begin(500000);
}

ICanChannel& platform_can_bus(uint8_t bus_index) {
    return (bus_index == 1) ? g_can1 : g_can0;
}

void platform_can_apply(uint8_t bus_index, bool enabled, uint32_t bitrate_hz, bool listen_only) {
    Stm32CanChannel& ch = (bus_index == 1) ? g_can1 : g_can0;
    if (!enabled) { ch.shutdown(); return; }
    ch.apply(bitrate_hz, listen_only);
}

bool platform_can_set_loopback(uint8_t bus_index, bool on) {
    return (bus_index == 1) ? g_can1.set_loopback(on) : g_can0.set_loopback(on);
}

bool platform_can_set_bitrate(uint8_t bus_index, uint32_t bitrate_hz) {
    return (bus_index == 1) ? g_can1.set_bitrate(bitrate_hz) : g_can0.set_bitrate(bitrate_hz);
}
