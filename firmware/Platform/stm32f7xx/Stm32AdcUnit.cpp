#include "Stm32AdcUnit.h"
#include "../AdcFold.h"

// Units register themselves so the shared HAL conversion callbacks (one pair for the whole
// application) can demux by ADC instance. Two is what an F767 board uses today (ADC1 + ADC3);
// the table is sized for all three.
namespace {
constexpr uint8_t kMaxUnits = 3;
Stm32AdcUnit* s_units[kMaxUnits] = {};

void adc_clock_enable(const ADC_TypeDef* adc) {
    if      (adc == ADC1) { __HAL_RCC_ADC1_CLK_ENABLE(); }
    else if (adc == ADC2) { __HAL_RCC_ADC2_CLK_ENABLE(); }
    else if (adc == ADC3) { __HAL_RCC_ADC3_CLK_ENABLE(); }
}
}  // namespace

Stm32AdcUnit* Stm32AdcUnit::by_instance(const ADC_TypeDef* adc) noexcept {
    for (auto* u : s_units) if (u && u->cfg_.adc == adc) return u;
    return nullptr;
}

void Stm32AdcUnit::init(const Cfg& cfg) noexcept {
    cfg_ = cfg;
    for (auto*& slot : s_units) { if (slot == nullptr || slot == this) { slot = this; break; } }

    adc_clock_enable(cfg_.adc);
    __HAL_RCC_DMA2_CLK_ENABLE();

    hadc_.Instance                   = cfg_.adc;
    hadc_.Init.ClockPrescaler        = ADC_CLOCK_SYNC_PCLK_DIV4;
    hadc_.Init.Resolution            = ADC_RESOLUTION_12B;
    hadc_.Init.ScanConvMode          = ENABLE;
    hadc_.Init.ContinuousConvMode    = ENABLE;
    hadc_.Init.DiscontinuousConvMode = DISABLE;
    hadc_.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hadc_.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
    hadc_.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    hadc_.Init.NbrOfConversion       = cfg_.rank_count ? cfg_.rank_count : 1u;
    hadc_.Init.DMAContinuousRequests = ENABLE;
    hadc_.Init.EOCSelection          = ADC_EOC_SINGLE_CONV;
    HAL_ADC_Init(&hadc_);

    hdma_.Instance                 = cfg_.dma_stream;
    hdma_.Init.Channel             = cfg_.dma_channel;
    hdma_.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    hdma_.Init.Mode                = DMA_CIRCULAR;
    hdma_.Init.Priority            = DMA_PRIORITY_HIGH;
    hdma_.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;

    // Link the DMA handle NOW, not on the first start_scan(). A unit with no ranks never calls
    // start_scan() at all, and HAL_ADC_Stop_DMA() -- which burst_start() calls before re-tasking
    // the converter -- dereferences hadc_.DMA_Handle. Leaving it null until the first scan meant a
    // knock-only converter faulted on its first burst, which no board that also scans would show.
    HAL_DMA_Init(&hdma_);
    __HAL_LINKDMA(&hadc_, DMA_Handle, hdma_);

    // Priority 8: BELOW configMAX_SYSCALL_INTERRUPT_PRIORITY (5), so it is harmless to the RTOS,
    // and the fold ISR is short (rank_count x oversample/2 adds, twice per ring).
    HAL_NVIC_SetPriority(cfg_.dma_irq, 8, 0);
    HAL_NVIC_EnableIRQ(cfg_.dma_irq);
}

void Stm32AdcUnit::start_scan() noexcept {
    if (!has_scan()) { burst_ = false; return; }   // knock-only unit: nothing to return to

    hadc_.Init.ScanConvMode       = ENABLE;
    hadc_.Init.ContinuousConvMode = ENABLE;
    hadc_.Init.NbrOfConversion    = cfg_.rank_count;
    HAL_ADC_Init(&hadc_);

    ADC_ChannelConfTypeDef rc = {};
    rc.SamplingTime = cfg_.sample_time;
    for (uint8_t r = 0; r < cfg_.rank_count; ++r) {
        rc.Channel = cfg_.ranks[r];
        rc.Rank    = static_cast<uint32_t>(r) + 1u;
        HAL_ADC_ConfigChannel(&hadc_, &rc);
    }

    hdma_.Init.Mode = DMA_CIRCULAR;
    HAL_DMA_Init(&hdma_);
    __HAL_LINKDMA(&hadc_, DMA_Handle, hdma_);

    burst_ = false;                                 // folds route to the scan again
    HAL_ADC_Start_DMA(&hadc_, reinterpret_cast<uint32_t*>(cfg_.ring),
                      static_cast<uint32_t>(cfg_.rank_count) * cfg_.oversample);
}

