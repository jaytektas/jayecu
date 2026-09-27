#include "../../board_hal.h"
#include "board_config.h"
#include "stm32f7xx_hal.h"
#include "Stm32AdcUnit.h"   // the SoC-tier ADC mechanism this board configures
#include "Stm32I2cBus.h"    // the SoC-tier I2C bus the environmental sensor hangs off
#include "Platform/devices/Lps2xBaro.h"   // the part driver (LPS22HB here, LPS25HB elsewhere)
#include "SdCardSpi.h"      // SdCard_Configure — this board's SPI + chip select
#include "jaytek_v1_pins.h"    // GENERATED from definition/boards/jaytek_v1.board.yaml
// The ACTIVE board's constants, through the neutral shim rather than by naming our own
// header: generated/ holds ONE board at a time, so two spellings could disagree.
#include "../../../../generated/boards/board.h"
#ifndef JAYECU_BOARD_IS_JAYTEK_V1
#error "generated/ was built for a different board. Re-run:  make codegen BOARD=jaytek_v1"
#endif

// ---------------------------------------------------------------------------
// Jaytek V1 board HAL implementation
// MCU: STM32F767ZIT6, LQFP-144
// Source: the EasyEDA Pro schematic's netlist export (2026-05-22, not shipped)
// ---------------------------------------------------------------------------

// Module-scope HAL handles — not globals; only this file touches them.
// The on-board environmental sensor and the bus it sits on. Which peripheral, which pins and
// which address are this board's to say; the LPS2x register map is the part driver's.
static Stm32I2cBus s_baro_bus;
static Lps2xBaro   s_baro;


// Factory 96-bit Unique Device ID (read-only, at UID_BASE). Three 32-bit words → 12 bytes, MSB-first.
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

// ---------------------------------------------------------------------------
// GPIO pin tables — the port/bit MAP comes from the board schema (generated
// jaytek_v1_pins.h: BOARD_IGN_PINS / BOARD_LS_PINS / ...). This file keeps only
// the driver logic. GpioDef is the generated row type so the rest of the file
// is unchanged.
// ---------------------------------------------------------------------------

using GpioDef = BoardGpioPin;

// ---------------------------------------------------------------------------
// This board's two ADC units, as CONFIGURATION of Stm32AdcUnit (the mechanism lives in
// Platform/stm32f7xx/Stm32AdcUnit.cpp; none of the register code is here any more).
//
//   ADC1 — 16 regular ranks: AV1-12 then AT1-4, free-running into a circular DMA ring.
//   ADC3 — 4 regular ranks (AV13-16) AND the knock pins (IN14/15), which TIME-SHARE it.
//          ADC3 runs the AV13-16 scan by default; a knock burst borrows the converter and
//          hands it straight back (board_knock_end_burst). One converter cannot do both at
//          once — a 281 kHz burst has no room for a 4-channel scan interleaved into it
//          without punching holes in the waveform the DSP is about to analyse — but a burst
//          is short and occasional (one per firing cylinder, ~0.7 ms at 6000 rpm against
//          5 ms between events on a four), so the scan owns ADC3 most of the time and
//          AV13-16 stay live with knock enabled. What is NOT allowed is handing out a value
//          the scan stopped refreshing: the unit dates its last fold and
//          board_read_adc_voltage() fails AV13-16 rather than return a frozen count.
//
// The F767 ADC has NO hardware oversampler (that unit is on L4/G4/H7), so oversampling is
// software: each fold averages half a ring, and that average IS the reading. No IIR sits
// behind it — see Platform/AdcFold.h for why, and for the measurements.
// ---------------------------------------------------------------------------
#define ADC1_NCH         16u                        // 12 AV + 4 AT regular ranks
#define ADC3_NCH          4u                        // AV13-16 (IN4-7); knock is IN14/15
#define ADC_OVERSAMPLE   16u                        // scans per ring; each fold averages half
// Per-channel sample time = the scan-rate knob. Bench-validated: readings are bit-stable from
// 480 down to 15 cycles (the MCP6004-buffered ~3.6 kΩ sources settle in <1 µs); only 3 cycles
// under-charges the S/H. 144 cycles gives a ~10.8 kHz 16-channel scan with ~10x settling
// margin — fast enough that MAP (when windowed-averaged) gets a 10 kHz raw feed off this scan.
#define ADC_SAMPLE_TIME  ADC_SAMPLETIME_144CYCLES
// Knock burst sample time: 84 cycles. ADC clock = PCLK2/4 = 27 MHz
// (216 MHz core), 12-bit conv = sample(84) + 12 = 96 cycles -> ~281.25 kHz.
#define KNOCK_BURST_SAMPLE_TIME  ADC_SAMPLETIME_84CYCLES

