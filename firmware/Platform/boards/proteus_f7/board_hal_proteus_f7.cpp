#include "../../board_hal.h"
#include "board_config.h"
#include "stm32f7xx_hal.h"
#include "Stm32AdcUnit.h"    // the SoC-tier ADC mechanism this board configures
#include "Stm32I2cBus.h"     // the SoC-tier I2C bus the environmental sensor hangs off
#include "Platform/devices/Lps2xBaro.h"
#include "SdCardSpi.h"      // SdCard_Configure — this board's SPI + chip select
#include "proteus_f7_pins.h"    // GENERATED from definition/boards/proteus_f7.board.yaml
// The ACTIVE board's constants, through the neutral shim rather than by naming our own
// header: generated/ holds ONE board at a time, so two spellings could disagree.
#include "../../../../generated/boards/board.h"
#ifndef JAYECU_BOARD_IS_PROTEUS_F7
#error "generated/ was built for a different board. Re-run:  make codegen BOARD=proteus_f7"
#endif

// ---------------------------------------------------------------------------
// Proteus (F7) board HAL
// MCU: STM32F767 (LQFP-144, part number inferred — see the board yaml)
// Pins follow the Proteus open-hardware design.
//
// NOT VERIFIED AGAINST HARDWARE. Every pin here is a transcription.
// ---------------------------------------------------------------------------

using GpioDef = BoardGpioPin;

// ---------------------------------------------------------------------------
// The ADC units. THE DIFFERENCE FROM jaytek_v1 IS THE WHOLE POINT OF THIS FILE:
//
//   ADC1 — 16 regular ranks: AV1-12 (AV12 is the battery sense) then AT1-4. That is
//          EXACTLY the F7 sequencer's maximum of 16, with nothing spare.
//   ADC3 — KNOCK ONLY. Proteus puts every analog input on ADC1, so there is no
//          AV13-16 scan to time-share with. The unit is configured with rank_count 0,
//          which means it runs no background scan and the knock burst simply OWNS the
//          converter: no borrow, no hand-back, no staleness window. On jaytek the same
//          class does the borrowing version. One mechanism, two configurations.
// ---------------------------------------------------------------------------
#define ADC1_NCH        16u    // 12 AV (incl. battery) + 4 AT
#define ADC_OVERSAMPLE  16u    // scans per ring; each fold averages half
#define ADC_SAMPLE_TIME ADC_SAMPLETIME_144CYCLES
// Knock burst sample time: 84 cycles.
// ADC clock = PCLK2/4 = 27 MHz; 12-bit conv = sample(84) + 12 = 96 cycles -> ~281.25 kHz.
#define KNOCK_BURST_SAMPLE_TIME ADC_SAMPLETIME_84CYCLES

static_assert(ADC1_NCH == BOARD_ANALOG_V_COUNT + BOARD_ANALOG_T_COUNT,
              "the ADC1 sequence must cover every analog pin this board has");
static_assert(ADC1_NCH <= 16u, "the STM32F7 regular sequencer holds at most 16 ranks");

// The DMA ring MUST be in .dma_nocache — the D-cache is on and DMA cannot see through it.
static uint16_t __attribute__((section(".dma_nocache"))) s_adc1_ring[ADC1_NCH * ADC_OVERSAMPLE];
static volatile uint16_t s_adc1_filt[ADC1_NCH];   // [0..11] = AV1-12, [12..15] = AT1-4

static uint32_t s_adc1_ranks[ADC1_NCH];
static uint32_t s_knock_ranks[BOARD_KNOCK_COUNT];

static Stm32AdcUnit s_adc1;   // the scan
static Stm32AdcUnit s_adc3;   // knock only (no ranks)

// The on-board environmental sensor and its bus.
static Stm32I2cBus s_baro_bus;
static Lps2xBaro   s_baro;

// Pin tables from the generated schema map.
static const GpioDef* const LS_PINS  = BOARD_LS_PINS;
static const GpioDef* const IGN_PINS = BOARD_IGN_PINS;
static const GpioDef* const HS_PINS  = BOARD_HS_PINS;
static const GpioDef* const LED_PINS = BOARD_LED_PINS;
static const GpioDef* const PG_PINS  = BOARD_PG_PINS;
static const GpioDef* const HBRIDGE_DIS_PINS = BOARD_HBRIDGE_DIS_PINS;
static const GpioDef* const HBRIDGE_DIR_PINS = BOARD_HBRIDGE_DIR_PINS;

