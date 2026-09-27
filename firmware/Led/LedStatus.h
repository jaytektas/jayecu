#pragma once

#include <cstdint>
#include "../Signal/EnginePosition.h"

// ---------------------------------------------------------------------------
// LedStatus — shared state written by EngineTask / CommsTask, read by LedTask.
//
// All fields are ≤32-bit and naturally aligned, so individual reads/writes are
// single-instruction atomic on Cortex-M7.  No mutex needed for this use case.
// ---------------------------------------------------------------------------

struct LedStatus {
    SyncLevel sync_level    = SyncLevel::NONE;
    float     rpm           = 0.0f;
    // Trigger teeth arrived in the last half second. rpm reads 0 without sync, so it cannot say the
    // engine is turning while the decoder is still looking — which is exactly when the RUN LED should
    // blink to say "turning, not synced".
    bool      turning       = false;
    bool      usb_connected = false;
    bool      usb_rx_pulse  = false;   // set briefly on each USB RX burst
    // DTC OBD-I flash-out: the LED CYCLES through every active code in its band —
    // orange = all active Level 1-2 codes, red = all active Level 3 codes (same
    // severity isn't rankable, so cycle them, don't pick one). Packed contiguous,
    // 0 = end. Each value's nibbles are the displayed digits (P0117 = 0x0117).
    static constexpr uint8_t DTC_LED_MAX = 6;
    uint16_t  dtc_warn_codes[DTC_LED_MAX]  = {};
    uint16_t  dtc_fault_codes[DTC_LED_MAX] = {};
};

extern LedStatus g_led_status;
