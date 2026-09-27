#pragma once

// ReconfigCost — WHAT THE LAST TUNE WRITE COST, and which rebuild spent it.
//
// A config write rebuilds the things that read the bytes that moved, and it does it INSIDE the 1 kHz
// engine frame. Measured on the bench at 3.6 ms for one scalar — three frames' worth — which the frame
// peak reports and cannot explain. This is the explanation: one microsecond figure per rebuilder,
// stamped where the work happens, printed by `cpu`.
//
// Free when nothing is rebuilding: a DWT read either side of work that only runs on a config change.

#include <stdint.h>

namespace reconfig {

enum Part : uint8_t { SENSORS = 0, OUTPUTS = 1, FRAME = 2, PART_COUNT = 3 };

void  note(Part p, uint32_t cycles);   // record what that rebuild just took
// A NEW RECONFIG STARTS HERE. Without it the parts that did nothing keep last time's figure, and a
// rebuild that was correctly SKIPPED reads as if it had run — which is exactly the claim this whole
// instrument exists to check.
void  begin();
uint32_t us(Part p);                   // …in microseconds, most recent
const char* name(Part p);

// RAII: time a rebuild by declaring one at the top of it.
struct Time {
    Part     p;
    uint32_t t0;
    explicit Time(Part part);
    ~Time();
};

}  // namespace reconfig