// The DMA rings MUST be in .dma_nocache — the D-cache is on and DMA cannot see through it.
static uint16_t __attribute__((section(".dma_nocache"))) s_adc1_ring[ADC1_NCH * ADC_OVERSAMPLE];
static uint16_t __attribute__((section(".dma_nocache"))) s_adc3_ring[ADC3_NCH * ADC_OVERSAMPLE];

// Filtered counts, one word per rank, in each unit's own sequence order.
static volatile uint16_t s_adc1_filt[ADC1_NCH];   // [0..11] = AV1-12, [12..15] = AT1-4
static volatile uint16_t s_adc3_filt[ADC3_NCH];   // [0..3]  = AV13-16

// The rank sequences, filled in board_init() from the generated (unit, channel) map.
static uint32_t s_adc1_ranks[ADC1_NCH];
static uint32_t s_adc3_ranks[ADC3_NCH];
static uint32_t s_knock_ranks[2];                 // KNOCK1/2 ADC channels (burst, not a scan)

static Stm32AdcUnit s_adc1;
static Stm32AdcUnit s_adc3;

// Where an AV pool index lives: the first ADC1_AV_COUNT are ADC1 ranks, the rest ADC3 ranks.
// This is the one genuinely board-shaped fact about the split, so it is stated once here.
static constexpr uint8_t ADC1_AV_COUNT = ADC1_NCH - BOARD_ANALOG_T_COUNT;   // 12

// Generous: it only has to outlast the longest legitimate gap, which is one full 2048-sample
// burst (7.3 ms) plus the worker's own 25 ms burst timeout. Past that it is a stuck ADC.
static constexpr uint32_t ADC3_AV_STALE_MS = 50u;

// Pin tables (port/bit) come from the generated schema map; these aliases keep
// the index-based names the rest of this file uses.
static const GpioDef* const LS_PINS      = BOARD_LS_PINS;
static const GpioDef* const IGN_PINS     = BOARD_IGN_PINS;
static const GpioDef* const HS_PINS      = BOARD_HS_PINS;
static const GpioDef* const LED_PINS     = BOARD_LED_PINS;
static const GpioDef* const PG_PINS      = BOARD_PG_PINS;
static const GpioDef* const HBRIDGE_DIS_PINS = BOARD_HBRIDGE_DIS_PINS;
static const GpioDef* const HBRIDGE_DIR_PINS = BOARD_HBRIDGE_DIR_PINS;

// The DMA stream interrupts, routed to the unit that owns each stream. The folds, the mode
// flip and the HAL conversion callbacks all live in Stm32AdcUnit now.
extern "C" void DMA2_Stream4_IRQHandler(void) { s_adc1.on_dma_irq(); }
extern "C" void DMA2_Stream1_IRQHandler(void) { s_adc3.on_dma_irq(); }

// ---------------------------------------------------------------------------
// GPIO output helper — initialise a list of pins as push-pull outputs
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// board_init
// ---------------------------------------------------------------------------

