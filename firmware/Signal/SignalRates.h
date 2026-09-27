#pragma once
#include <cstdint>
#include "SignalBus.h"

// SIGNAL RATE REQUIREMENTS — how fresh each bus channel has to be, decided by whoever READS it.
//
// A publisher used to pick its own rate. A sensor sampled at whatever its TYPE said — a number chosen for
// the physical quantity, so a coolant thermistor gets 20 Hz because coolant cannot change faster — with
// no reference to anyone reading it. A module's cadence came from the schema. Nothing compared the two.
//
// The ElectronicThrottle needs 500 Hz — the rate the reference implementation closes its ETB loop at. It
// closes a position loop and steps its calibration state machine on that same update. Throttle position
// is a `percent` sensor, and that type samples at 200 Hz. So the loop ran repeatedly on every sample for
// as long as the module has existed, and did so at 1 kHz, twice over again. The
// integrator merely sees a staircase; the DERIVATIVE sees nonsense — zero across the repeated frames and
// then a spike scaled by however many it waited, an impulse train where damping should be. It looked
// exactly like a throttle with enormous mechanical friction, and was chased as one for days.
//
// So the requirement belongs to the SIGNAL, and it is CLAIMED BY RUNNING CODE. A consumer stamps the rate
// it needs on the channel each time it reads it; the publisher asks the channel what is wanted. Neither
// has to know the other exists — which matters, because a channel's publisher is not always a sensor and
// a consumer has no business knowing which.
//
// Claims are made by execution, not by configuration, and that is the point. Deriving the requirement
// from the tune's selectors instead would hold a rate for a module that is switched off: the selector is
// still there, so the claim would still be counted. A module that is not running does not claim, and its
// need disappears without anyone having to remember to withdraw it.
//
// Highest claim wins, and a claim that stops being renewed stops counting. Claims accumulate into the
// next epoch while the publisher reads the last one; at the epoch boundary the accumulator becomes the
// answer and starts again from nothing. A consumer that goes away is gone within two epochs, and one
// that is merely slower than the fastest never disturbs it.
//
// A claim RAISES a publisher's rate and can never lower it. The publisher's own default is a floor, not a
// fallback: that data is captured for telemetry and the datalog whether any module reads it or not, and
// those never claim. A slow consumer must not be able to drag a channel down to its cadence and quietly
// coarsen the logs — 0 here means "nobody wants it faster", not "nobody wants it".
namespace sigrate {

// Long enough that every consumer claims several times within one — the slowest module cadence in this
// firmware is 20 Hz, so even a single-claim-per-update module lands ten times over.
constexpr uint32_t EPOCH_MS = 500;

struct Claim { uint16_t published; uint16_t accum; };
inline Claim    g_rate[SIG_COUNT] = {};
inline uint32_t g_epoch_ms = 0;

// A consumer says how fresh it needs this channel. Called from the code that actually reads it, every
// time it reads it — that is what makes the claim disappear when the consumer does.
inline void need(SignalId s, uint16_t hz) noexcept {
    const unsigned i = static_cast<unsigned>(s);
    if (i < SIG_COUNT && hz > g_rate[i].accum) g_rate[i].accum = hz;
}

// The highest rate claimed during the last completed epoch. 0 = nobody wants this channel faster than
// the publisher's own default. Publishers take the greater of this and their default, never the lesser.
inline uint16_t required_hz(SignalId s) noexcept {
    const unsigned i = static_cast<unsigned>(s);
    return (i < SIG_COUNT) ? g_rate[i].published : 0u;
}

// Roll the epoch when it is due. Call at frame rate from anywhere that runs unconditionally.
inline void service(uint32_t now_ms) noexcept {
    if (g_epoch_ms != 0 && (now_ms - g_epoch_ms) < EPOCH_MS) return;
    g_epoch_ms = now_ms;
    for (unsigned i = 0; i < SIG_COUNT; i++) {
        g_rate[i].published = g_rate[i].accum;   // whoever claimed in the epoch just ended
        g_rate[i].accum     = 0;                 // ... and they must say so again to keep it
    }
}

}   // namespace sigrate