// Factory 96-bit Unique Device ID (read-only, at UID_BASE). MSB-first.
void board_device_uid(uint8_t uid[12]) {
    const volatile uint32_t* w = reinterpret_cast<const volatile uint32_t*>(UID_BASE);
    for (int i = 0; i < 3; ++i) {
        const uint32_t v = w[i];
        uid[i * 4 + 0] = static_cast<uint8_t>(v >> 24);
        uid[i * 4 + 1] = static_cast<uint8_t>(v >> 16);
        uid[i * 4 + 2] = static_cast<uint8_t>(v >> 8);
        uid[i * 4 + 3] = static_cast<uint8_t>(v);
    }
}

extern "C" void DMA2_Stream4_IRQHandler(void) { s_adc1.on_dma_irq(); }
extern "C" void DMA2_Stream1_IRQHandler(void) { s_adc3.on_dma_irq(); }

static void gpio_init_outputs(const GpioDef* pins, uint8_t count, GPIO_PinState initial) {
    GPIO_InitTypeDef cfg = {};
    cfg.Mode  = GPIO_MODE_OUTPUT_PP;
    cfg.Pull  = GPIO_NOPULL;
    cfg.Speed = GPIO_SPEED_FREQ_LOW;
    for (uint8_t i = 0; i < count; ++i) {
        HAL_GPIO_WritePin(pins[i].port, pins[i].pin, initial);
        cfg.Pin = pins[i].pin;
        HAL_GPIO_Init(pins[i].port, &cfg);
    }
}

