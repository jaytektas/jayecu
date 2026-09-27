// The host learned region — ONE definition, linked by any test that needs one.
//
// Six tests each carried an identical copy of this: a buffer plus platform_learned_base /
// platform_learned_persistent. Any module that reads a learned table drags those symbols in, and a
// sixth copy appeared the moment learned tables became ordinary tables (their TableDesc points into
// this region), which is where a per-test copy stops being a shortcut and starts being the pattern.
//
// It is deliberately NOT in platform_hal_stub.cpp. These tests define their own platform_get_tick_ms
// because they drive the clock by hand, so linking the whole platform stub collides. A test wants the
// learned region without adopting a clock it is trying to control — so that is exactly what this is.
#include <cstdint>
#include "../../../generated/learned_layout.h"

static uint8_t g_learned_region[LEARNED_REGION_USED + 64];

extern "C" void* platform_learned_base(uint32_t* cap) {
    if (cap) *cap = sizeof(g_learned_region);
    return g_learned_region;
}
extern "C" bool platform_learned_persistent() { return false; }   // host: SD provides persistence

// Zero it — what each test called "reset the region", against a buffer it owned.
void platform_learned_reset() { for (auto& b : g_learned_region) b = 0; }
