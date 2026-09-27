#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// EngineSyncSampler — crank-angle-window averaging for engine-sync analog sensors.
//
// Cylinder-pressure-pulsing sensors (MAP, locked engine-sync) alias badly against a fixed-time
// point-sample: the manifold pressure dips on every intake event, so a 1 kHz/per-cycle sample
// catches a random phase of that pulsation. Averaging across a fixed CRANK ANGLE (not a fixed
// time) removes it RPM-independently — the win the doc's "engine_sync_voltage" intent describes.
//
// Lives in the scheduler domain because that owns the crank angle. The grid ISR calls feed() once
// per virtual tooth (every ~10°) with the tooth's angular pitch; feed() reads each registered
// pin's DMA-filtered ADC (platform_read_ain_raw — ADC COUNTS, an ISR-safe cache read, no bus, no
// syscall), accumulates, and at the window boundary writes the average to a per-pin volatile shadow.
// The sensor pipeline's analog Acquire reads that shadow via engine_sync_read_raw(); a pin that is not
// registered (not engine-sync) falls straight through to a direct read, so this is transparent.
// ---------------------------------------------------------------------------

class EngineSyncSampler {
public:
    static constexpr uint8_t  MAX_PINS = 4;
    static constexpr uint16_t DEFAULT_WINDOW_DEG10 = 3600;   // 360° ≈ 2 intake events on a 4-cyl

    // Register the engine-sync source pins (+ optional window in decidegrees; 0 keeps current).
    // Called from a task at recompute_live. n_ is dropped to 0 first so a concurrent feed() on a
    // live engine does nothing mid-update (worst case: one stale window), then raised when ready.
    void configure(const uint8_t* pins, uint8_t n, uint16_t window_deg10 = 0) noexcept;

    // Grid ISR, per virtual tooth: one sample per registered pin; close the window after `step`.
    void feed(uint16_t step_deg10) noexcept;

    // Acquire (task): windowed ADC counts if the pin is registered + a window has closed, else direct.
    uint16_t read_raw(uint8_t pin) const noexcept;

private:
    volatile uint8_t  n_ = 0;                 // active pin count (ISR reads it; 0 = sampler idle)
    uint8_t  pins_[MAX_PINS] = {};
    uint16_t window_deg10_ = DEFAULT_WINDOW_DEG10;
    uint32_t sum_[MAX_PINS] = {};
    uint16_t cnt_[MAX_PINS] = {};
    uint32_t acc_deg10_ = 0;
    volatile uint16_t win_raw_[MAX_PINS]   = {};
    volatile bool     win_valid_[MAX_PINS] = {};
};

extern EngineSyncSampler g_engine_sync;

// Free wrapper the analog Acquire stage calls (keeps AcquireHal decoupled from the class).
uint16_t engine_sync_read_raw(uint8_t pin);