void board_init() {
    // ---- GPIO clocks -------------------------------------------------------
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    GPIO_InitTypeDef cfg = {};

    // ---- SPI3 / SD card ----------------------------------------------------
    // PB3 (SCK) and PB4 (MISO) default to JTAG (TDO/TRST) on STM32F7.
    // Explicitly configuring them as AF6 (SPI3) releases them from JTAG.
    // SWD on PA13/PA14 is unaffected.
    __HAL_RCC_SPI3_CLK_ENABLE();
    cfg.Mode      = GPIO_MODE_AF_PP;
    cfg.Pull      = GPIO_NOPULL;
    cfg.Speed     = GPIO_SPEED_FREQ_HIGH;
    cfg.Alternate = GPIO_AF6_SPI3;
    cfg.Pin       = GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5; // SCK, MISO, MOSI
    HAL_GPIO_Init(GPIOB, &cfg);

    // PA15 — SD chip-select, output HIGH (deselected)
    HAL_GPIO_WritePin(BOARD_SD_CS_GPIO_PORT, BOARD_SD_CS_GPIO_PIN, GPIO_PIN_SET);
    cfg.Mode      = GPIO_MODE_OUTPUT_PP;
    cfg.Pull      = GPIO_NOPULL;
    cfg.Speed     = GPIO_SPEED_FREQ_HIGH;
    cfg.Alternate = 0;
    cfg.Pin       = BOARD_SD_CS_GPIO_PIN;
    HAL_GPIO_Init(BOARD_SD_CS_GPIO_PORT, &cfg);

    // Tell the SD driver which peripheral and which chip select are ours. It used to take
    // these from the SoC tier's platform_config.h, i.e. from this board's pins whichever
    // board was being built.
    SdCard_Configure(BOARD_SD_SPI_INSTANCE, BOARD_SD_CS_GPIO_PORT, BOARD_SD_CS_GPIO_PIN);

    // ---- Analog pins -------------------------------------------------------
    // Every AV/AT/KNOCK pin into GPIO analog mode. The ADC peripheral clocks are the unit's
    // business (Stm32AdcUnit::init); what is board-specific is WHICH pins are analog.
    //   ADC1: PA0-PA7 (AV1-8), PC4/PC5 (AV9/10), PB0/PB1 (AV11/12), PC0-PC3 (AT1-4)
    //   ADC3: PF6-PF9 (AV13-16), PF4/PF5 (KNOCK1/2)
    cfg.Mode = GPIO_MODE_ANALOG;
    cfg.Pull = GPIO_NOPULL;
    cfg.Pin  = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3
             | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;
    HAL_GPIO_Init(GPIOA, &cfg);
    cfg.Pin  = GPIO_PIN_0 | GPIO_PIN_1;
    HAL_GPIO_Init(GPIOB, &cfg);
    cfg.Pin  = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3
             | GPIO_PIN_4 | GPIO_PIN_5;
    HAL_GPIO_Init(GPIOC, &cfg);
    cfg.Pin  = GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6
             | GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9;
    HAL_GPIO_Init(GPIOF, &cfg);

    // ---- Rank sequences from the generated (unit, channel) map ------------
    // ADC1 ranks 1-12 = AV1-12, ranks 13-16 = AT1-4; ADC3 ranks 1-4 = AV13-16.
    // Buffer index == rank-1, which is what the filtered-word mapping below relies on.
    for (uint8_t r = 0; r < ADC1_AV_COUNT; ++r)      s_adc1_ranks[r] = BOARD_AV_CHANS[r].channel;
    for (uint8_t r = 0; r < BOARD_ANALOG_T_COUNT; ++r)
        s_adc1_ranks[ADC1_AV_COUNT + r] = BOARD_AT_CHANS[r].channel;
    for (uint8_t r = 0; r < ADC3_NCH; ++r)           s_adc3_ranks[r] = BOARD_AV_CHANS[ADC1_AV_COUNT + r].channel;
    for (uint8_t r = 0; r < BOARD_KNOCK_COUNT; ++r)  s_knock_ranks[r] = BOARD_KNOCK_CHANS[r].channel;

    // ---- The two units --------------------------------------------------
    // Streams per RM0410 Table 28; DMA2 Stream5/3/2 are the H-bridge DMA-PWM's, so the ADCs
    // take Stream4 (ADC1, ch0) and Stream1 (ADC3, ch2).
    s_adc1.init({ADC1, DMA2_Stream4, DMA_CHANNEL_0, DMA2_Stream4_IRQn,
                 s_adc1_ranks, ADC1_NCH, ADC_SAMPLE_TIME, ADC_OVERSAMPLE,
                 s_adc1_ring, s_adc1_filt});
    s_adc3.init({ADC3, DMA2_Stream1, DMA_CHANNEL_2, DMA2_Stream1_IRQn,
                 s_adc3_ranks, ADC3_NCH, ADC_SAMPLE_TIME, ADC_OVERSAMPLE,
                 s_adc3_ring, s_adc3_filt});
    s_adc1.start_scan();
    s_adc3.start_scan();

    // ---- Environmental sensor bus ------------------------------------------
    // LPS22HBTR on I2C1 (PB6=SCL, PB7=SDA, AF4); 1 kΩ pull-ups already on board.
    // Timing word 0x40912732 is 100 kHz at the 54 MHz APB1 this clock tree gives.
    // The part is IDENTIFIED and configured by the driver on its first service() —
    // this used to be a blind CTRL_REG1 write here, which could not tell an absent
    // sensor from a present one.
    s_baro_bus.init({BOARD_BARO_I2C_INSTANCE, GPIOB, GPIO_PIN_6, GPIO_PIN_7,
                     GPIO_AF4_I2C1, 0x40912732u, 10u});
    s_baro.init({&s_baro_bus, BOARD_BARO_I2C_ADDR, 100u /*ms period*/, 500u /*ms stale*/});

    // ---- Output GPIOs -------------------------------------------------------
    // IGN/LS (the firing output pool) are NOT pre-configured: they stay in reset
    // Hi-Z and are switched to driven push-pull only when the PinArbiter binds them
    // to a coil/injector (or another module claims them). An unclaimed pool pin
    // never drives a line. (GPIO clocks are already enabled above, so the later
    // enable_output() works.) HS/HBRIDGE/LED below are not pooled — init as before.
    gpio_init_outputs(HS_PINS,   8, GPIO_PIN_RESET);
    gpio_init_outputs(HBRIDGE_DIS_PINS, 2, GPIO_PIN_SET);   // HIGH = disabled
    gpio_init_outputs(HBRIDGE_DIR_PINS, 2, GPIO_PIN_RESET);
    gpio_init_outputs(LED_PINS, BOARD_LED_COUNT, GPIO_PIN_RESET);   // LOW = off (active HIGH)

    // HBRIDGE PWM pins — GPIO direction/enable seam only. The PWM carrier itself is driven by
    // Stm32DmaPwm (TIM1-triggered DMA to GPIOD->BSRR), configured separately in the board profile.
    gpio_init_outputs(BOARD_HBRIDGE_PWM_PINS, 2, GPIO_PIN_RESET);

    // ---- Input GPIOs -------------------------------------------------------
    // The digital-input pool, pull-down (active-high signals from 74HC2G17GW). Initialised PIN BY
    // PIN from the generated table rather than as one port mask: the pool is contiguous on GPIOD
    // here, and a mask silently encodes that. Another board spreads DIG across ports.
    cfg.Mode  = GPIO_MODE_INPUT;
    cfg.Pull  = GPIO_PULLDOWN;
    cfg.Speed = GPIO_SPEED_FREQ_LOW;
    for (const auto& d : BOARD_DIG_PINS) { cfg.Pin = d.pin; HAL_GPIO_Init(d.port, &cfg); }

    // Power-good monitors: PB8/PB9, no pull (open-drain, external pull to VDD)
    cfg.Mode  = GPIO_MODE_INPUT;
    cfg.Pull  = GPIO_NOPULL;
    cfg.Pin   = BOARD_PG_5V1_PIN | BOARD_PG_5V2_PIN;
    HAL_GPIO_Init(GPIOB, &cfg);

    // VR trigger inputs: PE0/PE1, pull-down
    cfg.Mode  = GPIO_MODE_INPUT;
    cfg.Pull  = GPIO_PULLDOWN;
    cfg.Pin   = BOARD_VR1_PIN | BOARD_VR2_PIN;
    HAL_GPIO_Init(GPIOE, &cfg);
}

