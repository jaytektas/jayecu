// CpuStats — see the header.

#include "CpuStats.h"

#include <string.h>

#if defined(JAYECU_FIRMWARE)
#include "FreeRTOS.h"
#include "task.h"
#endif

namespace cpustats {
namespace {

TaskLoad s_out[MAX_TASKS];
uint8_t  s_n    = 0;
uint8_t  s_load = 0;

#if defined(JAYECU_FIRMWARE)
// The previous window's totals, kept by task HANDLE — a name is not an identity (two tasks may share
// one) and the array order is whatever the kernel walked.
struct Prev { TaskHandle_t h; uint32_t run; };
Prev     s_prev[MAX_TASKS];
uint8_t  s_prev_n = 0;
uint32_t s_prev_total = 0;

uint32_t prev_of(TaskHandle_t h) {
    for (uint8_t i = 0; i < s_prev_n; i++) if (s_prev[i].h == h) return s_prev[i].run;
    return 0;                       // a task that did not exist last window: its whole total is new
}
#endif

}  // namespace

#if defined(JAYECU_FIRMWARE)

void sample() {
    static TaskStatus_t st[MAX_TASKS];          // static: ~60 bytes a task, and this runs on a 512-word stack
    uint32_t total = 0;
    const UBaseType_t n = uxTaskGetSystemState(st, MAX_TASKS, &total);
    if (n == 0) return;                          // more tasks than MAX_TASKS: the kernel returns 0

    const uint32_t d_total = total - s_prev_total;
    uint8_t out_n = 0;
    uint8_t idle_pct = 0;
    for (UBaseType_t i = 0; i < n && out_n < MAX_TASKS; i++) {
        const uint32_t d = st[i].ulRunTimeCounter - prev_of(st[i].xHandle);
        const uint8_t pct = d_total ? static_cast<uint8_t>((d * 100u + d_total / 2u) / d_total) : 0;
        // The idle task is the HEADROOM, not a load — it is reported as the one number everybody wants
        // and left out of the per-task list, where it would always be the biggest row and mean nothing.
        if (st[i].uxCurrentPriority == tskIDLE_PRIORITY && st[i].pcTaskName[0] == 'I') { idle_pct = pct; continue; }
        TaskLoad& t = s_out[out_n++];
        strncpy(t.name, st[i].pcTaskName, sizeof(t.name) - 1);
        t.name[sizeof(t.name) - 1] = '\0';
        t.pct        = pct;
        t.stack_free = st[i].usStackHighWaterMark;
    }
    // Heaviest first: the answer to "what is eating it" should be the first row, not somewhere in ten.
    for (uint8_t a = 1; a < out_n; a++) {
        const TaskLoad key = s_out[a];
        int8_t b = static_cast<int8_t>(a) - 1;
        for (; b >= 0 && s_out[b].pct < key.pct; b--) s_out[b + 1] = s_out[b];
        s_out[b + 1] = key;
    }
    s_n = out_n;
    // FIRST CALL SEEDS AND REPORTS NOTHING. Its "window" is everything since boot, which is a different
    // question and a misleading answer — an ECU that was busy starting up would read busy for ever.
    s_load = s_prev_total ? static_cast<uint8_t>(idle_pct >= 100 ? 0 : 100 - idle_pct) : 0;

    s_prev_n = 0;
    for (UBaseType_t i = 0; i < n && s_prev_n < MAX_TASKS; i++)
        s_prev[s_prev_n++] = { st[i].xHandle, st[i].ulRunTimeCounter };
    s_prev_total = total;
}

#else   // host build: no kernel, no counters, and nothing to invent

void sample() { s_n = 0; s_load = 0; }

#endif

uint8_t load_pct()   { return s_load; }
uint8_t task_count() { return s_n; }
const TaskLoad& task(uint8_t i) {
    static const TaskLoad none{};
    return i < s_n ? s_out[i] : none;
}

}  // namespace cpustats
