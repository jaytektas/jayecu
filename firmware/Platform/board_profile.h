#pragma once
#include "Scheduler/BoardProfile.h"

// ---------------------------------------------------------------------------
// The board composition seam — what the composition root (main) asks of WHATEVER
// board is compiled in. Every declaration here is board-NEUTRAL: exactly one board
// implements them, selected by CMake's BOARD cache variable, which picks
// Platform/boards/<board>/<board>_profile.cpp.
//
// This header exists because main used to name jaytek_v1 directly (eight call
// sites plus an include). That made the composition root a jaytek file: a second
// board could not be built without editing main, and "which board" stopped being a
// build-system decision. The BoardProfile abstraction was already correct — only
// the SYMBOL NAMES were board-specific.
//
// Nothing here says how a board provides any of this. A board may back its output
// pool with GPIO compare channels, an SPI smart driver, or anything else, as long
// as it honours the interface contracts in Scheduler/ITimerChannel.h — in
// particular that a firing sink's force_output_now() is ISR-safe.
// See docs/modular-platform-architecture.md, "Layer -1 — the SoC seam".
// ---------------------------------------------------------------------------

// The board's hardware resource inventory: the capture pool (trigger-capable
// inputs), the compare pool (firing outputs), and the shared timebase everything
// timestamps against. The assignment resolver binds the tune's logical roles onto
// this — nothing above the board layer names a pin.
const BoardProfile& board_profile() noexcept;

// Pin-less alarm used purely as a "call me back at tick T" source — the DCO for the
// VirtualTrigger angle clock. Drives no GPIO.
class IAlarmTimer;
IAlarmTimer& board_dco_channel() noexcept;

// The output pin pool for the PinArbiter: one sink per output row, in row order.
// board_build_output_pool() fills it once at boot; bind via PinArbiter::bind.
class ITimerChannel;
void            board_build_output_pool() noexcept;
ITimerChannel** board_output_pool(uint8_t& n) noexcept;

// The board's H-bridges as IHBridge objects. A board with fewer than two returns
// null for the ones it does not have.
class IHBridge;
void board_hbridges(IHBridge*& a, IHBridge*& b) noexcept;