// ---------------------------------------------------------------------------
// ADC reads
// ---------------------------------------------------------------------------

uint16_t board_read_adc_voltage(uint8_t index) {
    if (index >= BOARD_ANALOG_V_COUNT) return 0xFFFFu;
    if (index < ADC1_AV_COUNT) return s_adc1_filt[index];        // AV1-12: ADC1 ranks 1-12

    // AV13-16 share ADC3 with the knock burst. The burst hands the converter straight back, so in
    // normal running a fold lands every ~0.2 ms and this check never bites. It exists for the case
    // that is otherwise invisible: an ADC3 that never came back (a wedged knock worker, a DMA that
    // never completed) leaves these four holding a stale but entirely plausible count.
    // Out-of-range is how this HAL says "no reading" — the sensor pipeline fails the channel and
    // raises its own code, which a tuner can see, rather than a number that quietly stopped moving.
    if (static_cast<uint32_t>(HAL_GetTick() - s_adc3.last_fold_ms()) > ADC3_AV_STALE_MS)
        return 0xFFFFu;
    return s_adc3_filt[index - ADC1_AV_COUNT];                   // AV13-16: ADC3 ranks 1-4
}

uint16_t board_read_adc_temp(uint8_t index) {
    if (index >= BOARD_ANALOG_T_COUNT) return 0xFFFFu;
    return s_adc1_filt[ADC1_AV_COUNT + index];                   // AT1-4: ADC1 ranks 13-16
}