void board_init() {
    __HAL_RCC_GPIOA_CLK_ENABLE(); __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE(); __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE(); __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    GPIO_InitTypeDef cfg = {};

    // ---- SPI3 / SD card — PC10 SCK, PC11 MISO, PC12 MOSI, CS = PD2 ---------
    // Not JTAG pins on this board, so nothing needs releasing first.
    __HAL_RCC_SPI3_CLK_ENABLE();
    cfg.Mode      = GPIO_MODE_AF_PP;
    cfg.Pull      = GPIO_NOPULL;
    cfg.Speed     = GPIO_SPEED_FREQ_HIGH;
    cfg.Alternate = GPIO_AF6_SPI3;
    cfg.Pin       = GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12;
    HAL_GPIO_Init(GPIOC, &cfg);

    HAL_GPIO_WritePin(BOARD_SD_CS_GPIO_PORT, BOARD_SD_CS_GPIO_PIN, GPIO_PIN_SET);
    cfg.Mode      = GPIO_MODE_OUTPUT_PP;
    cfg.Alternate = 0;
    cfg.Pin       = BOARD_SD_CS_GPIO_PIN;
    HAL_GPIO_Init(BOARD_SD_CS_GPIO_PORT, &cfg);
    SdCard_Configure(BOARD_SD_SPI_INSTANCE, BOARD_SD_CS_GPIO_PORT, BOARD_SD_CS_GPIO_PIN);

    // ---- Analog pins -------------------------------------------------------
    //   ADC1: PA0-PA7 (AV5-11 + battery), PC0-PC3 (AV1-4), PC4/PC5 (AT1/AT2), PB0/PB1 (AT3/AT4)
    //   ADC3: PF4/PF5 (KNOCK1/2) — no AV pins on ADC3 at all on this board
    cfg.Mode = GPIO_MODE_ANALOG;
    cfg.Pull = GPIO_NOPULL;
    cfg.Pin  = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3
             | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;
    HAL_GPIO_Init(GPIOA, &cfg);
    cfg.Pin  = GPIO_PIN_0 | GPIO_PIN_1;
    HAL_GPIO_Init(GPIOB, &cfg);
    cfg.Pin  = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5;
    HAL_GPIO_Init(GPIOC, &cfg);
    cfg.Pin  = GPIO_PIN_4 | GPIO_PIN_5;
    HAL_GPIO_Init(GPIOF, &cfg);

    // ---- Rank sequences from the generated (unit, channel) map -------------
    for (uint8_t r = 0; r < BOARD_ANALOG_V_COUNT; ++r) s_adc1_ranks[r] = BOARD_AV_CHANS[r].channel;
    for (uint8_t r = 0; r < BOARD_ANALOG_T_COUNT; ++r)
        s_adc1_ranks[BOARD_ANALOG_V_COUNT + r] = BOARD_AT_CHANS[r].channel;
    for (uint8_t r = 0; r < BOARD_KNOCK_COUNT; ++r)    s_knock_ranks[r] = BOARD_KNOCK_CHANS[r].channel;

    // ---- The two units -----------------------------------------------------
    // Same streams as jaytek (RM0410 Table 28); DMA2 Stream5/3/2 belong to the H-bridge
    // DMA-PWM. ADC3 is given NO ranks and NO ring: knock owns it outright.
    s_adc1.init({ADC1, DMA2_Stream4, DMA_CHANNEL_0, DMA2_Stream4_IRQn,
                 s_adc1_ranks, ADC1_NCH, ADC_SAMPLE_TIME, ADC_OVERSAMPLE,
                 s_adc1_ring, s_adc1_filt});
    s_adc3.init({ADC3, DMA2_Stream1, DMA_CHANNEL_2, DMA2_Stream1_IRQn,
                 nullptr, 0u, ADC_SAMPLE_TIME, ADC_OVERSAMPLE, nullptr, nullptr});
    s_adc1.start_scan();
    s_adc3.start_scan();      // no-op: nothing to scan

    // ---- Environmental sensor bus — LPS25HB on I2C2 (PB10/PB11, AF4) -------
    // Timing word is jaytek's 100 kHz at 54 MHz APB1; same clock tree, same rate.
    s_baro_bus.init({BOARD_BARO_I2C_INSTANCE, GPIOB, GPIO_PIN_10, GPIO_PIN_11,
                     GPIO_AF4_I2C2, 0x40912732u, 10u});
    s_baro.init({&s_baro_bus, BOARD_BARO_I2C_ADDR, 100u, 500u});

    // ---- Output GPIOs ------------------------------------------------------
    // IGN/LS are NOT pre-configured: they stay in reset Hi-Z and are driven only when
    // the PinArbiter binds them. HS/HBRIDGE/LED are not pooled.
    gpio_init_outputs(HS_PINS, BOARD_HIGH_SIDE_COUNT, GPIO_PIN_RESET);
    gpio_init_outputs(HBRIDGE_DIS_PINS, BOARD_HBRIDGE_COUNT, GPIO_PIN_SET);    // HIGH = disabled
    gpio_init_outputs(HBRIDGE_DIR_PINS, BOARD_HBRIDGE_COUNT, GPIO_PIN_RESET);
    gpio_init_outputs(LED_PINS, BOARD_LED_COUNT, GPIO_PIN_RESET);
    gpio_init_outputs(BOARD_HBRIDGE_PWM_PINS, BOARD_HBRIDGE_COUNT, GPIO_PIN_RESET);

    // ---- Input GPIOs -------------------------------------------------------
    // The digital pool is NOT contiguous and NOT on one port here (PC6, then PE11-PE15),
    // so it is initialised pin by pin from the generated table.
    cfg.Mode  = GPIO_MODE_INPUT;
    cfg.Pull  = GPIO_PULLDOWN;
    cfg.Speed = GPIO_SPEED_FREQ_LOW;
    for (const auto& d : BOARD_DIG_PINS) { cfg.Pin = d.pin; HAL_GPIO_Init(d.port, &cfg); }

    // VR trigger inputs
    cfg.Pin = BOARD_VR1_PIN | BOARD_VR2_PIN;
    HAL_GPIO_Init(BOARD_VR1_PORT, &cfg);

    // Power-good monitors on PC14/PC15 — the pins an LSE crystal would otherwise take, which
    // is why BOARD_LSE_HZ is 0 for this board and SystemClock_Config leaves the LSE alone.
    // No pull: open-drain sources with an external pull to VDD, as on jaytek_v1.
    cfg.Mode = GPIO_MODE_INPUT;
    cfg.Pull = GPIO_NOPULL;
    for (uint8_t i = 0; i < BOARD_PG_COUNT; ++i) { cfg.Pin = PG_PINS[i].pin; HAL_GPIO_Init(PG_PINS[i].port, &cfg); }
}

