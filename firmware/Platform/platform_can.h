#pragma once
//
// platform_can — the board-agnostic CAN entry points for the composition tier.
// main composes against ICanChannel only; the bxCAN peripheral details (CAN1/CAN2 register
// blocks, clock gating, GPIO pinning, bit timing, filters) live behind these in the platform
// driver (firmware/Platform/stm32f7xx/PlatformCan.cpp). No HAL headers leak upward.
//
#include <cstdint>

class ICanChannel;

// Bring up all CAN peripheral blocks (clock + GPIO + bit timing + accept-all filter + start) at the
// power-on defaults. The composition tier applies the tune's settings over the top with
// platform_can_apply — this only guarantees the buses exist.
void platform_can_init();

// Apply one bus's settings. VALUES, not config: main is the only thing that reads g_config, and the
// platform edge stays board-agnostic (see the note above). Idempotent, so the composition tier can call
// it on every config change without churning a bus that did not move.
//   enabled     false takes the bus down entirely — a bus wired to nothing should not sit on the wire
//   bitrate_hz  must match every other node; a mismatch never ACKs, which looks like nothing being there
//   listen_only receive without transmitting or ACKing, for tapping a bus we do not own
void platform_can_apply(uint8_t bus_index, bool enabled, uint32_t bitrate_hz, bool listen_only);

// Abstract reference to a CAN bus: 0 = CAN1, 1 = CAN2.
ICanChannel& platform_can_bus(uint8_t bus_index);

// Put a bus into (or out of) hardware LOOPBACK: every frame it transmits is also delivered to its own
// receive FIFO, and no external node has to ACK it. That is what makes the on-ECU CAN stack testable on
// a bench with nothing else on the wire — a lone CAN node cannot otherwise complete a transmission at
// all, so OBD request/response, ISO-TP segmentation and flow control are unreachable. Diagnostic only:
// while it is on, nothing the ECU sends reaches the outside world. Returns false if the bus is down.
bool platform_can_set_loopback(uint8_t bus_index, bool on);

// Re-init a bus at a different bitrate. Both buses come up at 500 kbit (OBD), but a CAN device on the
// wire may be configured for something else — and a bitrate mismatch is indistinguishable from an absent
// device (nothing ACKs either way), so being able to sweep is the only way to tell them apart.
bool platform_can_set_bitrate(uint8_t bus_index, uint32_t bitrate_hz);