float board_knock_sample_rate(void) {
    return 27000000.0f / 96.0f;   // ~281.25 kHz (verify against the bench clock tree)
}

// The knock burst, on whichever converter this board puts the knock pins on. Both entry points are
// the unit's, so the borrow-and-return protocol is one implementation rather than one per board.
uint16_t board_knock_capture(uint8_t index, uint16_t* buf, uint16_t count) {
    if (index >= BOARD_KNOCK_COUNT) return 0u;
    return s_adc3.burst_blocking(s_knock_ranks[index], KNOCK_BURST_SAMPLE_TIME, buf, count);
}

void board_knock_register_complete(void (*cb)()) { s_adc3.register_burst_complete(cb); }

bool board_knock_start_burst(uint8_t index, uint16_t* buf, uint16_t count) {
    if (index >= BOARD_KNOCK_COUNT) return false;
    return s_adc3.burst_start(s_knock_ranks[index], KNOCK_BURST_SAMPLE_TIME, buf, count);
}

// Give the converter back to the AV13-16 scan. Called by the burst's owner once the samples are
// its own — the DMA has finished writing them, so the DSP does not need the ADC held while it runs.
// Idempotent, so the timeout path can call it without knowing whether the burst ever completed.
//
// This is the whole of "knock and AV13-16 coexist": the converter is borrowed for the length of a
// burst (~0.7 ms at 6000 rpm, 7.3 ms at idle) and returned, instead of being kept for as long as
// knock is enabled. What cannot be fixed here is the arithmetic — a twelve-cylinder at high rpm
// leaves little idle time between bursts, and AV13-16 then update slowly; the unit's fold stamp is
// what makes that visible instead of silent.
void board_knock_end_burst(void) { s_adc3.burst_end(); }

// ---------------------------------------------------------------------------
// Digital inputs
// ---------------------------------------------------------------------------

bool board_read_digital(uint8_t index) {
    if (index >= BOARD_DIGITAL_IN_COUNT) return false;
    const BoardExtiPin& d = BOARD_DIG_PINS[index];
    return HAL_GPIO_ReadPin(d.port, d.pin) == GPIO_PIN_SET;
}

// Frequency capture enable. The frequency pool maps 1:1 onto DIG1-8 (index 0-7 =
// PD8-PD15 = EXTI lines 8-15), the same pins board_read_digital() polls. Reconfigure
// the pin for rising-edge EXTI and route the line straight to its frequency counter
// (Stm32Capture.cpp's priority-1 ISR then does only platform_freq_on_edge per edge).
// Idempotent. NOTE: the tune's pin arbiter must keep a frequency source off a pin the
// trigger owns — binding here would otherwise steer that line away from the decoder.
extern "C" void Stm32Capture_BindFreqLine(uint8_t line, uint8_t freq_idx);

// Pins switched to BOTH-edge pulse capture. A both-edge counter ALSO yields the
// frequency, so a Frequency sensor sharing a pin with a Pulse-Width sensor (the
// flex case: ethanol = freq, fuel temp = pulse width) defers to the pulse counter
// instead of re-configuring the pin to rising-only.
static uint16_t s_pulse_pins = 0;
// Capture-enabled DIG pins by sensor-path MODE, file-scope so platform_capture_disable can tear the
// right one down. (Trigger inputs enable via Stm32CaptureChannel, NOT these — so a pin absent from all
// three masks is a trigger/unused pin the disable must leave alone.)
static uint16_t s_freq_on = 0;
static uint16_t s_sent_on = 0;

