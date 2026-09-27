#pragma once

#include "CanFrame.h"

// ---------------------------------------------------------------------------
// ICanChannel — one physical CAN bus interface.
// Implemented by the platform driver (bxCAN, FDCAN in CAN-FD fallback mode).
// Stubbed for native test builds.
// ---------------------------------------------------------------------------

// WHAT THE CONTROLLER KNOWS ABOUT THE WIRE. A CAN bus does not fail by going quiet — it fails by
// counting errors, and the controller keeps that count itself. Reading it turns "nothing is working"
// into a specific answer: an ACK error means no other node is out there, a bit error means something
// is driving against us, a form or stuff error usually means the bit rate disagrees.
//
// There is no such thing as a collision on CAN. Arbitration is non-destructive — the lower id simply
// wins and the loser retries — so a busy bus costs latency, not corruption. These counters, not a
// collision count, are what says a bus is unhealthy.
struct CanErrorStatus {
    uint8_t  tec       = 0;   // transmit error counter (>127 = error passive, 255 = bus-off)
    uint8_t  rec       = 0;   // receive error counter
    uint8_t  state     = 0;   // 0 active, 1 warning, 2 passive, 3 bus-off
    uint8_t  last_err  = 0;   // 0 none, 1 stuff, 2 form, 3 ACK, 4 bit-recessive, 5 bit-dominant, 6 CRC
    uint32_t bus_off_n = 0;   // times this bus has entered bus-off since boot
};

class ICanChannel {
public:
    virtual ~ICanChannel() = default;

    // Send a frame.  Returns false if TX FIFO is full.
    virtual bool send(const CanFrame& frame) = 0;

    // Non-blocking receive.  Returns true and fills frame if one is waiting.
    virtual bool receive(CanFrame& frame) = 0;

    // True if the hardware is configured and the bus is not in error-passive/busoff.
    virtual bool is_up() const = 0;

    // Error counters and the last error seen. Not pure: a stub bus has nothing to report, and a
    // driver that cannot read them should say "no errors" rather than force every implementer to
    // invent an answer.
    virtual CanErrorStatus error_status() const { return {}; }
};
