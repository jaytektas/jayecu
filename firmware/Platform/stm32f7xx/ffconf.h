// FatFS R0.15 configuration for JayECU.
// Only the options that differ from defaults are changed here.

#define FFCONF_DEF    86631

// R/W, full API
#define FF_FS_READONLY    0
#define FF_FS_MINIMIZE    0

// mkfs needed to format a blank card on first boot
#define FF_USE_MKFS       1

// Single drive, no partition table awareness needed
#define FF_VOLUMES        1
#define FF_STR_VOLUME_ID  0
#define FF_MULTI_PARTITION 0

// LFN enabled — descriptor filenames (tuneit-meta.json) and legacy panic logs need it.
// Mode 3 = stack-local buffer per call; thread-safe with FF_FS_REENTRANT, no heap.
#define FF_LFN_UNICODE    0
#define FF_USE_LFN        3

// One file object at a time per volume is fine for ECU use
#define FF_FS_LOCK        2

// RTOS sync object — use FreeRTOS mutex via ffsystem.c
// FreeRTOS.h must be included BEFORE ff.h, so the SemaphoreHandle_t typedef
// is visible by the time ff.h substitutes FF_SYNC_t.
#define FF_FS_REENTRANT   1
#define FF_FS_TIMEOUT     1000
#include "FreeRTOS.h"
#include "semphr.h"
#define FF_SYNC_t         SemaphoreHandle_t

// Word alignment for STM32 DMA compatibility
#define FF_USE_BUFF_WO    0
#define FF_MIN_SS         512
#define FF_MAX_SS         512

// exFAT disabled — not needed for small log files on FAT32
#define FF_FS_EXFAT       0

// Tiny mode off — keep full sector buffer
#define FF_FS_TINY        0

// Code page — 437 is plain ASCII-compatible
#define FF_CODE_PAGE      437

// Defaults not otherwise overridden — required by ff.c
#define FF_FS_RPATH       0   // No relative-path API
#define FF_LBA64          0   // 32-bit LBA only (single-volume, <2 TB)
#define FF_MAX_LFN        255
#define FF_LFN_BUF        255
#define FF_SFN_BUF        12
#define FF_STRF_ENCODE    3
#define FF_FS_NORTC       1   // No RTC available
#define FF_NORTC_MON      1
#define FF_NORTC_MDAY     1
#define FF_NORTC_YEAR     2026
#define FF_FS_NOFSINFO    0
#define FF_USE_FIND       0
#define FF_USE_EXPAND     0
#define FF_USE_CHMOD      0
#define FF_USE_LABEL      0
#define FF_USE_FORWARD    0
#define FF_USE_STRFUNC    0
#define FF_PRINT_LLI      0
#define FF_PRINT_FLOAT    0
#define FF_VOLUME_STRS    "RAM","NAND","CF","SD","SD2","USB","USB2","USB3"
#define FF_USE_TRIM       0
#define FF_FS_CRC         0