extern "C" bool platform_freq_enable(uint8_t pin) {
    if (pin >= BOARD_DIGITAL_IN_COUNT) return false;                          // freq pool = DIG1-8
    if (s_pulse_pins & (1u << pin)) return true;          // pulse capture already gives us the period
    if (s_freq_on & (1u << pin)) return true;             // already capturing
    const BoardExtiPin& d = BOARD_DIG_PINS[pin];          // port/bit/line all DECLARED by the schema
    const uint8_t line = d.line;
    GPIO_InitTypeDef g = {};
    g.Pin   = d.pin;
    g.Pull  = GPIO_PULLDOWN;                              // 74HC2G17 buffered; defined low if open
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Mode  = GPIO_MODE_IT_RISING;                        // one edge per cycle -> period = 1/f
    HAL_GPIO_Init(d.port, &g);                            // EXTICR + RTSR + unmask IMR
    Stm32Capture_BindFreqLine(line, pin);                 // line -> g_freq[pin]
    s_freq_on = static_cast<uint16_t>(s_freq_on | (1u << pin));
    return true;
}

// SENT capture enable. Same DIG1-8 pool, but SENT data is carried in the time
// between FALLING edges (J2716), so configure falling-edge EXTI and route the line
// to the SENT decoder's ring. Idempotent. Same pin-arbiter caveat as frequency.
extern "C" void Stm32Capture_BindSentLine(uint8_t line, uint8_t sent_idx);

extern "C" bool platform_sent_enable(uint8_t pin) {
    if (pin >= BOARD_DIGITAL_IN_COUNT) return false;                          // SENT pool = DIG1-8
    if (s_sent_on & (1u << pin)) return true;
    const BoardExtiPin& d = BOARD_DIG_PINS[pin];
    const uint8_t line = d.line;
    GPIO_InitTypeDef g = {};
    g.Pin   = d.pin;
    g.Pull  = GPIO_PULLUP;                                // SENT idles high; falling edges mark nibbles
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Mode  = GPIO_MODE_IT_FALLING;
    HAL_GPIO_Init(d.port, &g);
    Stm32Capture_BindSentLine(line, pin);                 // line -> g_sent[pin]
    s_sent_on = static_cast<uint16_t>(s_sent_on | (1u << pin));
    return true;
}

// Pulse-width / duty capture enable. BOTH edges on DIG(pin+1) so the ISR can time
// the high portion (rising→falling) as well as the period. Idempotent. Same pin-
// arbiter caveat as frequency.
extern "C" void Stm32Capture_BindPulseLine(uint8_t line, uint8_t pulse_idx);

extern "C" bool platform_pulse_enable(uint8_t pin) {
    if (pin >= BOARD_DIGITAL_IN_COUNT) return false;       // DIG1-8
    if (s_pulse_pins & (1u << pin)) return true;
    const BoardExtiPin& d = BOARD_DIG_PINS[pin];
    const uint8_t line = d.line;
    GPIO_InitTypeDef g = {};
    g.Pin   = d.pin;
    g.Pull  = GPIO_PULLDOWN;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Mode  = GPIO_MODE_IT_RISING_FALLING;                // capture both edges for high-time
    HAL_GPIO_Init(d.port, &g);
    Stm32Capture_BindPulseLine(line, pin);                // line -> g_pulse[pin]
    Stm32Capture_BindFreqLine(line, 0xFFu);               // pulse supersedes any freq route on this line
    s_pulse_pins = static_cast<uint16_t>(s_pulse_pins | (1u << pin));
    return true;
}

