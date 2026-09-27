#pragma once
#include "Scheduler/ITimerChannel.h"   // ICaptureChannel / ITimerChannel (MCU-agnostic)
#include "stm32f7xx_hal.h"

// ---------------------------------------------------------------------------
// STM32F7 input-capture mechanism for the trigger / frequency / quadrature
// capture layer.
//
// Stm32CaptureChannel — an ICaptureChannel backed by EXTI on ANY GPIO pin,
//   timestamped against a shared free-running 32-bit timer (TIM5). Pins are
//   bound to logical roles (crank / cam / frequency / ...) at runtime by the
//   assignment resolver — this object is only the reusable per-pin mechanism,
//   it owns no role. The EXTI line index equals the pin number, so each board
//   pin lands on a distinct line (jaytek_v1's capture pins were deliberately
//   laid out on separate EXTI lines for exactly this).
//
// Stm32Timebase — the same free-running TIM5 exposed as the ITimerChannel
//   "timebase": the authoritative clock that every capture timestamp (and,
//   later, every output-compare event) derives from.
//
// Nothing here names a board pin — the board's BoardProfile instantiates one
// channel per physical capture-capable pin and hands the pool upward.
// ---------------------------------------------------------------------------

class Stm32CaptureChannel final : public ICaptureChannel {
public:
    // Identify the physical input. Registers itself for EXTI dispatch on
    // construction (line == pin). Touches no hardware — call configure() and
    // Stm32Capture_Init() before edges can arrive.
    Stm32CaptureChannel(GPIO_TypeDef* port, uint8_t pin) noexcept;

    // ---- ICaptureChannel ----
    [[nodiscard]] uint32_t    get_last_capture_ticks() const noexcept override { return last_ticks_; }
    [[nodiscard]] CaptureEdge get_edge_polarity()      const noexcept override { return last_edge_; }
    [[nodiscard]] bool        get_current_level()      const noexcept override;
    void register_callback(CaptureCallback cb, void* user_data) noexcept override;
    void set_capture_enabled(bool enabled) noexcept override;

    // ---- Dynamic binding (task context; used by the reconfigure path) ----
    // (Re)configure the EXTI trigger edge and leave the line masked until
    // set_capture_enabled(true). Safe to call repeatedly to rebind a pin's edge.
    void configure(CaptureEdge edge) noexcept;

    // ---- EXTI dispatch entry (ISR context) ----
    void on_edge(uint32_t ticks) noexcept;

    [[nodiscard]] uint8_t line() const noexcept { return pin_; }

private:
    GPIO_TypeDef*        port_;
    uint8_t              pin_;        // also the EXTI line index
    CaptureEdge          edge_;
    volatile uint32_t    last_ticks_;
    volatile CaptureEdge last_edge_;
    CaptureCallback      cb_;
    void*                cb_data_;
};

class Stm32Timebase final : public ITimerChannel {
public:
    explicit Stm32Timebase(uint32_t ticks_per_second) noexcept : tps_(ticks_per_second) {}

    [[nodiscard]] uint32_t get_current_ticks()    const noexcept override { return TIM5->CNT; }
    [[nodiscard]] uint32_t get_ticks_per_second() const noexcept override { return tps_; }

    // Pure timebase: never drives an output pin. The scheduler only reads the
    // counter accessors on the timebase, so force_output_now is a no-op.
    void force_output_now(OutputAction)                noexcept override {}

private:
    uint32_t tps_;
};

// One-time hardware bring-up: start TIM5 free-running at `ticks_per_second` and
// enable the EXTI NVIC line groups. Call once from platform_init() after the
// GPIO clocks are up. Individual channels still need configure() +
// set_capture_enabled(true) (driven by the assignment / decoder start path).
extern "C" void Stm32Capture_Init(uint32_t ticks_per_second);

// Route an EXTI line straight to a frequency counter / SENT decoder (bypassing the
// capture channel) so the priority-1 ISR does minimal work on the edge. The line
// must already be EXTI-configured + unmasked (see the board's platform_*_enable).
extern "C" void Stm32Capture_BindFreqLine(uint8_t line, uint8_t freq_idx);
extern "C" void Stm32Capture_BindSentLine(uint8_t line, uint8_t sent_idx);
extern "C" void Stm32Capture_BindPulseLine(uint8_t line, uint8_t pulse_idx);
