#pragma once

// CycleWire — the ONE place that knows the engine-cycle capture's wire format.
//
// The firmware's CycleRecorder emits [CycleHeader][CycleEdge...]; this turns that into the native
// enginecycle::Cycle the view draws. It sits at the comms boundary on purpose: EngineCycleView draws
// the native model and nothing else, so every protocol detail — byte layout, the signal/channel
// encoding, the crank-fold rule — stops here.
//
// A rusEFI bridge is the same shape: a different decode function, the same Cycle out, and it sets
// Fidelity::Reconstructed because it had to interpolate angle. Ours reports Measured because the ECU
// records the angle it actually fired at.

#include "EngineCycle.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace cyclewire {

// Mirrors CycleRecorder::State in the firmware.
// Stale = a complete cycle is available, but the ECU lost the trigger after capturing it. The frame
// is real — it is the last cycle the engine turned — but it is history, and the rpm in its header is
// the rpm it was captured at, not the engine's now.
enum class State : uint8_t { Idle = 0, Armed = 1, Recording = 2, Complete = 3, Stale = 4 };

struct Result {
    bool     ok      = false;    // the header parsed; `cycle` is only meaningful when true
    State    state   = State::Idle;
    uint16_t total   = 0;        // edges in the whole capture (may exceed what this page carried)
    uint8_t  dropped = 0;        // edges the ECU lost to a full ring; non-zero = incomplete capture
    std::string message;         // human-readable status for the view, always set

    // A whole cycle is available to draw. TRUE for Stale as well: the frame is real, and refusing to
    // draw it would hide the last cycle before a stall — the most useful frame there is when
    // diagnosing why an engine stopped. Ask stale() to caption it honestly.
    [[nodiscard]] bool complete() const {
        return ok && (state == State::Complete || state == State::Stale);
    }
    // The engine lost its trigger after this frame was captured, so the rpm in it is historical.
    [[nodiscard]] bool stale() const { return ok && state == State::Stale; }
};

// Wire header/edge sizes — the firmware asserts these exact numbers, so a mismatch is a bug on one
// side rather than something to tolerate.
inline constexpr size_t kHeaderBytes = 20;
inline constexpr size_t kEdgeBytes   = 4;
inline constexpr uint16_t kMagic     = 0x5943;   // 'C' | 'Y' << 8

// Parse an assembled capture into `out`. `out` is cleared first, so a failed parse leaves an empty
// cycle rather than a half-populated one the view would draw as if it were real.
Result decode(const uint8_t* data, size_t len, enginecycle::Cycle& out);

// Just the header, for the arm/poll loop — no edges needed to decide whether to keep waiting.
Result peek(const uint8_t* data, size_t len);

} // namespace cyclewire
