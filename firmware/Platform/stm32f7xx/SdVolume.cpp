#include "SdVolume.h"
#include "SdCardSpi.h"
#include "ff.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

static FATFS    s_fs;
static bool     s_mounted = false;
static uint32_t s_epoch   = 0;

bool sd_volume_ensure(SdArbitrator& arb) {
    if (!arb.ecu_has_card()) return false;

    // Fast path, no lock: mounted, and the card has not been to the PC since.
    if (s_mounted && s_epoch == arb.ecu_epoch() && SdCard_IsAvailable()) return true;

    // Slow path: (re)mount. Serialised — f_mount from two tasks at once is not safe. Before the
    // scheduler runs (boot's config read) there is only one caller, and no mutex to take.
    static StaticSemaphore_t s_lock_buf;
    static SemaphoreHandle_t s_lock = nullptr;
    const bool rtos = xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
    if (rtos) {
        taskENTER_CRITICAL();
        if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
        taskEXIT_CRITICAL();
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
    }

    const uint32_t epoch = arb.ecu_epoch();
    bool ok = s_mounted && s_epoch == epoch && SdCard_IsAvailable();
    if (!ok) {
        if (s_mounted && s_epoch != epoch) SdCard_Invalidate();   // the PC had it: re-probe the card too
        SdCard_Init();                                             // idempotent unless invalidated
        f_mount(nullptr, "0:", 0);
        s_mounted = SdCard_IsAvailable() && f_mount(&s_fs, "0:", 1) == FR_OK;
        s_epoch   = epoch;
        ok = s_mounted;
    }

    if (rtos) xSemaphoreGive(s_lock);
    return ok;
}
