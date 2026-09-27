// FatFS R0.15 OS-dependent helpers — FreeRTOS implementation.
// Replaces the third_party/fatfs/ffsystem.c Win32 template.

#include "ff.h"
#include "FreeRTOS.h"
#include "semphr.h"

#if FF_USE_LFN == 3
void* ff_memalloc(UINT msize) { return pvPortMalloc(msize); }
void  ff_memfree (void* p)    { vPortFree(p); }
#endif

#if FF_FS_REENTRANT

int ff_cre_syncobj(BYTE /*vol*/, FF_SYNC_t* sobj) {
    *sobj = xSemaphoreCreateMutex();
    return (*sobj != NULL) ? 1 : 0;
}

int ff_del_syncobj(FF_SYNC_t sobj) {
    vSemaphoreDelete(sobj);
    return 1;
}

int ff_req_grant(FF_SYNC_t sobj) {
    return (xSemaphoreTake(sobj, pdMS_TO_TICKS(FF_FS_TIMEOUT)) == pdTRUE) ? 1 : 0;
}

void ff_rel_grant(FF_SYNC_t sobj) {
    xSemaphoreGive(sobj);
}

#endif
