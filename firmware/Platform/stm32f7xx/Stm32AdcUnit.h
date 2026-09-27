#pragma once
#include "stm32f7xx_hal.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// Stm32AdcUnit — one ADC peripheral and its DMA stream, as a reusable mechanism.
//
// A unit does at most two things, and which of them a board wants is configuration:
//
//   a background SCAN — the regular sequence free-runs into a circular DMA ring and is
//     folded (see Platform/AdcFold.h) to one filtered word per rank. Readers take the
//     latest word: non-blocking, no reconfigure, no poll-timeout stall. A unit with
//     rank_count == 0 has no scan, which is a legitimate configuration — a board whose
//     knock pins are the ADC's only customers wants exactly that.
//
//   a one-shot BURST — a single channel free-running at a high rate into a one-shot DMA.
//     This is the knock path. If the unit also scans, the burst BORROWS the converter and
//     must give it back (burst_end); if it does not, the burst simply owns it and
//     burst_end has nothing to restore. That difference is the whole of "this board
//     time-shares its knock ADC and that one does not", and it is now a config fact
//     rather than two different drivers.
//
// This file names no board. The rank list, the rate, the ring and where the filtered words
// land all come from the board. It was previously 277 lines of STM32 register code inside
// board_hal_jaytek_v1.cpp, which a second board could only copy.
// See docs/modular-platform-architecture.md, "Layer -1 — the SoC seam".
//
// THE RING MUST LIVE IN .dma_nocache. The D-cache is enabled (platform_init) and DMA
// cannot see through it; the MPU marks that section non-cacheable, which is why no
// per-transfer clean/invalidate appears anywhere below.
// ---------------------------------------------------------------------------
class Stm32AdcUnit {
public:
    struct Cfg {
        ADC_TypeDef*        adc;            // ADC1 / ADC3
        DMA_Stream_TypeDef* dma_stream;     // the stream wired to this ADC (RM0410 Table 28)
        uint32_t            dma_channel;    // DMA_CHANNEL_x for that stream
        IRQn_Type           dma_irq;
        const uint32_t*     ranks;          // ADC_CHANNEL_x per rank, in sequence order
        uint8_t             rank_count;     // 0 = this unit runs no background scan
        uint32_t            sample_time;    // ADC_SAMPLETIME_x for the scan
        uint8_t             oversample;     // scans per ring; each fold averages half
        uint16_t*           ring;           // rank_count * oversample halfwords, .dma_nocache
        volatile uint16_t*  out;            // rank_count filtered words
    };

    // Clocks, handles, DMA and NVIC. Does NOT start conversion — call start_scan().
    void init(const Cfg& cfg) noexcept;

    // (Re)configure the regular sequence and run it as a circular DMA scan. Idempotent, and
    // the single description of "this unit is scanning" — boot and burst_end() both land here,
    // so there are not two ideas of what the resting state is. No-op without ranks.
    void start_scan() noexcept;

    [[nodiscard]] bool     has_scan()      const noexcept { return cfg_.rank_count != 0u; }
    [[nodiscard]] uint32_t last_fold_ms()  const noexcept { return fold_ms_; }
    [[nodiscard]] bool     burst_active()  const noexcept { return burst_; }

    // Fired from the DMA-complete ISR when a burst finishes. Registered once.
    void register_burst_complete(void (*cb)()) noexcept { burst_cb_ = cb; }

    // Arm a one-shot capture of `count` samples on `channel` into `buf` and return at once;
    // the completion callback fires from the ISR. Returns false if the arm was rejected.
    // A successful arm HOLDS the converter — burst_end() as soon as the samples are yours.
    bool burst_start(uint32_t channel, uint32_t sample_time, uint16_t* buf, uint16_t count) noexcept;

    // The same capture, spun on rather than waited for — the bench/CLI path. Returns the
    // sample count (0 on failure), and always returns the converter before it returns.
    // Refuses outright if an interrupt-driven burst is already in flight.
    uint16_t burst_blocking(uint32_t channel, uint32_t sample_time, uint16_t* buf, uint16_t count) noexcept;

    // Release the converter back to its scan (if it has one). Idempotent, so a timeout path
    // may call it without knowing whether the burst ever completed.
    void burst_end() noexcept;

    // ---- ISR entry points (called from the HAL conversion callbacks / DMA handler) ----
    void on_half_complete() noexcept;
    void on_full_complete() noexcept;
    void on_dma_irq() noexcept { HAL_DMA_IRQHandler(&hdma_); }

    [[nodiscard]] ADC_HandleTypeDef* handle() noexcept { return &hadc_; }

    // The unit owning `adc`, or null. Lets the shared HAL conversion callbacks demux.
    static Stm32AdcUnit* by_instance(const ADC_TypeDef* adc) noexcept;

private:
    bool apply_burst_(uint32_t channel, uint32_t sample_time, uint16_t* buf, uint16_t count) noexcept;

    Cfg               cfg_{};
    ADC_HandleTypeDef hadc_{};
    DMA_HandleTypeDef hdma_{};
    volatile uint32_t fold_ms_ = 0;
    volatile bool     burst_   = false;
    void            (*burst_cb_)() = nullptr;
};