// ---------------------------------------------------------------------------
// ADC reads. Every analog pin is an ADC1 rank here, so there is no second unit to
// consult and no staleness window: index IS the rank.
// ---------------------------------------------------------------------------
uint16_t board_read_adc_voltage(uint8_t index) {
    if (index >= BOARD_ANALOG_V_COUNT) return 0xFFFFu;
    return s_adc1_filt[index];
}

uint16_t board_read_adc_temp(uint8_t index) {
    if (index >= BOARD_ANALOG_T_COUNT) return 0xFFFFu;
    return s_adc1_filt[BOARD_ANALOG_V_COUNT + index];
}

float board_knock_sample_rate(void) { return 27000000.0f / 96.0f; }   // ~281.25 kHz

// Knock owns ADC3 outright on this board, so burst_end() has nothing to restore — the
// borrow protocol is the same call, doing less.
uint16_t board_knock_capture(uint8_t index, uint16_t* buf, uint16_t count) {
    if (index >= BOARD_KNOCK_COUNT) return 0u;
    return s_adc3.burst_blocking(s_knock_ranks[index], KNOCK_BURST_SAMPLE_TIME, buf, count);
}
void board_knock_register_complete(void (*cb)()) { s_adc3.register_burst_complete(cb); }
bool board_knock_start_burst(uint8_t index, uint16_t* buf, uint16_t count) {
    if (index >= BOARD_KNOCK_COUNT) return false;
    return s_adc3.burst_start(s_knock_ranks[index], KNOCK_BURST_SAMPLE_TIME, buf, count);
}
void board_knock_end_burst(void) { s_adc3.burst_end(); }

// ---------------------------------------------------------------------------
// Digital inputs + capture enables — all through the generated pin table, whose
// `line` is the EXTI line the SCHEMA declares rather than one derived from an index.
// ---------------------------------------------------------------------------
bool board_read_digital(uint8_t index) {
    if (index >= BOARD_DIGITAL_IN_COUNT) return false;
    const BoardExtiPin& d = BOARD_DIG_PINS[index];
    return HAL_GPIO_ReadPin(d.port, d.pin) == GPIO_PIN_SET;
}

extern "C" void Stm32Capture_BindFreqLine(uint8_t line, uint8_t freq_idx);
extern "C" void Stm32Capture_BindSentLine(uint8_t line, uint8_t sent_idx);
extern "C" void Stm32Capture_BindPulseLine(uint8_t line, uint8_t pulse_idx);

static uint16_t s_freq_on = 0, s_sent_on = 0, s_pulse_pins = 0;

static bool capture_enable(uint8_t pin, uint32_t mode, uint32_t pull) {
    const BoardExtiPin& d = BOARD_DIG_PINS[pin];
    GPIO_InitTypeDef g = {};
    g.Pin   = d.pin;
    g.Pull  = pull;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Mode  = mode;
    HAL_GPIO_Init(d.port, &g);            // EXTICR + RTSR/FTSR + unmask IMR
    return true;
}

extern "C" bool platform_freq_enable(uint8_t pin) {
    if (pin >= BOARD_DIGITAL_IN_COUNT) return false;
    if (s_pulse_pins & (1u << pin)) return true;   // pulse capture already gives us the period
    if (s_freq_on & (1u << pin)) return true;
    capture_enable(pin, GPIO_MODE_IT_RISING, GPIO_PULLDOWN);
    Stm32Capture_BindFreqLine(BOARD_DIG_PINS[pin].line, pin);
    s_freq_on = static_cast<uint16_t>(s_freq_on | (1u << pin));
    return true;
}

extern "C" bool platform_sent_enable(uint8_t pin) {
    if (pin >= BOARD_DIGITAL_IN_COUNT) return false;
    if (s_sent_on & (1u << pin)) return true;
    capture_enable(pin, GPIO_MODE_IT_FALLING, GPIO_PULLUP);   // SENT idles high
    Stm32Capture_BindSentLine(BOARD_DIG_PINS[pin].line, pin);
    s_sent_on = static_cast<uint16_t>(s_sent_on | (1u << pin));
    return true;
}

