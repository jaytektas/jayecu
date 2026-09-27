#pragma once
#include "ITimerChannel.h"
#include <cmath>
#include <cstdint>

// ---------------------------------------------------------------------------
// IHBridge — a full H-bridge as a first-class HAL object, NOT a bare PWM pin. You drive it with a
// SIGNED command (|cmd| = duty, sign = current direction) at a carrier frequency, and enable/disable
// it. DIR/DIS live INSIDE the bridge, so the actuator/control layer never touches them — it just says
// "drive this bridge to -40%". The board declares its bridges (count + pins) as fixed hardware, the
// same way `battery` is a board feature; the only thing tunable is what the bridges DO (the actuator
// mode). See docs/pwm-hal-design.md §5.
// ---------------------------------------------------------------------------
struct IHBridge {
    virtual ~IHBridge() = default;
    virtual void set_freq(uint32_t hz) noexcept = 0;     // carrier (shared per DMA timer)
    virtual void drive(float signed_pct) noexcept = 0;   // -100..+100: magnitude = duty, sign = DIR
    virtual void enable(bool on) noexcept = 0;           // DIS + PWM enable
};
