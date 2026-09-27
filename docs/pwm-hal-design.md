# PWM HAL — design of record

How the firmware drives PWM outputs on the jaytek_v1 (STM32F767ZIT6) board: a thin hardware seam
(`IPwmOut`), two backends behind it (DMA-PWM and SoftPwm), and a hardware-agnostic H-bridge actuator
layer on top that runs ETBs, BAC valves, or a 4-wire bipolar stepper across the two bridges.

Everything below is grounded in the datasheet (`reference/stm32f767zi.pdf`) and reference manual
(`reference/rm0410.pdf`); the pin/timer/DMA facts were read out of those, not assumed.

> **AS BUILT (differs from §4/§6/§8 below).** The `IPwmOut` seam was never taken up. The board's
> bridges are `Jaytek1HBridge` (`jaytek_v1_profile.cpp`), which implements **`IHBridge` directly** and
> talks to `Stm32DmaPwm` with no PWM interface in between; every non-bridge PWM goes to `SoftPwm`
> through `OutputManager`/`TachOutput`, also directly. `IPwmOut`, `SoftPwmOut`, `DmaPwmOut` and
> `HBridgeDrive` therefore had no implementor, no caller and no test, and have been deleted. The
> hardware-agnostic seam the actuator layer actually stands on is **`IHBridge`** — §5 holds as
> written, and it is a seam of the same kind one level up. What §4 buys that is not already had is a
> single PWM abstraction shared by the OutputManager path and the bridge path; if that is wanted, the
> interface is one file and the two adapters are ten lines each, but it should arrive with a caller.

---

## 1. The constraint that shapes everything

The board's two H-bridge PWM pins are **PD6 (HBRIDGE1) and PD3 (HBRIDGE2)**, wired to two IFX9201
H-bridge drivers. The ETBs on them want a **10–25 kHz** carrier (3 kHz whines; >20 kHz is silent).

**PD3/PD6 have NO timer alternate function** — confirmed against datasheet Table 13 (p.93): AF1
(TIM1/2) and AF2 (TIM3/4/5) are both `-` for both pins. So true hardware PWM (timer→pin via AF) is
impossible on the ETB pins. And an output-compare-ISR ("toggle the GPIO in the compare ISR") at
10 kHz gets shredded by firing-layer preemption — a ~20 µs preempt on a 50 µs half-period is a ~40%
duty glitch on a *closed-loop throttle*. Neither classic tier works for the ETBs.

### The answer: timer-triggered DMA → GPIO BSRR
A free timer's events fire **DMA** transfers that write pre-computed words straight to `GPIOx->BSRR`.
Hardware-timed edges on pins with **no AF, zero CPU per edge, zero jitter** (DMA beats the CPU on the
bus and is immune to the firing ISR). This is the only way to get clean high-frequency PWM on PD3/PD6.

---

## 2. The pin / timer / DMA facts (read from the manuals)

### 2.1 Which output pins can do hardware PWM (datasheet Table 13)
Audited all 44 output-capable pins. Only these have a usable timer channel AF:

| pin | role | timer channel |
|-----|------|---------------|
| PE9/PE11/PE13/PE14 | LS10/12/14/15 | TIM1_CH1/2/3/4 |
| PE8/PE10/PE12 | LS9/11/13 | TIM1_CH1N/2N/3N (complementary) |
| PE5/PE6 | IGN4/5 | TIM9_CH1/2 |
| PC6 | HS8 | TIM3_CH1 / TIM8_CH1 |

Everything else — **IGN1‑3, IGN6‑12, LS1‑8/16‑22, HS1‑7, and the two H-bridges (PD3/PD6)** — has no
timer channel. So AF-hardware-PWM would cover only ~7 scattered pins, and the one real cluster (TIM1
on LS9‑15) is frequency-locked. **Not worth a tier** — DMA-PWM does the same job on any pin.

### 2.2 Timers in use vs free
- **TIM5** (32-bit, 1 MHz) — firing/scheduler core (CCR1=DCO, CCR2=angle, CCR3=time, CNT=capture). Off-limits.
- **TIM4** (16-bit) — the SoftPwm tick engine. Dedicated.
- **TIM1, TIM2, TIM3, TIM8** + TIM9‑14 — free. (TIM6/7 are basic, no channels.)

### 2.3 DMA-to-GPIO needs DMA2 + TIM1/TIM8 (RM0410 Table 28)
Two hard F7 facts:
1. **Only DMA2 can reach GPIO** (AHB1); DMA1's peripheral port is APB1-only.
2. **Only TIM1 and TIM8 route DMA requests to DMA2** (Table 28). TIM2/3/4/5 are on DMA1 → can't drive a GPIO DMA.

So the ETB DMA-PWM uses **TIM1**. DMA2 usage today: only `Stream4/Ch0` (ADC1) and `Stream1/Ch2`
(ADC3). The assignment that dodges both:

