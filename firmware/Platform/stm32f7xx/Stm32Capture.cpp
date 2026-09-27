#include "Stm32Capture.h"

// ---------------------------------------------------------------------------
// EXTI line (0..15) -> owning capture channel. Lines map 1:1 to pin numbers,
// and jaytek_v1's capture pins occupy distinct lines, so there's never a
// collision. Written once during static construction (board pool), read in ISR.
// ---------------------------------------------------------------------------
static Stm32CaptureChannel* s_line_channel[16] = { nullptr };

// EXTI line -> frequency-counter index, or 0xFF for "not a frequency line". A
// frequency pin bypasses the capture-channel machinery entirely: the #1 IRQ does
// the BARE MINIMUM — hand the edge timestamp to the period recorder and move on.
// All the maths (period -> Hz, staleness) is deferred to the read side. Init to
// 0xFF at load (line 0 is a real counter index, so a zeroed table would misroute).
static uint8_t s_freq_line[16] = {
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
};

// EXTI line -> SENT decoder index, 0xFF = none. SENT (J2716) captures FALLING edges;
// like the frequency table, a SENT line bypasses the capture channel and just hands
// the timestamp to the decoder's ring (the decode runs later, at read time).
static uint8_t s_sent_line[16] = {
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
};

// EXTI line -> pulse/duty counter index, 0xFF = none. A pulse line captures BOTH
// edges; the ISR reads the pin level (via the line's capture channel — the one
// extra GPIO read that duty needs) to tag rising vs falling, then records the edge.
static uint8_t s_pulse_line[16] = {
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
};

// The minimal edge sinks (platform_hal_stm32.cpp): record the timestamp only — no
// division / decode — keeping the priority-1 ISR lean. Forward-declared to avoid
// pulling the platform header into the capture core.
extern "C" void platform_freq_on_edge(uint8_t pin, uint32_t now_us);
extern "C" void platform_sent_on_edge(uint8_t pin, uint32_t now_us);
extern "C" void platform_pulse_on_edge(uint8_t pin, uint32_t now_us, bool rising);

// Route a (already-EXTI-configured) line to a frequency counter / SENT decoder /
// pulse counter. Called from the board's platform_*_enable; task context.
extern "C" void Stm32Capture_BindFreqLine(uint8_t line, uint8_t freq_idx) {
    if (line < 16u) s_freq_line[line] = freq_idx;
}
extern "C" void Stm32Capture_BindSentLine(uint8_t line, uint8_t sent_idx) {
    if (line < 16u) s_sent_line[line] = sent_idx;
}
extern "C" void Stm32Capture_BindPulseLine(uint8_t line, uint8_t pulse_idx) {
    if (line < 16u) s_pulse_line[line] = pulse_idx;
}

Stm32CaptureChannel::Stm32CaptureChannel(GPIO_TypeDef* port, uint8_t pin) noexcept
    : port_(port), pin_(pin), edge_(CaptureEdge::RISING),
      last_ticks_(0), last_edge_(CaptureEdge::RISING),
      cb_(nullptr), cb_data_(nullptr)
{
    if (pin_ < 16u) s_line_channel[pin_] = this;
}

bool Stm32CaptureChannel::get_current_level() const noexcept {
    return (port_->IDR & (1u << pin_)) != 0u;
}

void Stm32CaptureChannel::register_callback(CaptureCallback cb, void* user_data) noexcept {
    cb_      = cb;
    cb_data_ = user_data;
}

void Stm32CaptureChannel::configure(CaptureEdge edge) noexcept {
    edge_ = edge;
    GPIO_InitTypeDef g = {};
    g.Pin   = (1u << pin_);
    g.Pull  = GPIO_PULLDOWN;            // matches board_init; defined level if unconnected
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    switch (edge) {
        case CaptureEdge::FALLING: g.Mode = GPIO_MODE_IT_FALLING;         break;
        case CaptureEdge::BOTH:    g.Mode = GPIO_MODE_IT_RISING_FALLING;  break;
        case CaptureEdge::RISING:
        default:                   g.Mode = GPIO_MODE_IT_RISING;          break;
    }
    HAL_GPIO_Init(port_, &g);           // sets SYSCFG EXTICR + RTSR/FTSR + unmasks IMR
    // Leave masked until explicitly enabled by the decoder/scheduler start.
    const uint32_t pm = __get_PRIMASK(); __disable_irq();
    EXTI->IMR &= ~(1u << pin_);
    __set_PRIMASK(pm);
}

void Stm32CaptureChannel::set_capture_enabled(bool enabled) noexcept {
    const uint32_t mask = (1u << pin_);
    // Per-line unmask of EXTI->IMR. The RMW is bracketed by a PRIMASK
    // save/restore (not a blanket enable) so it composes if IRQs are already off.
    const uint32_t pm = __get_PRIMASK(); __disable_irq();
    if (enabled) EXTI->IMR |= mask;
    else         EXTI->IMR &= ~mask;
    __set_PRIMASK(pm);
}

