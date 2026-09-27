#include "Stm32DmaPwm.h"
#include "stm32f7xx_hal.h"

// Two GPIOD pins driven as PWM without a timer AF: the DMA writes their edges to GPIOD->BSRR.
// BSRR low 16 bits = set, high 16 bits = reset. WHICH pins is the board's to say (init).


// The DMA source words, in NON-CACHEABLE RAM. Three constant BSRR values the streams replay forever
// with MINC off. .dma_nocache is NOLOAD (never zeroed at boot), which is fine: init() below assigns
// all three before any stream is armed.
namespace {
struct BsrrWords { volatile uint32_t set_both, rst_pd6, rst_pd3; };
BsrrWords __attribute__((section(".dma_nocache"))) s_bsrr;
}  // namespace

void Stm32DmaPwm::init(uint32_t pwm_freq_hz, uint8_t bit_a, uint8_t bit_b) noexcept {
    bit_a_ = bit_a;
    bit_b_ = bit_b;
    const uint32_t PD6 = bit_a_, PD3 = bit_b_;   // local names; the maths below is unchanged
    __HAL_RCC_TIM1_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();      // already on for the ADC; harmless to re-enable
    __HAL_RCC_GPIOD_CLK_ENABLE();

    // PD6/PD3 -> push-pull outputs, high speed, start LOW. The DMA drives them via BSRR thereafter.
    GPIOD->BSRR = (1u << (PD6 + 16)) | (1u << (PD3 + 16));                       // reset both
    GPIOD->MODER   = (GPIOD->MODER & ~((3u << (PD6 * 2)) | (3u << (PD3 * 2))))
                   | (1u << (PD6 * 2)) | (1u << (PD3 * 2));                      // 01 = output
    GPIOD->OTYPER &= ~((1u << PD6) | (1u << PD3));                               // push-pull
    GPIOD->OSPEEDR |= (3u << (PD6 * 2)) | (3u << (PD3 * 2));                     // very high speed

    // These live in .dma_nocache, so the DMA and the CPU see the same memory with no maintenance at
    // all. Cache cleaning by address was tried here and is a trap: the words were class members and a
    // single 32-byte clean only flushes the one cache line the (unaligned) start address falls in, so
    // whether the reset words reached RAM depended on where the linker placed the object. Non-cacheable
    // memory has no such failure mode, and it is what every other DMA buffer in this firmware uses.
    s_bsrr.set_both = (1u << PD6) | (1u << PD3);   // BSRR: set both at the start of each period
    s_bsrr.rst_pd6  = (1u << (PD6 + 16));          // BSRR: reset PD6 at its duty point
    s_bsrr.rst_pd3  = (1u << (PD3 + 16));

    // TIM1 kernel clock = APB2 timer clock (PCLK2, doubled when the APB2 prescaler is /=1).
    tclk_ = HAL_RCC_GetPCLK2Freq();
    if (((RCC->CFGR & RCC_CFGR_PPRE2) >> RCC_CFGR_PPRE2_Pos) >= 4u) tclk_ *= 2u;

    set_freq(pwm_freq_hz);                      // sets PSC + ARR for the requested band

    TIM1->CR1   = 0;                             // edge-aligned up-count, counter off
    TIM1->CCR1  = 0; TIM1->CCR2 = 0;
    TIM1->CCMR1 = 0;                             // CH1/CH2 as output compare (CCxS=00), OCxM frozen
    TIM1->CCER  = 0;                             // no pin outputs (CCxE=0) — we only use the events
    TIM1->RCR   = 0;
    TIM1->EGR   = TIM_EGR_UG;                    // latch PSC/ARR
    TIM1->SR    = 0;
    TIM1->DIER  = TIM_DIER_UDE | TIM_DIER_CC1DE | TIM_DIER_CC2DE;   // DMA on update + CC1 + CC2

    // --- DMA2 streams: one constant word -> GPIOD->BSRR, circular, NDTR=1, 32-bit, no inc/FIFO. ---
    auto cfg = [](DMA_Stream_TypeDef* s, uint32_t ch, volatile uint32_t* src) {
        s->CR &= ~DMA_SxCR_EN;
        while (s->CR & DMA_SxCR_EN) { }
        s->PAR  = reinterpret_cast<uint32_t>(&GPIOD->BSRR);
        s->M0AR = reinterpret_cast<uint32_t>(src);
        s->NDTR = 1;
        s->FCR  = 0;                              // direct mode (FIFO off)
        s->CR   = (ch << DMA_SxCR_CHSEL_Pos)
                | DMA_SxCR_DIR_0                  // 01 = memory-to-peripheral
                | DMA_SxCR_CIRC                   // circular (replay forever)
                | DMA_SxCR_MSIZE_1                // 32-bit memory
                | DMA_SxCR_PSIZE_1                // 32-bit peripheral
                | DMA_SxCR_PL_1;                  // high priority; MINC/PINC off (constant word)
    };
    cfg(DMA2_Stream5, 6, &s_bsrr.set_both);            // TIM1_UP  -> set PD6|PD3
    cfg(DMA2_Stream3, 6, &s_bsrr.rst_pd6);             // TIM1_CH1 -> reset PD6
    cfg(DMA2_Stream2, 6, &s_bsrr.rst_pd3);             // TIM1_CH2 -> reset PD3

}

