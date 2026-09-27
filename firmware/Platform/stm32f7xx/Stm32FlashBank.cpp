#include "Stm32FlashBank.h"
#include "stm32f7xx_hal.h"
#include "../../Scheduler/Log.h"
#include "../platform_hal.h"   // platform_watchdog_refresh (pet before the CPU-stalling erase)
#include <cstring>

Stm32FlashBank::Stm32FlashBank(uint32_t sector, uint32_t base_addr, uint32_t size_bytes)
    : sector_(sector), base_(base_addr), size_(size_bytes) {}

bool Stm32FlashBank::erase() {
    EFI_LOG_DEBUG("flash", "erase sec=%lu base=0x%08lx: unlock",
                  (unsigned long)sector_, (unsigned long)base_);
    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase{};
    erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase.Sector       = sector_;
    erase.NbSectors    = 1;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;  // 2.7–3.6 V, 32-bit access

    // The HAL_FLASHEx_Erase below busy-waits while the flash bus is stalled.
    // If this is the last line seen before a hang, the CPU faulted fetching an
    // ISR/PendSV handler from a stalled flash bank during the erase.
    EFI_LOG_DEBUG("flash", "erase sec=%lu: HAL_FLASHEx_Erase begin", (unsigned long)sector_);
    // Pet the IWDG immediately before the erase: HAL_FLASHEx_Erase stalls the CPU (single-bank flash,
    // all fetch blocked) for ~1-2 s, longer than the watchdog task's pet cadence. Petting here gives the
    // erase a full timeout window so a legitimate burn can't trip the watchdog mid-erase.
    platform_watchdog_refresh();
    uint32_t error = 0;
    const bool ok = (HAL_FLASHEx_Erase(&erase, &error) == HAL_OK);
    EFI_LOG_DEBUG("flash", "erase sec=%lu: end ok=%d err=0x%lx",
                  (unsigned long)sector_, (int)ok, (unsigned long)error);

    HAL_FLASH_Lock();
    return ok;
}

bool Stm32FlashBank::program(uint32_t offset, const uint8_t* data, uint32_t len) {
    if (offset + len > size_) return false;

    EFI_LOG_DEBUG("flash", "program off=%lu len=%lu: begin",
                  (unsigned long)offset, (unsigned long)len);
    HAL_FLASH_Unlock();

    bool ok = true;
    uint32_t addr = base_ + offset;

    // Program in 32-bit words for speed (FLASH_TYPEPROGRAM_WORD).
    // Pad the final partial word with 0xFF (erased flash value) to avoid
    // turning any bit back to 1 — that would require another erase.
    uint32_t i = 0;
    for (; i + 4 <= len; i += 4, addr += 4) {
        uint32_t word;
        memcpy(&word, data + i, 4);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, word) != HAL_OK) {
            ok = false;
            break;
        }
    }
    // Remaining bytes (0–3)
    if (ok && i < len) {
        uint32_t word = 0xFFFF'FFFFu;
        memcpy(&word, data + i, len - i);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, word) != HAL_OK) {
            ok = false;
        }
    }

    HAL_FLASH_Lock();
    EFI_LOG_DEBUG("flash", "program off=%lu: end ok=%d", (unsigned long)offset, (int)ok);
    return ok;
}

bool Stm32FlashBank::read(uint32_t offset, uint8_t* out, uint32_t len) const {
    if (offset + len > size_) return false;
    // STM32 internal flash is memory-mapped — a direct memcpy is all we need.
    memcpy(out, reinterpret_cast<const uint8_t*>(base_ + offset), len);
    return true;
}
