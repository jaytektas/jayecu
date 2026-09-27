#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Stm32DmaPwm — DMA-driven PWM for pins with NO timer alternate function (the two H-bridge pins
// PD6/PD3). TIM1 events fire DMA2 transfers that write constant words to GPIOD->BSRR: the UPDATE
// event sets both pins at the start of the period, and the CC1/CC2 compare matches reset PD6/PD3 at
// their respective duty points. Hardware-timed edges, zero CPU per edge, zero jitter — the only way
// to get clean high-frequency PWM on pins that can't reach a timer channel. See docs/pwm-hal-design.md §3.1.
//
// Stream map (RM0410 Table 28, dodging the ADC's DMA2 Stream1/Stream4):
//   TIM1_UP  -> DMA2 Stream5 / Ch6   (set PD6|PD3)
//   TIM1_CH1 -> DMA2 Stream3 / Ch6   (reset PD6 at CCR1)
//   TIM1_CH2 -> DMA2 Stream2 / Ch6   (reset PD3 at CCR2)
//
// Both channels share TIM1's period (one carrier) with independent duty — correct for two ETBs or a
// bipolar stepper's two coils. **BENCH-VALIDATED ONLY** (register-level, no host coverage). enable()
// is engine-wide: the H-bridge actuator always enables/disables both bridges together.
// ---------------------------------------------------------------------------
class Stm32DmaPwm {
public:
    static constexpr int CH_A = 0;   // TIM1_CH1 / CCR1
    static constexpr int CH_B = 1;   // TIM1_CH2 / CCR2

    // `bit_a` / `bit_b` are the GPIOD bit numbers of the two pins. They are a BOARD fact --
    // jaytek drives PD6/PD3, proteus PD12/PD13 -- and were compiled in here as PD6/PD3
    // constants, which is the one thing that stopped a second board reusing this engine.
    // Both pins must be on GPIOD, because the DMA writes one port's BSRR.
    void init(uint32_t pwm_freq_hz, uint8_t bit_a, uint8_t bit_b) noexcept;
    void set_freq(uint32_t hz) noexcept;          // ARR = f_tim/hz (shared by both channels)
    void set_duty(int ch, float pct) noexcept;    // CCRx = pct% * (ARR+1)

    void start() noexcept;                        // run TIM1 + the 3 DMA streams
    void stop()  noexcept;                        // park both pins low, halt the engine

private:
    // The three constant BSRR words the DMA streams replay each cycle do NOT live here — they are in
    // the .dma_nocache section (see s_bsrr in the .cpp). They were members once, cleaned out of the
    // D-cache with a single SCB_CleanDCache_by_Addr(&set_word_, 32). That is correct only if the object
    // happens to be 32-byte aligned: a cache line is 32 bytes, so a 32-byte clean from an unaligned
    // address flushes ONE line. This object landed at 0x2007731c, so the clean reached set_word_ and
    // missed both reset words — the DMA then replayed zero into BSRR, PD6 was set every period and
    // never reset, and the bridge sat at 100 % duty with the plate jammed on a stop. It depended
    // entirely on where the linker put the object, so it came and went with unrelated edits.
    uint32_t arr_     = 0;
    uint32_t tclk_    = 0;             // TIM1 kernel clock (Hz)
    uint8_t  bit_a_   = 0;
    uint8_t  bit_b_   = 0;
    bool     running_ = false;
};