| timer event | DMA2 stream | ch | action |
|-------------|-------------|----|--------|
| TIM1_UP   | **Stream 5** | 6 | period start → BSRR **set** PD3+PD6 |
| TIM1_CH1  | **Stream 3** | 6 | ETB-A duty → BSRR **reset** PD6 |
| TIM1_CH2  | **Stream 2** | 6 | ETB-B duty → BSRR **reset** PD3 |

Free, no ADC collision. Leaves DMA2 Streams 0/6/7 + all of DMA1. (Spare TIM1_CH3 → Stream6/Ch6.)

---

## 3. The two backends

Final model: **two PWM backends, no middle tier.** OC-ISR and AF-hardware are dropped — nothing needs
them (the fast jitter-sensitive load is the 2 ETBs → DMA; everything else is slow → SoftPwm).

### 3.1 DMA-PWM (`DmaPwmOut`) — the two H-bridge pins
- **TIM1 at full APB2 clock (~216 MHz), PSC=0.** `ARR = f_tim / f_pwm`, `CCRn = duty% · (ARR+1)`.
- Range **1 Hz – 50 kHz** with duty resolution always better than ~0.025% (≥12 bits) — far past the
  ~0.1% an ETB needs. (16-bit ARR only forces a prescaler below ~3.3 kHz, which DMA-PWM never needs.)
- **Both bridges share TIM1's period (one frequency), independent duty per CCR.** Correct for every
  use — two ETBs want the same carrier, two stepper coils want the same carrier.
- 3 DMA2 streams write constant BSRR words; **duty change = a CCR write, no DMA reconfig, zero CPU.**

### 3.2 SoftPwm (`SoftPwmOut`) — everything else
The existing single-timer (TIM4) software pulse engine + shared channel allocator. It carries every
non-bridge PWM (tach, fans, pumps, boost, solenoids, …). One timer + one sorted event list, so cost
scales with **total edge rate**:

```
ISR load ≈ (Σ 2·f_i) × ~250 ns/edge      →  budget ~2% CPU ≈ Σ f_i ≤ ~30–40 kHz
```

So we **widen the channel pool and cap the per-channel frequency**: `MAX_CHANNELS` raised (16–24), and
SoftPwm outputs' `pwm_freq_hz` clamped to ~2 kHz in the config layer. That keeps ~20+ channels under
~1.5% CPU. (Sub-list insert is O(N), so ~24 ch @ 1 kHz is the comfortable corner.)

---

## 4. The seam: `IPwmOut`

The entire firmware↔HAL contract for PWM is one interface that knows nothing about ETBs/steppers/
backends:

```cpp
struct IPwmOut {
    virtual ~IPwmOut() = default;
    virtual void set_freq(uint32_t hz) = 0;   // set once at config (shared per DMA timer)
    virtual void set_duty(float pct)   = 0;   // 0..100, the hot path
    virtual void enable(bool on)       = 0;   // park/Hi-Z when off
};
```

**Not built — see the AS BUILT note at the top.** `DmaPwmOut` and `SoftPwmOut` would implement it;
callers can't tell which. DIR/DIS stay on the existing
GPIO sink (`ITimerChannel`). Above this line everything is hardware-agnostic and host-testable; below
it is board-specific registers.

This same interface unifies the generic OutputManager PWM path and the H-bridge actuator — one PWM
abstraction, two backends, instead of the OutputManager poking SoftPwm directly.

---

## 5. The H-bridge actuator layer (firmware, hardware-agnostic)

`HBridgeActuator` is the **purely mechanical transducer** over the two bridges (`2× IHBridge` — each a
bridge PWM + DIR/DIS). It converts a command into coil drive and **nothing else**: no target, no
position integrator, no velocity/accel pacing, no clamps, no move-vs-hold decision. It speaks *only*
`IHBridge::drive()` / `enable()`, so it tests on the host with fake bridges — no hardware in the unit
tests. The IFX9201's PWM+DIR pair **is** a bipolar coil driver (DIR = current polarity, PWM =
magnitude), so two bridges are natively either two DC actuators or one stepper.

### Modes
- **`dual_dc`** — bridge0 = actuator A (ETB / BAC / motor), bridge1 = B. `drive_dc(bridge, signed_pct)`
  passes a **signed** demand straight through (`dir_invert` applied): `|cmd|` = duty, `sign` = DIR,
  `enable` = DIS. Two ETBs, or ETB+BAC, or 2 BAC. **No magnitude clamp here** — the module clamps.
- **`bipolar_stepper`** — bridge0 = coil A, bridge1 = coil B. `render_step(microstep_pos, current_pct)`
  renders **the instantaneous position the module hands down**: `A = cos θ · I`, `B = sin θ · I`, where
  `θ = (pos mod 4·microstep)/(4·microstep)·2π` and signed drive carries DIR. `microstep = 1` is the
  full-step wave `(A+,B+)→(A+,B−)→(A−,B−)→(A−,B+)`. The zero-jitter DMA carrier is what keeps coil
  currents clean. The actuator makes **no** move-vs-hold distinction — `current_pct` is whatever the
  module chose for this frame.

