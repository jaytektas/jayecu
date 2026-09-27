#pragma once
#include "ITimerChannel.h"

// ---------------------------------------------------------------------------
// BoardProfile — describes the complete set of timer resources available on
// one hardware variant, without naming any MCU-specific peripheral.
//
// Constructed statically by the platform layer (firmware/platform/).
// Never included by the EFI subsystem (firmware/src/efi_pos/) directly —
// only the platform layer uses it to build an EcuHardwareAssignment.
// ---------------------------------------------------------------------------

// Bitmask of hardware capabilities for one timer pin.
enum class PinCapabilityFlags : uint8_t {
    TIMER_CAPTURE = 0x01, // Supports input capture (ICaptureChannel)
    TIMER_COMPARE = 0x02, // Supports output compare (ITimerChannel)
    EXTI_CAPABLE  = 0x04, // External interrupt only (limited ICaptureChannel)
    CAPT_AND_COMP = 0x03  // Both capture and compare (advanced timer channels)
};

// One input-capture resource available on this board.
// label: human-readable identifier for diagnostics only (e.g. "TIM2_CH1").
//        Not used in any control logic.
struct CaptureResource {
    ICaptureChannel*   channel;
    PinCapabilityFlags capability;
    const char*        label;
};

// One output-compare resource available on this board.
struct CompareResource {
    ITimerChannel*     channel;
    PinCapabilityFlags capability;
    const char*        label;
};

// Complete hardware resource inventory for one board variant.
// All pointers must remain valid for the lifetime of the firmware.
// capture_resources and compare_resources point to statically-allocated arrays.
struct BoardProfile {
    const CaptureResource* capture_resources; // Array of available ICaptureChannel wrappers
    uint8_t                capture_count;
    const CompareResource* compare_resources; // Array of available ITimerChannel wrappers
    uint8_t                compare_count;
    // Output-compare channels are fixed-function (unlike the tune-assignable
    // capture pool): the first `ign_compare_count` entries of compare_resources
    // are the ignition outputs (→ assignment.ign[]), the remainder are low-side
    // outputs (→ assignment.ls[]). The resolver splits the pool on this index.
    uint8_t                ign_compare_count;
    // Shared free-running timebase. ALL compare and capture channels must derive
    // their timestamps from the same underlying counter as this timebase.
    // Used as the authoritative clock for absolute tick arithmetic.
    const ITimerChannel*   timebase;
    uint32_t               ticks_per_second;  // e.g. 216000000 for STM32F767 at 216 MHz
};