// Re-task the converter to a single channel, free-running, one-shot DMA of `count` samples.
// Returns whether the transfer actually started — the ONE place the burst is started, so the
// callers below must not start it again.
bool Stm32AdcUnit::apply_burst_(uint32_t channel, uint32_t sample_time,
                                uint16_t* buf, uint16_t count) noexcept {
    hadc_.Init.ScanConvMode       = DISABLE;
    hadc_.Init.ContinuousConvMode = ENABLE;         // free-run the channel across the burst
    hadc_.Init.NbrOfConversion    = 1u;
    HAL_ADC_Init(&hadc_);

    ADC_ChannelConfTypeDef rc = {};
    rc.Channel      = channel;
    rc.Rank         = 1u;
    rc.SamplingTime = sample_time;
    HAL_ADC_ConfigChannel(&hadc_, &rc);

    hdma_.Init.Mode = DMA_NORMAL;                   // one-shot (not circular)
    HAL_DMA_Init(&hdma_);
    __HAL_LINKDMA(&hadc_, DMA_Handle, hdma_);

    return HAL_ADC_Start_DMA(&hadc_, reinterpret_cast<uint32_t*>(buf), count) == HAL_OK;
}

bool Stm32AdcUnit::burst_start(uint32_t channel, uint32_t sample_time,
                               uint16_t* buf, uint16_t count) noexcept {
    if (buf == nullptr || count == 0u) return false;

    // The IRQ is masked across the flip because a completion left pending by the OLD
    // configuration must not be serviced against the NEW one — it would fold burst samples
    // into the scan's filter, or fire the knock callback for a scan.
    HAL_NVIC_DisableIRQ(cfg_.dma_irq);
    HAL_ADC_Stop_DMA(&hadc_);                       // stop the scan / any prior burst
    HAL_NVIC_ClearPendingIRQ(cfg_.dma_irq);         // drop the scan's last completion, not the burst's
    burst_ = true;                                  // route the completion to the burst callback
    const bool ok = apply_burst_(channel, sample_time, buf, count);
    HAL_NVIC_EnableIRQ(cfg_.dma_irq);
    if (!ok) burst_end();                           // a failed arm must not keep the converter
    return ok;
}

uint16_t Stm32AdcUnit::burst_blocking(uint32_t channel, uint32_t sample_time,
                                      uint16_t* buf, uint16_t count) noexcept {
    if (buf == nullptr || count == 0u) return 0u;
    // Refuse rather than stamp on a burst the worker has in flight: this is the bench/CLI
    // path, and the worker owns the converter whenever the engine is firing with knock on.
    if (burst_) return 0u;

    // Poll-driven, so no DMA ISR may run during the burst (it would fold burst data into the
    // scan's filter).
    HAL_NVIC_DisableIRQ(cfg_.dma_irq);
    HAL_ADC_Stop_DMA(&hadc_);
    (void)apply_burst_(channel, sample_time, buf, count);

    uint32_t guard = 0u;                            // guarded spin so a stall cannot hang the CLI
    while (__HAL_DMA_GET_COUNTER(&hdma_) > 0u && ++guard < 5000000u) { }
    const uint16_t got = (__HAL_DMA_GET_COUNTER(&hdma_) == 0u) ? count : 0u;

    // Hand the converter straight back. The samples are already in `buf` — nothing downstream
    // needs it held — so the scan resumes before this returns, even on the failed path.
    HAL_ADC_Stop_DMA(&hadc_);
    HAL_NVIC_ClearPendingIRQ(cfg_.dma_irq);         // a completion from the burst config, not the scan
    start_scan();
    HAL_NVIC_EnableIRQ(cfg_.dma_irq);
    return got;
}

void Stm32AdcUnit::burst_end() noexcept {
    if (!burst_) return;
    HAL_NVIC_DisableIRQ(cfg_.dma_irq);
    HAL_ADC_Stop_DMA(&hadc_);
    HAL_NVIC_ClearPendingIRQ(cfg_.dma_irq);         // drop a completion raised by the burst config
    start_scan();                                   // clears burst_; no-op for a knock-only unit
    burst_ = false;
    HAL_NVIC_EnableIRQ(cfg_.dma_irq);
}

// Half/full completion: fold the half the DMA is NOT currently filling, so the fold never
// races the transfer and no double-buffer copy is needed.
void Stm32AdcUnit::on_half_complete() noexcept {
    if (burst_ || !has_scan()) return;
    adc_fold_ranks(cfg_.ring, cfg_.rank_count, cfg_.oversample / 2u, cfg_.out);
    fold_ms_ = HAL_GetTick();
}

void Stm32AdcUnit::on_full_complete() noexcept {
    if (burst_) { if (burst_cb_) burst_cb_(); return; }   // burst done -> wake the worker
    if (!has_scan()) return;
    adc_fold_ranks(cfg_.ring + static_cast<uint32_t>(cfg_.rank_count) * (cfg_.oversample / 2u),
                   cfg_.rank_count, cfg_.oversample / 2u, cfg_.out);
    fold_ms_ = HAL_GetTick();
}

// The application's single pair of HAL conversion callbacks, demuxed to the owning unit.
extern "C" void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef* hadc) {
    if (auto* u = Stm32AdcUnit::by_instance(hadc->Instance)) u->on_half_complete();
}
extern "C" void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc) {
    if (auto* u = Stm32AdcUnit::by_instance(hadc->Instance)) u->on_full_complete();
}
