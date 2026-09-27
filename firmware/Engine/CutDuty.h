#pragma once
#include <cstdint>

// CutDuty — a cut PERCENTAGE, delivered over time.
//
// wk::ign_cut and wk::fuel_cut are booleans: all cylinders or none. So the only percentage available
// without a second cut-mask producer in the scheduler is a duty — cut on some frames and not others —
// and this is the one place that decides which frames.
//
// SPREAD, NOT BURSTS. The obvious version is a threshold ("cut while pct > 50"), which is not a
// percentage at all: it is nothing below the threshold and everything above it, which is a second hard
// cut wearing a soft cut's name. The accumulator carries the remainder, so 30% cuts roughly every third
// frame rather than a third of the time in one lump — the difference between a limiter that bounces and
// one that buzzes.
//
// AND THE TTL IS THE PULSE WIDTH. A cut is released by its signal EXPIRING (EngineTask reads
// bus.valid(), so validity is the OR and publishing `false` would still read as a cut). ttl() is the
// module's period PLUS grace, which is right for freshness and wrong here: at a 5 ms cadence it is 9 ms,
// so one frame's cut would last two frames and every duty would come out roughly double. Publish a cut
// frame with EngineModule::frame_ms() instead, so one decision is one frame.
struct CutDuty {
    float acc = 0.0f;

    // True on the frames that should cut, for a duty of `pct` (0..100).
    bool step(float pct) {
        if (pct <= 0.0f)   { acc = 0.0f; return false; }
        if (pct >= 100.0f) { acc = 0.0f; return true; }
        acc += pct;
        if (acc >= 100.0f) { acc -= 100.0f; return true; }
        return false;
    }

    void reset() { acc = 0.0f; }
};