void Stm32CaptureChannel::on_edge(uint32_t ticks) noexcept {
    last_ticks_ = ticks;
    // Post-edge pin level identifies the polarity (correct for single- and
    // both-edge configs on a clean digital input).
    last_edge_ = get_current_level() ? CaptureEdge::RISING : CaptureEdge::FALLING;
    if (cb_) cb_(ticks, cb_data_);
}

// ---------------------------------------------------------------------------
// Shared EXTI dispatch. Latch the timebase at ISR entry, then fan out every
// pending line in this handler's range to its registered channel.
// ---------------------------------------------------------------------------
static inline void exti_service(uint8_t lo, uint8_t hi) noexcept {
    const uint32_t now = TIM5->CNT;     // timestamp first, before any branching
    const uint32_t pr  = EXTI->PR;
    for (uint8_t line = lo; line <= hi; ++line) {
        const uint32_t mask = (1u << line);
        if (pr & mask) {
            EXTI->PR = mask;            // write-1-to-clear pending
            const uint8_t fi = s_freq_line[line];
            if (fi != 0xFFu) {          // frequency pin: record the edge and we're done
                platform_freq_on_edge(fi, now);
                continue;               // (mutually exclusive with a capture role)
            }
            const uint8_t si = s_sent_line[line];
            if (si != 0xFFu) {          // SENT pin: record the falling edge, decode later
                platform_sent_on_edge(si, now);
                continue;
            }
            Stm32CaptureChannel* ch = s_line_channel[line];
            const uint8_t pi = s_pulse_line[line];
            if (pi != 0xFFu) {          // pulse/duty pin: tag the edge by the post-edge level
                platform_pulse_on_edge(pi, now, ch ? ch->get_current_level() : true);
                continue;
            }
            if (ch) ch->on_edge(now);
        }
    }
}

extern "C" void EXTI0_IRQHandler(void)     { exti_service(0, 0); }
extern "C" void EXTI1_IRQHandler(void)     { exti_service(1, 1); }
extern "C" void EXTI9_5_IRQHandler(void)   { exti_service(5, 9); }
extern "C" void EXTI15_10_IRQHandler(void) { exti_service(10, 15); }

extern "C" void Stm32Capture_Init(uint32_t ticks_per_second) {
    __HAL_RCC_SYSCFG_CLK_ENABLE();      // EXTI line -> port mux (SYSCFG_EXTICR)

    // TIM5: free-running 32-bit timebase at `ticks_per_second`.
    // APB1 timer clock = PCLK1 x2 (APB1 prescaler is /4 on this clock tree).
    __HAL_RCC_TIM5_CLK_ENABLE();
    const uint32_t timer_clk = 2u * HAL_RCC_GetPCLK1Freq();
    const uint32_t psc       = ticks_per_second ? (timer_clk / ticks_per_second) : 1u;
    TIM5->PSC = (psc ? psc : 1u) - 1u;
    TIM5->ARR = 0xFFFFFFFFu;
    TIM5->EGR = TIM_EGR_UG;             // load PSC/ARR now
    TIM5->SR  = 0u;                     // clear the update flag raised by UG
    TIM5->CR1 = TIM_CR1_CEN;            // start counting

    // EXTI groups covering jaytek_v1's capture pins (VR1/2 = lines 0/1,
    // DIG1-8 = lines 8..15).
    //
    // Priority 1 — the highest in the land. The capture/decode/schedule block is
    // hard-real-time crank position: it MUST never be delayed by a tooth period
    // (~83 us at 20k RPM on a 36-1 wheel), or two edges coalesce into one EXTI
    // pending bit and a tooth is silently lost. So it sits ABOVE
    // configMAX_SYSCALL_INTERRUPT_PRIORITY (5): no FreeRTOS critical section,
    // __disable_irq() region, or kernel tick can mask it.
    //
    // The hard contract that buys this: NOTHING on this ISR path may call a
    // FreeRTOS API. All handoff to the OS is via shadow state (volatile flags /
    // double-buffered registers) that lower-priority task logic polls. See
    // EnginePositionHal::cycle_start_trampoline.
    HAL_NVIC_SetPriority(EXTI0_IRQn,     1, 0); HAL_NVIC_EnableIRQ(EXTI0_IRQn);
    HAL_NVIC_SetPriority(EXTI1_IRQn,     1, 0); HAL_NVIC_EnableIRQ(EXTI1_IRQn);
    HAL_NVIC_SetPriority(EXTI9_5_IRQn,   1, 0); HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 1, 0); HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}
