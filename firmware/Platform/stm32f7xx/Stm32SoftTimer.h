#pragma once
#include "Scheduler/ITimerChannel.h"   // IAlarmTimer (MCU-agnostic)
#include "stm32f7xx_hal.h"

// ---------------------------------------------------------------------------
// Stm32SoftTimer — an IAlarmTimer backed by TIM4, the dedicated tick source for
// the software PWM engine (SoftPwm). It sits at NVIC priority 3: BELOW the trigger
// decoder (EXTI, prio 1) and the firing scheduler (TIM5, prio 2), so a tach edge
// never delays crank capture or a coil/injector event — but ABOVE the FreeRTOS
// syscall ceiling (5), so OS critical sections can't jitter it. Like the TIM5
// firing layer it is OS-decoupled: NOTHING on this ISR path may call a FreeRTOS API.
//
// TIM4 is 16-bit. A software high-word, bumped in the update (overflow) ISR,
// extends it to a clean 32-bit monotonic timebase so SoftPwm's absolute-tick
// ordered list stays correct (no 16-bit wrap to reason about). One-shot: arm()
// programs CCR1, on match the callback re-arms for the next list head.
// ---------------------------------------------------------------------------
class Stm32SoftTimer final : public IAlarmTimer {
public:
    Stm32SoftTimer() noexcept;

    // Start TIM4 free-running at `ticks_per_second`, enable the overflow + compare
    // interrupts, and set NVIC priority 3. Call once at init.
    void init(uint32_t ticks_per_second) noexcept;

    // ---- IAlarmTimer ----
    void arm(uint32_t abs_ticks) noexcept override;
    void disarm() noexcept override;
    [[nodiscard]] uint32_t now() const noexcept override;
    void register_callback(MatchCallback cb, void* user_data) noexcept override {
        cb_ = cb; cb_data_ = user_data;
    }

    // ---- called from TIM4_IRQHandler ----
    void on_update() noexcept  { high_ += 0x10000u; }   // counter overflow: extend timebase
    void on_compare() noexcept;                          // CCR1 match: fire if due

private:
    // 32-bit tick from the software high-word + the 16-bit counter, accounting for
    // an overflow that is pending-but-not-yet-serviced (caller masks IRQs).
    [[nodiscard]] uint32_t snapshot_() const noexcept {
        uint32_t hi = high_, cnt = TIM4->CNT;
        if (TIM4->SR & TIM_SR_UIF) { hi += 0x10000u; cnt = TIM4->CNT; }
        return hi | cnt;
    }

    MatchCallback     cb_       = nullptr;
    void*             cb_data_  = nullptr;
    volatile uint32_t high_     = 0;      // software high 16 bits (<<16), bumped on overflow
    volatile uint32_t deadline_ = 0;
    volatile bool     armed_    = false;
};
