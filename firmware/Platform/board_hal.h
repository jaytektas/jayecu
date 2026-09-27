#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// Board HAL — connector-level I/O for one specific hardware board.
//
// Implemented in firmware/Platform/boards/<board>/board_hal_<board>.cpp.
// The stub in firmware/Platform/boards/stubs/board_hal_stub.cpp satisfies
// native test builds that link no hardware-specific code.
//
// IMPORTANT: engine modules must never include this header directly.
//            Use platform_hal.h (the stable C interface) instead.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Initialise all GPIO, ADC, I2C clocks and pin modes for this board.
// Must be called once from platform_init(), before any other board_hal call.
// On Jaytek V1 this also switches PB3/PB4 from JTAG default to SPI3 AF6.
void board_init();

// ---------------------------------------------------------------------------
// ADC — returns the latest 12-bit counts [0..4095] for a channel, 0xFFFF on
// invalid index. Contract is NON-BLOCKING: a board may sample however its
// silicon prefers (background DMA scan + oversample/filter, or a plain poll)
// behind these functions; callers must not assume a fresh conversion per call.
// Jaytek V1: all AV1-16 + AT1-4 are oversampled/filtered via background DMA
// scans (ADC1: AV1-12 + AT1-4; ADC3: AV13-16). ADC3 is SHARED with knock: a
// burst borrows it for the length of the burst and hands it back, so AV13-16
// stay live with knock enabled (they read 0xFFFF only if the scan genuinely
// stopped refreshing). There is NO knock poll read: a poll would collide with the
// live DMA conversion, so the knock data path is the burst, below.
// ---------------------------------------------------------------------------

// AV1-16: voltage inputs (index 0-15)
//   ADC1: PA0-PA7 (AV1-8), PB0 (AV11), PB1 (AV12), PC4 (AV9), PC5 (AV10)
//   ADC3: PF6 (AV13), PF7 (AV14), PF8 (AV15), PF9 (AV16)
uint16_t board_read_adc_voltage(uint8_t index);

// AT1-4: temperature inputs (index 0-3)
//   ADC1: PC0 (AT1), PC1 (AT2), PC2 (AT3), PC3 (AT4)
uint16_t board_read_adc_temp(uint8_t index);

// Knock burst capture: fill `buf` with up to `count` raw 12-bit samples of knock channel `index`
// (0/1) at board_knock_sample_rate(). Re-tasks ADC3 to a single-channel one-shot DMA on the knock
// pin (IN14/IN15) and restores the AV13-16 scan before returning. Blocking; returns the number of
// samples captured (0 on failure, and 0 without sampling if a non-blocking burst is already in
// flight — this is the bench path, and it never stamps on the worker). Feed the result to the DSP.
uint16_t board_knock_capture(uint8_t index, uint16_t* buf, uint16_t count);
float    board_knock_sample_rate(void);   // Hz — the rate board_knock_capture samples at

// Non-blocking knock burst (the running-engine path): arm the ADC3->DMA capture of `count` samples on
// knock channel `index` into `buf` and return immediately. On completion the callback registered via
// board_knock_register_complete() fires from the DMA-complete ISR (DMA2_Stream1, NVIC prio 8 -> FromISR
// legal) — the worker sleeps on that instead of polling. `buf` holds `count` samples once it fires.
// Returns false if the arm was rejected.
//
// THE BORROW MUST BE RETURNED. A successful arm holds ADC3, and the AV13-16 scan is stopped for as
// long as it does; the caller owns the samples the moment the completion fires (the DMA is done
// writing them), so it must call board_knock_end_burst() THEN — before the DSP, not after — and on
// the timeout path too. That call is what makes knock and AV13-16 coexist rather than exclude.
bool board_knock_start_burst(uint8_t index, uint16_t* buf, uint16_t count);
void board_knock_end_burst(void);          // release ADC3 back to the AV13-16 scan; idempotent
void board_knock_register_complete(void (*cb)(void));

// NOTE: no counts->mV here. The ECU is ADC-counts-native; the COUNTS->mV/V conversion is the client's
// job (studio, from the board params in the meta). The HAL only ever produces raw counts.