// Tear down a DIG pin's sensor-path capture when it is no longer assigned to any freq/pulse/SENT sensor
// — the symmetric half the acquire enables lacked, so a leaked EXTI can't keep firing its handler (and
// stealing decode/scheduler cycles) until the next reset. Guarded on the sensor-path masks: a pin that
// was never enabled here (a trigger input on the SAME DIG pool, or an unused pin) is left untouched.
// Idempotent. DeInit masks the EXTI IMR and resets the pin; the pending-flag clear drops any latched
// edge so a later re-enable can't fire a phantom.
extern "C" bool platform_capture_disable(uint8_t pin) {
    if (pin >= BOARD_DIGITAL_IN_COUNT) return false;
    const uint16_t bit = static_cast<uint16_t>(1u << pin);
    if (!((s_freq_on | s_sent_on | s_pulse_pins) & bit)) return false;   // not ours -> leave it
    const BoardExtiPin& d = BOARD_DIG_PINS[pin];
    const uint8_t line = d.line;
    HAL_GPIO_DeInit(d.port, d.pin);                                      // clears EXTICR/RTSR/FTSR + masks IMR
    __HAL_GPIO_EXTI_CLEAR_IT(d.pin);                                     // drop any pending edge latched while live
    Stm32Capture_BindFreqLine(line, 0xFFu);
    Stm32Capture_BindPulseLine(line, 0xFFu);
    Stm32Capture_BindSentLine(line, 0xFFu);
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
    HAL_GPIO_WritePin(LS_PINS[index].port, LS_PINS[index].pin,
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return true;
}

bool board_set_hs(uint8_t index, bool on) {
    if (index >= BOARD_HIGH_SIDE_COUNT) return false;
    HAL_GPIO_WritePin(HS_PINS[index].port, HS_PINS[index].pin,
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return true;
}

bool board_set_ign(uint8_t index, bool on) {
    if (index >= BOARD_IGNITION_COUNT) return false;
    HAL_GPIO_WritePin(IGN_PINS[index].port, IGN_PINS[index].pin,
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return true;
}

bool board_hbridge_set_enable(uint8_t hbridge_index, bool enabled) {
    if (hbridge_index >= BOARD_HBRIDGE_COUNT) return false;
    // DIS LOW = bridge enabled; HIGH = disabled
    HAL_GPIO_WritePin(HBRIDGE_DIS_PINS[hbridge_index].port, HBRIDGE_DIS_PINS[hbridge_index].pin,
                      enabled ? GPIO_PIN_RESET : GPIO_PIN_SET);
    return true;
}

bool board_hbridge_set_direction(uint8_t hbridge_index, bool forward) {
    if (hbridge_index >= BOARD_HBRIDGE_COUNT) return false;
    HAL_GPIO_WritePin(HBRIDGE_DIR_PINS[hbridge_index].port, HBRIDGE_DIR_PINS[hbridge_index].pin,
                      forward ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return true;
}

void board_set_led(uint8_t index, bool on) {
    if (index >= BOARD_LED_COUNT) return;
    // LEDs are active HIGH on Jaytek V1 (verified against jaytek bootloader)
    HAL_GPIO_WritePin(LED_PINS[index].port, LED_PINS[index].pin,
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

bool board_read_power_good(uint8_t index) {
    if (index >= BOARD_PG_COUNT) return false;
    return HAL_GPIO_ReadPin(PG_PINS[index].port, PG_PINS[index].pin) == GPIO_PIN_SET;
}

// ---------------------------------------------------------------------------
// Barometric / ambient sensor
//
// THE ENGINE FRAME NEVER BLOCKS ON A BUS. These used to do a synchronous I2C read (10 ms
// timeout) from inside the 1 kHz frame, hidden behind a 100 ms cache so 99 calls in 100 were
// free — and the 100th cost ~505 us against a 1000 us budget, on a frame that normally runs in
// 180 (measured as `worst_sensor=idx99 @505us`). A per-call average is the wrong number to
// design to; the worst case is the only one the frame actually has to survive.
//
// So there is exactly ONE owner of the bus — board_baro_service(), called from a background
// task — and the readers below are pure cache reads that cannot reach I2C at all.
// I2C1 carries only this device, so a single owner needs no mutex.
// ---------------------------------------------------------------------------

void  board_baro_service()     { s_baro.service(HAL_GetTick()); }
float board_read_baro_kpa()    { return s_baro.kpa(); }
float board_read_baro_temp_c() { return s_baro.temp_c(); }
bool  board_baro_valid()       { return s_baro.valid(HAL_GetTick()); }
