#pragma once
#include "Scheduler/ITimerChannel.h"   // ITimerChannel (MCU-agnostic)
#include "stm32f7xx_hal.h"

// ---------------------------------------------------------------------------
// STM32F7 output-compare mechanism for the ignition / injection / event
// scheduling layer.
//
// Stm32CompareChannel — an ITimerChannel backed by one GPIO output pin, fired
//   from a shared TIM5 output-compare (CCR1) match. It owns no engine role:
//   the assignment resolver binds it to a logical coil/injector at runtime.
//   force_output_now() is a single GPIO BSRR write; schedule_match_ticks()
//   enqueues an absolute deadline in the SAME free-running TIM5 counter the
//   capture layer timestamps against — so a predicted fire-tick needs no
//   timebase conversion between "when the tooth was seen" and "when the coil
//   fires".
//
// Multiplexing: every logical output holds at most ONE pending deadline at a
//   time (the scheduler's chained dwell->spark / open->close state machine),
//   so all channels share the single CCR1 compare, selected each time by a
//   "soonest armed deadline" scan over the registry. CH2..4 are left spare.
//
// The firing path is hard-real-time and OS-decoupled (no FreeRTOS calls):
//   TIM5 compare ISR -> channel callback -> GPIO edge + re-arm. It runs above
//   the FreeRTOS syscall ceiling, like the capture ISR.
//
// Nothing here names a board pin — the board's BoardProfile instantiates one
// channel per physical output pin and hands the pool upward.
// ---------------------------------------------------------------------------

// Stm32CompareChannel is now a dumb GPIO output sink (the firing-layer rework
// moved all timing to IAlarmTimer). force_output_now() is a single BSRR write; it
// holds no deadline and is never scanned. It self-registers only so
// Stm32OutputCompare_Init() can drive every pin LOW at boot.
class Stm32CompareChannel final : public ITimerChannel {
public:
    Stm32CompareChannel(GPIO_TypeDef* port, uint8_t pin) noexcept;

    // ---- ITimerChannel (output role) ----
    [[nodiscard]] uint32_t get_current_ticks()    const noexcept override { return TIM5->CNT; }
    [[nodiscard]] uint32_t get_ticks_per_second() const noexcept override;
    void force_output_now(OutputAction action) noexcept override;

    // Configure the pin as push-pull output preset to idle_level (called by the
    // scheduler when this channel is bound). Unbound channels are never configured
    // → they stay in reset Hi-Z, not driven.
    void enable_output(OutputAction idle_level) noexcept override;

    // Return the pin to Hi-Z (input, no pull) — released back to the pool.
    void disable_output() noexcept override;

private:
    GPIO_TypeDef* port_;
    uint32_t      pin_mask_;     // (1u << pin)
};

// ---------------------------------------------------------------------------
// Stm32Alarm — a pin-less "call me back at tick X" comparator on one TIM5 CCRn
// (n = 1..4). Holds ONE pending deadline; the owner re-arms it for the next head
// each callback. The firing-layer rework uses three: CCR1 = DCO (virtual-tooth
// clock), CCR2 = angle events, CCR3 = time events. Self-registers in the TIM5 ISR
// demux. See docs/firing-layer-two-timer-plan.md.
// ---------------------------------------------------------------------------
class Stm32Alarm final : public IAlarmTimer {
public:
    explicit Stm32Alarm(uint8_t ccr_channel) noexcept;   // 1..4

    void arm(uint32_t abs_ticks) noexcept override;
    void disarm() noexcept override;
    [[nodiscard]] uint32_t now() const noexcept override;
    void register_callback(MatchCallback cb, void* user_data) noexcept override {
        cb_ = cb; cb_data_ = user_data;
    }

    // Called from TIM5_IRQHandler when this channel's CCnIF is set OR is_due().
    void on_match() noexcept;
    [[nodiscard]] uint8_t channel() const noexcept { return ch_; }
    // Armed and its deadline has passed. The past-deadline arm pends the IRQ
    // WITHOUT setting CCnIF (the HW compare won't recur until a 32-bit wrap), so
    // the ISR must service it via this, not the flag alone (wrap-safe signed dt).
    [[nodiscard]] bool armed() const noexcept { return armed_; }
    [[nodiscard]] bool is_due(uint32_t now) const noexcept {
        return armed_ && (int32_t)(now - deadline_) >= 0;
    }

private:
    uint8_t           ch_;        // 1..4
    MatchCallback     cb_;
    void*             cb_data_;
    volatile uint32_t deadline_;
    volatile bool     armed_;
};

// Configure the TIM5 CCR1 compare interrupt + the output pins for the scheduler.
// TIM5's counter must already be running (call Stm32Capture_Init first). Sets
// the TIM5 IRQ above the FreeRTOS syscall ceiling — the fire path is OS-free.
extern "C" void Stm32OutputCompare_Init(uint32_t ticks_per_second);
