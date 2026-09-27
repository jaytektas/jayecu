#include "ff.h"
#include "diskio.h"
#include "SdCardSpi.h"
#include "stm32f7xx_hal.h"
#include <stdbool.h>

// Provide a weak default for get_fattime — override if RTC is available.
__attribute__((weak)) DWORD get_fattime(void) { return 0; }

// ---------------------------------------------------------------------------
// FatFS disk I/O bridge — physical drive 0 = SD card on SPI.
// All calls guard on SdArbitrator so the SD is never accessed while USB owns it.
// ---------------------------------------------------------------------------

// C shim: SdArbitrator is C++ — expose the ECU-owns check via a thin extern.
// Defined in SdArbitratorShim.cpp.
extern bool SdArbitrator_EcuHasCard(void);

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    if (!SdArbitrator_EcuHasCard()) return STA_NOINIT;
    return SdCard_IsAvailable() ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    if (!SdArbitrator_EcuHasCard()) return STA_NOINIT;
    return SdCard_Init() ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE* buf, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (!SdArbitrator_EcuHasCard()) return RES_NOTRDY;
    return SdCard_Read(buf, sector, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buf, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (!SdArbitrator_EcuHasCard()) return RES_NOTRDY;
    return SdCard_Write(buf, sector, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buf) {
    if (pdrv != 0) return RES_PARERR;
    if (!SdArbitrator_EcuHasCard()) return RES_NOTRDY;

    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            *(DWORD*)buf = SdCard_GetSectorCount();
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD*)buf = 512;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *(DWORD*)buf = 1;
            return RES_OK;
        default:
            return RES_PARERR;
    }
}
