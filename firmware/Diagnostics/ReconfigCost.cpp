// ReconfigCost — see the header.

#include "ReconfigCost.h"
#if defined(JAYECU_FIRMWARE)
#include "../Platform/platform_hal.h"
#else
// Host: there is no cycle counter and no 1 kHz frame to protect, so this measures nothing and says so.
static inline uint32_t platform_cyccnt() { return 0; }
static inline uint32_t platform_cpu_hz() { return 0; }
#endif

namespace reconfig {
namespace { uint32_t s_cyc[PART_COUNT] = {}; }

void note(Part p, uint32_t cycles) { if (p < PART_COUNT) s_cyc[p] = cycles; }
void begin() { for (uint8_t i = 0; i < PART_COUNT; i++) s_cyc[i] = 0; }

uint32_t us(Part p) {
    const uint32_t hz = platform_cpu_hz();
    return (p < PART_COUNT && hz) ? s_cyc[p] / (hz / 1000000u) : 0u;
}

const char* name(Part p) {
    switch (p) {
        case SENSORS: return "sensors";
        case OUTPUTS: return "outputs";
        case FRAME:   return "whole frame";
        default:      return "?";
    }
}

Time::Time(Part part) : p(part), t0(platform_cyccnt()) {}
Time::~Time() { note(p, platform_cyccnt() - t0); }

}  // namespace reconfig
