#include "Stm32SoftTimer.h"

// Single TIM4 instance, registered by the ctor and demuxed in the shared IRQ.
static Stm32SoftTimer* s_soft = nullptr;

// ---------------------------------------------------------------------------
// TIM4 ISR — overflow (timebase extension) + CC1 (the soft-PWM alarm). Service
// the overflow FIRST so any now() read inside the compare callback sees the
// already-extended high word. NVIC priority 3: preemptible by capture(1) and the
// firing scheduler(2); above the FreeRTOS syscall ceiling, so no OS calls here.
// ---------------------------------------------------------------------------
extern "C" void TIM4_IRQHandler(void) {
    const uint32_t sr = TIM4->SR;
    if (sr & TIM_SR_UIF)   { TIM4->SR = ~TIM_SR_UIF;   if (s_soft) s_soft->on_update(); }
    if (sr & TIM_SR_CC1IF) { TIM4->SR = ~TIM_SR_CC1IF; if (s_soft) s_soft->on_compare(); }
}

Stm32SoftTimer::Stm32SoftTimer() noexcept { s_soft = this; }

void Stm32SoftTimer::init(uint32_t ticks_per_second) noexcept {
    if (!ticks_per_second) ticks_per_second = 100000u;
    __HAL_RCC_TIM4_CLK_ENABLE();
    // APB1 timer clock = PCLK1 x2 (same tree as TIM5; see Stm32Capture_Init).
    const uint32_t timer_clk = 2u * HAL_RCC_GetPCLK1Freq();
    uint32_t psc = timer_clk / ticks_per_second;
    if (!psc) psc = 1u;
    TIM4->PSC  = psc - 1u;
    TIM4->ARR  = 0xFFFFu;                 // 16-bit free-running (software-extended to 32)
    TIM4->EGR  = TIM_EGR_UG;              // load PSC/ARR
    TIM4->SR   = 0u;                      // clear the UG-raised update flag
    TIM4->DIER = TIM_DIER_UIE;            // overflow interrupt -> timebase high-word
    TIM4->CR1  = TIM_CR1_CEN;             // start counting
    high_ = 0; armed_ = false;

    // Priority 3: below decoder(1) + scheduler(2), above the syscall ceiling(5).
    HAL_NVIC_SetPriority(TIM4_IRQn, 3, 0);
    HAL_NVIC_EnableIRQ(TIM4_IRQn);
}

void Stm32SoftTimer::arm(uint32_t abs_ticks) noexcept {
    const uint32_t pm = __get_PRIMASK(); __disable_irq();
    TIM4->CCR1  = abs_ticks & 0xFFFFu;    // 16-bit compare; high word is software-tracked
    deadline_   = abs_ticks;
    TIM4->SR    = ~TIM_SR_CC1IF;          // clear stale flag
    TIM4->DIER |= TIM_DIER_CC1IE;
    armed_      = true;
    // Already due/past: the 16-bit compare may not recur for up to a full wrap, so
    // pend the IRQ to service it now (signed-delta, wrap-safe).
    if ((int32_t)(abs_ticks - snapshot_()) <= 0) NVIC_SetPendingIRQ(TIM4_IRQn);
    __set_PRIMASK(pm);
}

void Stm32SoftTimer::disarm() noexcept {
    const uint32_t pm = __get_PRIMASK(); __disable_irq();
    TIM4->DIER &= ~TIM_DIER_CC1IE;
    armed_ = false;
    __set_PRIMASK(pm);
}

uint32_t Stm32SoftTimer::now() const noexcept {
    const uint32_t pm = __get_PRIMASK(); __disable_irq();
    const uint32_t t = snapshot_();
    __set_PRIMASK(pm);
    return t;
}

void Stm32SoftTimer::on_compare() noexcept {
    if (!armed_) return;
    // The CCR1 low-16 may match on an earlier wrap than the deadline; only fire when
    // the full 32-bit tick has actually reached it (delays < one wrap, so usually now).
    if ((int32_t)(snapshot_() - deadline_) >= 0) {
        TIM4->DIER &= ~TIM_DIER_CC1IE;    // one-shot: the callback re-arms the next head
        armed_ = false;
        if (cb_) cb_(cb_data_);
    }
}
