#pragma once

// ---------------------------------------------------------------------------
// Jaytek V1 board pin / peripheral assignments.
// Derived from the EasyEDA Pro schematic's netlist export (2026-05-22, not shipped).
// All GPIO port/pin values are exact — no estimates.
//
// Included only by board_hal_jaytek_v1.cpp.
// Platform-level consumers use platform_hal.h instead.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// SD card — SPI3 (PB3=SCK/AF6, PB4=MISO/AF6, PB5=MOSI/AF6), CS = PA15
// PB3 and PB4 are JTAG pins by default (TDO / TRST).  board_init() switches
// them to SPI3 AF6 via HAL_GPIO_Init; SWD (PA13/PA14) is unaffected.
// ---------------------------------------------------------------------------
#define BOARD_SD_SPI_INSTANCE    SPI3
#define BOARD_SD_CS_GPIO_PORT    GPIOA
#define BOARD_SD_CS_GPIO_PIN     GPIO_PIN_15

// ---------------------------------------------------------------------------
// Barometric sensor — LPS22HBTR via I2C1 (PB6=SCL/AF4, PB7=SDA/AF4)
// 1 kΩ pull-ups to VDD already on board.
// ---------------------------------------------------------------------------
#define BOARD_BARO_I2C_INSTANCE  I2C1
#define BOARD_BARO_I2C_ADDR      (0x5Cu << 1u)   // 7-bit 0x5C (SDO=GND, bench-probed WHO_AM_I=0xB1)

// NOTE: HBRIDGE (DIS/DIR/PWM) and status-LED pin maps are no longer defined here —
// they are generated from the board schema (jaytek_v1_pins.h: BOARD_HBRIDGE_*_PINS /
// BOARD_LED_PINS). This file keeps only the pins board_hal configures directly
// (SD/baro buses; VR/DIN/PG GPIO init below).

// ---------------------------------------------------------------------------
// 5 V sensor supply power-good monitors — TLS115D0LD open-drain (HIGH = OK)
// (also in the generated map; the _PIN masks are used for GPIO init in board_hal)
// ---------------------------------------------------------------------------
#define BOARD_PG_5V1_PORT        GPIOB
#define BOARD_PG_5V1_PIN         GPIO_PIN_9    // 5V_SENSOR1_PG
#define BOARD_PG_5V2_PORT        GPIOB
#define BOARD_PG_5V2_PIN         GPIO_PIN_8    // 5V_SENSOR2_PG

// ---------------------------------------------------------------------------
// VR trigger inputs — MAX9924UAUB+ (rising edge = zero-crossing)
// Routed to TriggerDecoder via EXTI0/EXTI1; not part of InputAssignment.
// ---------------------------------------------------------------------------
#define BOARD_VR1_PORT           GPIOE
#define BOARD_VR1_PIN            GPIO_PIN_0    // EXTI0
#define BOARD_VR2_PORT           GPIOE
#define BOARD_VR2_PIN            GPIO_PIN_1    // EXTI1

// ---------------------------------------------------------------------------
// Digital inputs — 74HC2G17GW Schmitt-trigger buffer, 3.3 V compatible.
// DIGITAL1-8 = PD8-PD15 (EXTI8-15), but the port/bit/line map is GENERATED —
// BOARD_DIG_PINS in jaytek_v1_pins.h. The old BOARD_DIN_GPIO_PORT +
// BOARD_DIN_FIRST_PIN_BIT pair encoded "one port, contiguous bits", which is a
// property of this board rather than of a digital input pool.
// ---------------------------------------------------------------------------
