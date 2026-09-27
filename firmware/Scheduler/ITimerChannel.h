#pragma once
#include "SchedulerTypes.h"

// ---------------------------------------------------------------------------
// Hardware Timer Abstraction Layer
//
// ITimerChannel  — output-compare channel (ignition / injection / ADC trigger)
// ICaptureChannel — input-capture channel (crank VR, cam Hall/VR)
//
// Concrete implementations live entirely in firmware/platform/.
// This header must never include any MCU-specific header.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// ITimerChannel
// ---------------------------------------------------------------------------

// Called from the timer ISR when an output-compare match fires.
// Must be ISR-safe: no heap, no blocking, no floating point.
using MatchCallback = void (*)(void* user_data);

// ITimerChannel is now an OUTPUT-only channel: a GPIO sink that can also report
// the shared timebase. Firing timing is owned by the scheduler + IAlarmTimer
// (firing-layer rework) — channels no longer hold per-pin deadlines, so the old
// schedule_match_ticks / disable_match / register_match_callback / configure_
// output_action have been removed (the O(N) oc_reprogram scan went with them).
class ITimerChannel {
public:
    virtual ~ITimerChannel() = default;

    // Returns the current free-running timer counter.
    // Callable from any context; single-cycle LDR on aligned uint32_t is atomic
    // on Cortex-M7 — no barrier needed for a single read.
    [[nodiscard]] virtual uint32_t get_current_ticks()    const noexcept = 0;

    // Returns the timer clock frequency in Hz (e.g. 216000000 for STM32F767).
    // Constant for the lifetime of the object.
    [[nodiscard]] virtual uint32_t get_ticks_per_second() const noexcept = 0;

    // Immediately apply `action` to the GPIO pin without scheduling a timer event.
    // Used by the chained ISR to toggle the pin at the exact moment the match fires
    // (dwell start → DRIVE_HIGH, spark → DRIVE_LOW, injector open/close).
    // Must be ISR-safe: single GPIO register write, no blocking, no heap.
    virtual void force_output_now(OutputAction action) noexcept = 0;

    // Configure the pin as a driven push-pull output, preset to `idle_level`
    // (glitch-free: ODR set before the mode switch). Called by the scheduler ONLY
    // when the assignment router binds this channel to a coil/injector role — so a
    // pin that is never assigned stays in its reset Hi-Z state rather than actively
    // driving LOW. Task-context (does a GPIO init), not ISR-safe. Default no-op for
    // non-GPIO channels (e.g. the timebase).
    virtual void enable_output(OutputAction idle_level) noexcept { (void)idle_level; }

    // Return the pin to Hi-Z (reset/input state) — the release counterpart of
    // enable_output(). Called by the arbiter when ownership ends, so a released pin
    // stops driving. (Board pulls hold it at the safe level while Hi-Z.) Default
    // no-op for non-GPIO channels.
    virtual void disable_output() noexcept {}
};

// ---------------------------------------------------------------------------
// IAlarmTimer — a pin-less "call me back at an absolute tick X" comparator.
//
// The firing-layer rework (docs/firing-layer-two-timer-plan.md) replaces the
// per-pin deadline model + O(N) soonest-scan with scheduler-owned ordered lists
// driven by two domain alarms: Timer 1 (angle events) and Timer 2 (time events).
// An IAlarmTimer holds ONE pending deadline; the scheduler re-arms it for the
// next list head on each callback. No GPIO — outputs are driven separately via
// IGpioOutput sinks. Backed by one TIM5 CCRn.
// ---------------------------------------------------------------------------
class IAlarmTimer {
public:
    virtual ~IAlarmTimer() = default;

    // Program a single one-shot callback at absolute free-running tick `abs_ticks`.
    // If already in the past, fires immediately (no 32-bit-wrap wait). ISR-safe.
    virtual void arm(uint32_t abs_ticks) noexcept = 0;

    // Cancel the pending callback. Safe if not armed.
    virtual void disarm() noexcept = 0;

    // Current free-running timer count (same timebase the scheduler interpolates in).
    [[nodiscard]] virtual uint32_t now() const noexcept = 0;

    // Register the match callback (replaces any prior). cb must be non-null.
    virtual void register_callback(MatchCallback cb, void* user_data) noexcept = 0;
};

// ---------------------------------------------------------------------------
// ICaptureChannel
// ---------------------------------------------------------------------------

// Called from the capture ISR when an edge is detected.
// timestamp_ticks: free-running timer count at the capture edge.
// Must be ISR-safe: no heap, no blocking, no floating point.
using CaptureCallback = void (*)(uint32_t timestamp_ticks, void* user_data);

class ICaptureChannel {
public:
    virtual ~ICaptureChannel() = default;

    // Returns the timer count captured at the most recent edge.
    // On Cortex-M7, reading an aligned uint32_t is a single-cycle LDR — atomic.
    // Safe to read from task context without disabling IRQ, with the understanding
    // that the value may be updated by an ISR between reads.
    [[nodiscard]] virtual uint32_t    get_last_capture_ticks() const noexcept = 0;

    // Returns the polarity of the most recent captured edge.
    [[nodiscard]] virtual CaptureEdge get_edge_polarity()      const noexcept = 0;

    // Returns the current logical level of the input pin (High/Low).
    [[nodiscard]] virtual bool        get_current_level()      const noexcept = 0;

    // Register the ISR callback. Replaces any prior registration atomically.
    // cb must be non-null. Called once before set_capture_enabled(true).
    virtual void register_callback(CaptureCallback cb,
                                    void* user_data) noexcept = 0;

    // Enable or disable the capture interrupt.
    // The concrete implementation must disable the interrupt atomically
    // (e.g. via NVIC_DisableIRQ) rather than masking all interrupts.
    virtual void set_capture_enabled(bool enabled) noexcept = 0;
};

// ---------------------------------------------------------------------------
// IGpioOutput — simple binary output (Status LEDs, ETB DIS/DIR)
// ---------------------------------------------------------------------------
class IGpioOutput {
public:
    virtual ~IGpioOutput() = default;
    virtual void write(bool high) noexcept = 0;
    virtual void toggle() noexcept = 0;
};

// ---------------------------------------------------------------------------
// IGpioInput — simple binary input (Switches, logic signals)
// ---------------------------------------------------------------------------
class IGpioInput {
public:
    virtual ~IGpioInput() = default;
    [[nodiscard]] virtual bool read() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// ICanBus — CAN communication interface
// ---------------------------------------------------------------------------
struct CanMessage {
    uint32_t id;
    uint8_t  len;
    uint8_t  data[8];
    bool     is_extended;
};

class ICanBus {
public:
    virtual ~ICanBus() = default;
    virtual bool send(const CanMessage& msg) noexcept = 0;
    virtual bool receive(CanMessage& msg) noexcept = 0;
};
