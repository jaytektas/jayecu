#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// telem_round<T> — pack a scaled float into a fixed-point telemetry integer by
// rounding to NEAREST, not truncating toward zero.
//
// Telemetry fields are fixed-point (e.g. map_kpa_x10 = kPa × 10). A plain
// static_cast<int>(value) floors, and float scaling rarely lands exactly on an
// integer: 5000 Hz / 5000 × 100 × 10 = 999.99998f, which floors to 999 instead
// of 1000 — every reading sits one LSB low. Round-half-away-from-zero fixes it
// for both signs without pulling in <cmath>/lroundf on the MCU.
// ---------------------------------------------------------------------------
template <typename T>
static inline T telem_round(float v) {
    return static_cast<T>(v + (v >= 0.0f ? 0.5f : -0.5f));
}
