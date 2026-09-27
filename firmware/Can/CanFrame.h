#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// CanFrame — a single classic CAN frame (not FD).
// Supports both standard (11-bit) and extended (29-bit) IDs.
// ---------------------------------------------------------------------------

struct CanFrame {
    uint32_t id;       // 11-bit or 29-bit depending on ext
    bool     ext;      // true = extended 29-bit ID
    bool     rtr;      // remote transmit request
    uint8_t  dlc;      // data length code 0-8
    uint8_t  data[8];
};
