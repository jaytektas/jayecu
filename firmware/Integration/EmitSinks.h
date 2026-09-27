#pragma once
//
// Emit sinks — the OutSink implementations that drive real actuators at the tail of an output
// pipeline (arbitrate -> encode -> emit).  See docs/polymorphic-pipeline-architecture.md.
//
// These depend only on the MCU-agnostic output backends (SoftPwm + ITimerChannel), not on any
// HAL — SoftPwm is the "software eTPU" pulse engine that carries every PWM output row; a digital sink
// drives a plain GPIO sink.  The pipeline's emit_sink stage calls one of these with the
// encoded command (and the fail-safe value when the arbitrated source is invalid).
//
#include "../Pipeline/OutputStages.h"
#include "../Scheduler/SoftPwm.h"
#include "../Scheduler/ITimerChannel.h"

namespace pipe {

// --- PWM emit: command is a duty percent (0..100) -> SoftPwm waveform on one channel. ---
// Bind the channel's pin once (pwm->set_pin) before running; the sink only sets the waveform.
struct PwmEmitCtx {
    SoftPwm* pwm;
    int      ch;
    uint32_t period_ticks;   // 1 / freq, in the SoftPwm tick unit
    // A CARRIER THAT MOVES. period_ticks is the FIXED setting; when the slot takes its frequency from
    // a table or an expression the manager writes the live period here each frame instead. Kept as a
    // period rather than a frequency because that is what the pulse engine wants, and doing the
    // division once per frame in the manager keeps it out of the sink.
    uint32_t live_period_ticks = 0;   // 0 = use period_ticks
};
inline void pwm_emit_sink(void* ctx, float command, bool /*valid*/) {
    auto* p = static_cast<PwmEmitCtx*>(ctx);
    const uint32_t period = p->live_period_ticks ? p->live_period_ticks : p->period_ticks;
    if (!p->pwm || period == 0) return;
    float duty = command;                                  // already clamped by encode
    if (duty < 0.0f) duty = 0.0f; else if (duty > 100.0f) duty = 100.0f;
    const uint32_t high = static_cast<uint32_t>(period * (duty / 100.0f));
    p->pwm->set_waveform(p->ch, period, high);
}

// --- digital emit: command >= threshold -> drive the pin to its active level. ---
// active_high=true: ON drives the pin HIGH. Bind enable_output() once before running.
struct DigitalEmitCtx {
    ITimerChannel* pin;
    bool           active_high;
    float          threshold;
};
// THE DIGITAL DECISION, in one place. The sink switches on it and the manager PUBLISHES on it, and
// those two must be the same answer or the channel describes a pin that is doing something else.
inline bool digital_on(const DigitalEmitCtx& d, float command) { return command >= d.threshold; }

inline void digital_emit_sink(void* ctx, float command, bool /*valid*/) {
    auto* d = static_cast<DigitalEmitCtx*>(ctx);
    if (!d->pin) return;
    const bool on = digital_on(*d, command);
    const OutputAction act = (on == d->active_high) ? OutputAction::DRIVE_HIGH
                                                    : OutputAction::DRIVE_LOW;
    d->pin->force_output_now(act);
}

} // namespace pipe