void Stm32DmaPwm::set_freq(uint32_t hz) noexcept {
    if (!hz || !tclk_) { arr_ = 0; return; }
    // Match the timer clock to the target band: pick the SMALLEST prescaler that fits one PWM period in
    // the 16-bit counter. At full clock 16 bits only reaches ~3.3 kHz; prescaling extends the low end so
    // the whole 1 Hz..50 kHz range works (slow BAC/solenoid AND whine-free ETB) at max duty resolution.
    uint32_t total = tclk_ / hz;                  // timer ticks per period at full clock
    uint32_t div   = (total + 65535u) / 65536u;   // ceil(total / 65536) = prescaler divider
    if (div == 0u) div = 1u;
    uint32_t period = total / div;                // ticks within the prescaled counter
    if (period < 2u)     period = 2u;
    if (period > 65536u) period = 65536u;
    arr_ = period - 1u;
    TIM1->PSC = div - 1u;                          // prescaler register = divider - 1
    TIM1->ARR = arr_;
    TIM1->EGR = TIM_EGR_UG;                        // latch PSC + ARR
}

void Stm32DmaPwm::set_duty(int ch, float pct) noexcept {
    if (pct < 0.0f) pct = 0.0f; else if (pct > 100.0f) pct = 100.0f;
    const uint32_t ccr = static_cast<uint32_t>((pct / 100.0f) * static_cast<float>(arr_ + 1u));
    if (ch == CH_A) TIM1->CCR1 = ccr; else TIM1->CCR2 = ccr;
}

// (Re)arm ONE BSRR stream from a known-clean state. The intermittent post-boot "carrier won't start"
// was a classic STM32 DMA start race: setting DMA_SxCR_EN while a stale error flag (TEIF/FEIF/DMEIF) is
// still set makes the hardware SILENTLY refuse to start the stream — and only a full reset cleared the
// flag (which is why a clean SWD reset revived it but a flash-reset/set_freq didn't). Fix: per stream,
// disable + wait for EN to actually fall, clear ALL of its flags, reload NDTR, THEN enable. The CR
// config (channel/dir/circular/sizes/addresses) set in init() survives a clear of just the EN bit.
// `ifcr` is DMA2->LIFCR (streams 0-3) or HIFCR (4-7); flag_mask is that stream's 5 flags (write-1-clear).
static inline void arm_stream(DMA_Stream_TypeDef* s, volatile uint32_t* ifcr, uint32_t flag_mask) noexcept {
    s->CR &= ~DMA_SxCR_EN;
    while (s->CR & DMA_SxCR_EN) { }      // EN must read 0 before reconfigure (HW drains any transfer)
    *ifcr   = flag_mask;                 // clear this stream's stale flags so EN can stick
    s->NDTR = 1;
    s->CR  |= DMA_SxCR_EN;
}

void Stm32DmaPwm::start() noexcept {
    if (running_) return;
    TIM1->CR1 &= ~TIM_CR1_CEN;           // stop the timer so no DMA request races the stream (re)arm
    // Arm all three streams cleanly while the timer is stopped, so the first TIM event finds them ready.
    arm_stream(DMA2_Stream5, &DMA2->HIFCR, 0x00000F40u);   // TIM1_UP  -> set PD6|PD3   (FEIF/DMEIF/TEIF/HTIF/TCIF 5)
    arm_stream(DMA2_Stream3, &DMA2->LIFCR, 0x0F400000u);   // TIM1_CH1 -> reset PD6      (...3)
    arm_stream(DMA2_Stream2, &DMA2->LIFCR, 0x003D0000u);   // TIM1_CH2 -> reset PD3      (...2)
    TIM1->EGR  = TIM_EGR_UG;             // reset CNT + latch PSC/ARR + prime the first 'set' via stream5
    TIM1->CR1 |= TIM_CR1_CEN;            // run — streams already armed and waiting
    running_ = true;
}

void Stm32DmaPwm::stop() noexcept {
    TIM1->CR1 &= ~TIM_CR1_CEN;
    DMA2_Stream5->CR &= ~DMA_SxCR_EN;
    DMA2_Stream3->CR &= ~DMA_SxCR_EN;
    DMA2_Stream2->CR &= ~DMA_SxCR_EN;
    GPIOD->BSRR = s_bsrr.rst_pd6 | s_bsrr.rst_pd3;           // park both pins low
    running_ = false;
}