// ---------------------------------------------------------------------------
// Digital inputs — DIGITAL1-8 (PD8-PD15, index 0-7)
// Returns raw pin level; invert/debounce logic is the caller's responsibility.
// ---------------------------------------------------------------------------
bool board_read_digital(uint8_t index);

// ---------------------------------------------------------------------------
// Low-side outputs — LS1-22 (index 0-21)
// true = drive low-side FET ON (pulls load to GND).
// Returns false on invalid index.
// ---------------------------------------------------------------------------
bool board_set_ls(uint8_t index, bool on);

// ---------------------------------------------------------------------------
// High-side outputs — HS1-8 (index 0-7), VNQ7140AJTR quad switches
// true = switch ON (drives load to VBAT).
// ---------------------------------------------------------------------------
bool board_set_hs(uint8_t index, bool on);

// ---------------------------------------------------------------------------
// Ignition outputs — IGN1-12 (index 0-11), IX4427NTR non-inverting gate drivers
// true = gate driver active (coil dwell); false = coil fires (spark).
// ---------------------------------------------------------------------------
bool board_set_ign(uint8_t index, bool on);

// ---------------------------------------------------------------------------
// HBRIDGE H-bridge — IFX9201SGAUMA1 (hbridge_index 0=HBRIDGE1, 1=HBRIDGE2)
// HBRIDGE1: DIS=PD5, DIR=PD7, PWM=PD6
// HBRIDGE2: DIS=PD2, DIR=PD4, PWM=PD3
// NOTE: PD6/PD3 have NO timer alternate function on the STM32F767 (datasheet DS11532 Table 13:
// AF1=TIM1/2 and AF2=TIM3/4/5 are both '-' for both pins — confirmed against reference/stm32f767zi.pdf
// p93). So the PWM is NOT a native timer-AF PWM: Stm32DmaPwm drives it as hardware-timed PWM by
// having TIM1 trigger DMA writes of the pin edges to GPIOD->BSRR (see Stm32DmaPwm.cpp), wired via
// Jaytek1HBridge in jaytek_v1_profile.cpp. This board_hal seam handles GPIO direction/enable only.
// ---------------------------------------------------------------------------
bool board_hbridge_set_enable(uint8_t hbridge_index, bool enabled);    // DIS LOW = enabled
bool board_hbridge_set_direction(uint8_t hbridge_index, bool forward); // DIR selects rotation

// ---------------------------------------------------------------------------
// Status LEDs — active LOW (index: 0=RUNNING/PA8, 1=WARNING/PC7, 2=ERROR/PC8, 3=COMMS/PC9)
// ---------------------------------------------------------------------------
void board_set_led(uint8_t index, bool on);

// ---------------------------------------------------------------------------
// Power-good monitors (index 0=5V_SENSOR1/PB9, 1=5V_SENSOR2/PB8)
// Returns true when the supply is good (pin HIGH).
// ---------------------------------------------------------------------------
bool board_read_power_good(uint8_t index);

// ---------------------------------------------------------------------------
// Barometric sensor — LPS22HBTR via I2C1 (PB6=SCL, PB7=SDA)
// Onboard barometric pressure (kPa) from the LPS22HBTR over I2C1. Continuous 10 Hz;
// the read is cached/rate-limited (~100 ms) so it's safe to call every frame. Holds
// the last-good value (101.3 kPa at boot) if the bus NAKs.
// ---------------------------------------------------------------------------
float board_read_baro_kpa();

// Ambient/board temperature (degC) from the same LPS22HBTR (it reads P and T). Same
// 100 ms cache. Available for baro temp compensation / a board-temp diagnostic.
float board_read_baro_temp_c();
// Sample the on-board environmental sensor over I2C. THE ONLY function here that touches the bus —
// call it from a background task, never from the engine frame. The two readers above are caches.
void board_baro_service();
// False when the sampler has not produced a good reading recently (bus fault, absent part), so a
// stale cache surfaces as an invalid sensor rather than a frozen plausible value.
bool board_baro_valid();

// ---------------------------------------------------------------------------
// Device UID — the factory-programmed 96-bit unique device ID (read-only, survives
// firmware updates / flash erase). Writes 12 bytes, MSB-first. Used by the host to
// route a connected ECU to its tuning project (stable per physical chip).
// ---------------------------------------------------------------------------
void board_device_uid(uint8_t uid[12]);
