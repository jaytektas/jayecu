#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "stm32f7xx_hal.h"   // SPI_TypeDef / GPIO_TypeDef for SdCard_Configure

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// SPI SD card low-level driver.
// Targets the SPI peripheral and CS pin defined in platform_config.h.
//
// Mutex note: uses a FreeRTOS static mutex when the scheduler is running.
// ISR callers (USB MSC diskio) bypass the mutex — safe because SdArbitrator
// prevents concurrent ECU/USB ownership.
// ---------------------------------------------------------------------------

// WHICH SPI and WHICH CS PIN — a BOARD fact, supplied by the board's board_init() before
// any other call here. jaytek_v1 uses SPI3 with CS on PA15; proteus_f7 uses SPI3 with CS on
// PD2. These were compiled into the SoC tier as jaytek's pins, so this driver would have
// toggled PA15 on a board whose chip select is PD2 — the card simply never responding, with
// the SPI traffic looking perfectly correct on a scope.
//
// The SCK/MISO/MOSI pins are NOT passed: those are alternate-function pins the board already
// configures in board_init(). Only the peripheral and the software-driven CS are needed here.
void SdCard_Configure(SPI_TypeDef* spi, GPIO_TypeDef* cs_port, uint16_t cs_pin);

void SdCard_InitMutex(void);

// Probe and initialise the card. Returns true if a card was found and typed.
// Idempotent: returns immediately if already initialised. Call SdCard_Invalidate()
// first to force a re-init (e.g. after USB MSC returns the bus).
bool SdCard_Init(void);

// Mark the card as needing re-initialisation. Call when USB MSC returns the card
// to the ECU so the next SdCard_Init() performs a full SPI re-probe.
void SdCard_Invalidate(void);

// Read/write 512-byte sectors. sector is LBA address.
bool SdCard_Read (uint8_t* buff, uint32_t sector, uint32_t count);
// Write-shape counters: how many disk_write calls, how many were single-sector vs multi, the total
// sectors, and the longest run in one call. Says whether the card is being streamed to or poked.
void SdCard_WriteStats(uint32_t* calls, uint32_t* single, uint32_t* multi,
                       uint32_t* sectors, uint32_t* max_run);
void SdCard_WriteStatsReset(void);

bool SdCard_Write(const uint8_t* buff, uint32_t sector, uint32_t count);

uint32_t SdCard_GetSectorCount(void);
bool     SdCard_IsAvailable(void);

#ifdef __cplusplus
}
#endif
