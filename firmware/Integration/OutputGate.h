#pragma once
//
// OutputGate — WHEN an output is on, as a small state machine with no opinions about expressions.
//
// An output slot answers two questions each frame: "should it come on" and "should it go off". Those
// are expressions, and the expression VM (Signal/Expr.h) is deliberately pure — no timers, no
// latches, no memory of the last answer — which is exactly what makes a condition safe to evaluate
// on a running engine. Everything that REMEMBERS therefore lives here:
//
//   the latch          two conditions rather than one, so the band between them is a deadband:
//                      "on above 95, off below 90" holds its state in between, which is hysteresis
//                      written as what it is instead of a hidden constant.
//   minimum on / off   a relay that chatters is a relay that welds, and a compressor has a minimum
//                      cycle time in its datasheet.
//   maximum on         a starter motor must not be cranked forever. When it trips, the output is
//                      released whatever the button says and the slot is locked out for the re-arm
//                      delay — the motor cooling down — so a held button cannot immediately re-crank.
//
// Split out of OutputManager so it is testable without a bus, a config image or a pin: the timing
// rules are the part with edge cases, and they should not need hardware to prove.
//
#include <cstdint>

struct OutputGateTimings {
    uint16_t min_on_ms  = 0;   // once on, stay on at least this long
    uint16_t min_off_ms = 0;   // once off, stay off at least this long
    uint16_t max_on_ms  = 0;   // 0 = no limit; else release after this and lock out
    uint16_t rearm_ms   = 0;   // lockout after a max-on trip
};

struct OutputGate {
    uint8_t  on               = 1;   // uint8_t, not bool: a pipeline stage holds a plain pointer to it
    uint32_t since_ms         = 0;   // when it last changed state
    uint32_t on_since_ms      = 0;   // when it last turned on
    uint32_t lockout_until_ms = 0;   // 0 = not locked out

    // Advance one frame. `ask_on` / `ask_off` are the two conditions' answers for this instant; every
    // rule about how long a state has held is applied here, in one place, in this order:
    // max-on first (a safety limit outranks any request), then the lockout, then the transitions.
    void step(bool ask_on, bool ask_off, const OutputGateTimings& t, uint32_t now_ms) {
        if (on && t.max_on_ms && (now_ms - on_since_ms) >= t.max_on_ms) {
            on = 0;
            since_ms = now_ms;
            lockout_until_ms = now_ms + t.rearm_ms;
            if (!lockout_until_ms) lockout_until_ms = 1;   // 0 means "no lockout"; keep one tick of it
            return;
        }
        if (!on && lockout_until_ms &&
            static_cast<int32_t>(now_ms - lockout_until_ms) < 0) return;   // still cooling down

        const uint32_t held = now_ms - since_ms;
        if (!on && ask_on && held >= t.min_off_ms) {
            on = 1;
            since_ms = on_since_ms = now_ms;
            lockout_until_ms = 0;
        } else if (on && ask_off && held >= t.min_on_ms) {
            on = 0;
            since_ms = now_ms;
        }
        // Neither: the state holds. With two conditions that is the deadband, and it is the point.
    }
};

// WHAT AN UNANSWERABLE CONDITION MEANS. A program that names a dead, absent or never-assigned
// channel cannot answer, and the safe answer is not the same for every load — a cooling fan should
// fail ON, a starter and a fuel pump must fail OFF, and something merely inconvenient can hold its
// last state. So the slot says which, and this is that decision in one place.
//
//   policy: 0 = Off, 1 = On, 2 = Hold
enum : uint8_t { GATE_INVALID_OFF = 0, GATE_INVALID_ON = 1, GATE_INVALID_HOLD = 2 };

inline bool gate_answer(bool answered, float value, uint8_t policy, bool current) {
    if (answered)                     return value != 0.0f;
    if (policy == GATE_INVALID_HOLD)  return current;
    return policy == GATE_INVALID_ON;
}
