#pragma once

// ---------------------------------------------------------------------------
// Proteus (F7) board pin / peripheral assignments — only the pins board_hal
// configures DIRECTLY. Everything with a functional role (IGN/LS/HS/AV/AT/DIG/
// VR/LED/HBRIDGE/knock) comes from the generated map, proteus_f7_pins.h.
//
// Pins follow the Proteus open-hardware design; see the note at the top of
// definition/boards/proteus_f7.board.yaml. NOT verified against hardware.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// SD card — SPI3 on PC10 (SCK) / PC11 (MISO) / PC12 (MOSI), CS = PD2.
// DIFFERENT PINS FROM jaytek_v1,
// which uses PB3/PB4/PB5 + PA15 — and unlike jaytek these are not JTAG pins, so
// nothing has to be released from a debug function first.
//
// board_init() hands these to the SD driver via SdCard_Configure(), so the driver no
// longer carries another board's chip select.
// ---------------------------------------------------------------------------
#define BOARD_SD_SPI_INSTANCE    SPI3
#define BOARD_SD_CS_GPIO_PORT    GPIOD
#define BOARD_SD_CS_GPIO_PIN     GPIO_PIN_2

// ---------------------------------------------------------------------------
// Environmental sensor — LPS25HB on I2C2 (PB10 = SCL, PB11 = SDA, AF4).
// A DIFFERENT PART on a DIFFERENT BUS from jaytek's LPS22HB/I2C1; Lps2xBaro
// identifies which by probing WHO_AM_I, so this file only says where the bus is.
// ---------------------------------------------------------------------------
#define BOARD_BARO_I2C_INSTANCE  I2C2
#define BOARD_BARO_I2C_ADDR      (0x5Cu << 1u)   // 7-bit 0x5C for both parts

// ---------------------------------------------------------------------------
// VR trigger inputs — PE7 / PE8 (EXTI 7 / 8). Part of the capture pool, so the
// generated map owns the routing; these are here for the GPIO mode init only.
// ---------------------------------------------------------------------------
#define BOARD_VR1_PORT           GPIOE
#define BOARD_VR1_PIN            GPIO_PIN_7
#define BOARD_VR2_PORT           GPIOE
#define BOARD_VR2_PIN            GPIO_PIN_8
