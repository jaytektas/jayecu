#pragma once

// ---------------------------------------------------------------------------
// JayECU STM32F767 hardware pin/peripheral assignments.
// All board-specific defines live here — the drivers include this header.
// ---------------------------------------------------------------------------

// SD card pins are NOT here. Which SPI peripheral and which chip-select pin the card sits
// on is a BOARD fact, and this is the SoC tier: these were jaytek_v1's SPI3/PA15, so
// SdCardSpi.c would have driven PA15 on proteus_f7, whose CS is PD2. Each board now passes
// its own via SdCard_Configure() from board_init() — see SdCardSpi.h.

// Flash config banks — Sectors 10 and 11 of the 2 MB internal flash.
// Single-bank STM32F767 sector map (256 KB sectors 5..11):
//   sector 10 -> 0x0818_0000 .. 0x081B_FFFF
//   sector 11 -> 0x081C_0000 .. 0x081F_FFFF
// The base address MUST be the start of the same sector that erase() clears,
// otherwise erase and program/read target different regions: program writes
// into un-erased flash (verify fails) and the erase clobbers the neighbouring
// bank's header. Each bank owns exactly one full 256 KB sector.
#define FLASH_BANK_A_SECTOR    FLASH_SECTOR_10
#define FLASH_BANK_A_BASE      0x08180000UL
#define FLASH_BANK_A_SIZE      (256U * 1024U)

#define FLASH_BANK_B_SECTOR    FLASH_SECTOR_11
#define FLASH_BANK_B_BASE      0x081C0000UL
#define FLASH_BANK_B_SIZE      (256U * 1024U)

// SD file paths. FatFS here is FF_USE_LFN=0 → 8.3 names only (stem <= 8 chars), else
// f_open fails FR_INVALID_NAME. (This macro is currently unused — the authoritative
// default lives in SdConfigStore — but keep it 8.3-valid.) The DTC table (with its
// per-code freeze-frames) persists to dtc.bin; there is no separate fault-log file.
#define SD_CONFIG_PATH    "0:/ecucfg.bin"
