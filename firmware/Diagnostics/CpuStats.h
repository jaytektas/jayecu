#pragma once

// CpuStats — WHO IS USING THE MCU, sampled from the kernel's own run-time counters.
//
// FreeRTOS accumulates, per task, how long it has held the CPU (portGET_RUN_TIME_COUNTER_VALUE, which
// on this board is the microsecond timebase). This turns two of those snapshots into the number a
// person wants: the share of the last window each task took, and the share nobody took — the idle
// task — which is the headroom.
//
// DELTAS, NOT TOTALS. A total answers "since boot", which is useless the moment anything transient
// happens; the window is what says the ECU is busy NOW. Unsigned subtraction carries the wrap.
//
// This is not the same question as EngineTask::frame_load_pct(), and neither answers the other. This
// one says how much of the chip is spoken for; that one says how much of the 1 kHz frame the engine
// loop itself used, which is what decides whether a frame is late. A chip at 40 % can be one frame
// away from missing spark, and a chip at 90 % can be perfectly safe if the 10 % is all engine.

#include <stdint.h>

namespace cpustats {

constexpr uint8_t MAX_TASKS = 16;

struct TaskLoad {
    char     name[16];
    uint8_t  pct;          // share of the last window
    uint16_t stack_free;   // words still unused at the deepest point this task has ever reached
};

// Take a window. Call it about once a second: the first call only seeds the baseline.
void sample();

uint8_t load_pct();            // 100 - idle's share, over the last window
uint8_t task_count();
const TaskLoad& task(uint8_t i);   // sorted heaviest-first; index < task_count()

}  // namespace cpustats