### Speed, limits, and the integrator live in the controlling module — NOT the actuator
The actuator is stateless. Everything that *decides* sits above it:
- **Position integrator + velocity/accel ramp** (the former `max_steps_per_service`). A stepper is
  open-loop — slew the electrical angle faster than the rotor can follow and it slips poles, losing
  sync with no feedback to recover. So the module holds current/target position, paces the approach at
  a safe rate, and calls `render_step` with the *instantaneous* position each frame.
- **Move-vs-hold current.** A coil holding full PWM at standstill cooks; the module picks a move
  current while stepping and a lower hold current at rest, and passes it as `current_pct`.
- **Travel / current / duty limits.** Any clamp (motor thermal ceiling, throttle authority) is applied
  by the module *before* it commands — the actuator never clips.

### Control sits above, on the bus
Idle / throttle-position / stepper-target controllers are firmware modules that publish a **demand to
the SignalBus** (signed duty for DC, step target for stepper), exactly like `idle_duty` today. The
controlling module owns the ramp/limits/current policy above, then drives the actuator; a controller
never reaches past it.

---

## 6. Composition edge — the one place that knows the hardware

`SystemComposer` (board-aware) is the only code that binds concrete to abstract. **As built the
binding happens one level up**: `jaytek_v1_hbridges()` hands the composer two `IHBridge*` and the
composer never sees a PWM object at all.
- bridge pins PD6/PD3 → `DmaPwmOut` (TIM1 ch1/ch2); every other PWM pin → `SoftPwmOut`
  *(as built: `Jaytek1HBridge` → `Stm32DmaPwm` directly; other PWM → `SoftPwm` directly)*
- DIR/DIS (PD7/PD5, PD4/PD2) → GPIO sinks
- actuator `mode` + stepper params ← config

Move the ETB to another pin, swap a backend, or change boards → touch only the bottom box and one
line here. The actuator and control code never know.

```
controlling module          → integrator/ramp/limits/current [hw-agnostic]
HBridgeActuator (render)    → IHBridge::drive() / enable()    [hw-agnostic, host-tested]
──────────────────────────────────────────────────────────  ← seam: IHBridge (IPwmOut + DIR/DIS GPIO)
DmaPwmOut / SoftPwmOut       → TIM1+DMA2 BSRR / SoftPwm        [HAL, board-specific]
```

---

## 7. Config (schema)

Config splits along the actuator/module seam — the actuator gets only what it needs to *render*, the
controlling module gets everything that *decides*:

- **Actuator (`HBridgeActuator::Config`):** `mode` (`dual_dc` | `bipolar_stepper`), `pwm_freq_hz`
  (carrier, shared by both bridges), `dir_invert` (bit per bridge — wiring polarity / stepper rotation),
  `microstep` depth (1/2/4/8/16). Nothing else lives here.
- **Controlling module:** the demand signal (cand) + enable; and all policy — current limit / duty
  clamp (dual_dc), move-current % + hold-current % + accel/max-rate ramp + steps/rev (stepper). The
  module owns the integrator and feeds the actuator instantaneous `(position, current)` / `signed_pct`.
- **SoftPwm outputs:** `pwm_freq_hz` clamped to the SoftPwm ceiling (~2 kHz) at codegen/runtime.

---

## 8. Implementation plan / files

1. ~~`firmware/Scheduler/IPwmOut.h` — the seam.~~ **Not adopted; deleted.**
2. ~~`firmware/Scheduler/SoftPwmOut.h` — wraps SoftPwm.~~ **Not adopted; deleted.**
3. `firmware/Platform/stm32f7xx/Stm32DmaPwm.{h,cpp}` — TIM1 + DMA2 → BSRR. **Board-specific,
   bench-validated only** (no host coverage possible).
4. `firmware/Engine/Modules/HBridgeActuator.{h,cpp}` — dual_dc + bipolar_stepper, host-tested.
5. SoftPwm: raise `MAX_CHANNELS`; clamp SoftPwm-backed `pwm_freq_hz` in OutputManager.
6. Schema `Outputs`/a new `HBridge` block for the mode + stepper params; codegen.
7. `SystemComposer` wiring + board profile (DmaPwmOut over PD6/PD3).
8. Tests: `test_hbridge_actuator.cpp` (dual_dc sign/duty, stepper full-step pattern, microstep sin/cos,
   hold-current), with fake `IPwmOut`. DMA path validated on the bench (scope etb±).

### Verification
- Host: actuator logic green with fakes.
- Bench: scope etb± — DC duty/dir, then a stepper sweep (coil currents), ultrasonic carrier (no whine).