extern "C" bool platform_pulse_enable(uint8_t pin) {
    if (pin >= BOARD_DIGITAL_IN_COUNT) return false;
    if (s_pulse_pins & (1u << pin)) return true;
    capture_enable(pin, GPIO_MODE_IT_RISING_FALLING, GPIO_PULLDOWN);
    const uint8_t line = BOARD_DIG_PINS[pin].line;
    Stm32Capture_BindPulseLine(line, pin);
    Stm32Capture_BindFreqLine(line, 0xFFu);   // pulse supersedes any freq route on this line
    s_pulse_pins = static_cast<uint16_t>(s_pulse_pins | (1u << pin));
    return true;
}

extern "C" bool platform_capture_disable(uint8_t pin) {
    if (pin >= BOARD_DIGITAL_IN_COUNT) return false;
    const uint16_t bit = static_cast<uint16_t>(1u << pin);
    if (!((s_freq_on | s_sent_on | s_pulse_pins) & bit)) return false;   // not ours -> leave it
    const BoardExtiPin& d = BOARD_DIG_PINS[pin];
    HAL_GPIO_DeInit(d.port, d.pin);
    __HAL_GPIO_EXTI_CLEAR_IT(d.pin);
    Stm32Capture_BindFreqLine(d.line, 0xFFu);
    Stm32Capture_BindPulseLine(d.line, 0xFFu);
    Stm32Capture_BindSentLine(d.line, 0xFFu);
    s_freq_on    = static_cast<uint16_t>(s_freq_on    & ~bit);
    s_sent_on    = static_cast<uint16_t>(s_sent_on    & ~bit);
    s_pulse_pins = static_cast<uint16_t>(s_pulse_pins & ~bit);
    return true;
}

// ---------------------------------------------------------------------------
// Outputs
// ---------------------------------------------------------------------------
bool board_set_ls(uint8_t index, bool on) {
    if (index >= BOARD_LOW_SIDE_COUNT) return false;
    HAL_GPIO_WritePin(LS_PINS[index].port, LS_PINS[index].pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return true;
}
bool board_set_hs(uint8_t index, bool on) {
    if (index >= BOARD_HIGH_SIDE_COUNT) return false;
    HAL_GPIO_WritePin(HS_PINS[index].port, HS_PINS[index].pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return true;
}
bool board_set_ign(uint8_t index, bool on) {
    if (index >= BOARD_IGNITION_COUNT) return false;
    HAL_GPIO_WritePin(IGN_PINS[index].port, IGN_PINS[index].pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return true;
}
bool board_hbridge_set_enable(uint8_t i, bool enabled) {
    if (i >= BOARD_HBRIDGE_COUNT) return false;
    // TLE9201 DIS: LOW = enabled, HIGH = disabled (same sense as jaytek's IFX9201).
    HAL_GPIO_WritePin(HBRIDGE_DIS_PINS[i].port, HBRIDGE_DIS_PINS[i].pin,
                      enabled ? GPIO_PIN_RESET : GPIO_PIN_SET);
    return true;
}
bool board_hbridge_set_direction(uint8_t i, bool forward) {
    if (i >= BOARD_HBRIDGE_COUNT) return false;
    HAL_GPIO_WritePin(HBRIDGE_DIR_PINS[i].port, HBRIDGE_DIR_PINS[i].pin,
                      forward ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return true;
}
void board_set_led(uint8_t index, bool on) {
    if (index >= BOARD_LED_COUNT) return;
    HAL_GPIO_WritePin(LED_PINS[index].port, LED_PINS[index].pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

// The 5 V follower supplies' power-good lines (PC14 = PG1, PC15 = PG2). HIGH = good.
// Meaningful only with the key on — with it off, USB back-feeds the followers through a diode.
bool board_read_power_good(uint8_t index) {
    if (index >= BOARD_PG_COUNT) return false;
    return HAL_GPIO_ReadPin(PG_PINS[index].port, PG_PINS[index].pin) == GPIO_PIN_SET;
}

// ---------------------------------------------------------------------------
// Environmental sensor — service() is the ONLY member that touches I2C, and it is
// called from a background task. The readers cannot reach the bus.
// ---------------------------------------------------------------------------
void  board_baro_service()     { s_baro.service(HAL_GetTick()); }
float board_read_baro_kpa()    { return s_baro.kpa(); }
float board_read_baro_temp_c() { return s_baro.temp_c(); }
bool  board_baro_valid()       { return s_baro.valid(HAL_GetTick()); }
