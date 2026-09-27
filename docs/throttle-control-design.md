# Throttle Control — cable + ETB (drive-by-wire) design

Status: **agreed design, no code yet** (v0). All major forks resolved in design discussion;
this is the reviewable artifact before implementation. Mechanisms marked EXISTS were verified
against the current tree; MISSING items are the only net-new plumbing.

---

## 1. Scope + what exists today (pre-flight)

A new `ThrottleControl` `EngineModule` that owns **throttle as a function** — currently no module
does. Throttle today is only an *input* plus scattered *consumers*:

- **TPS sensor — EXISTS.** `tps_1`/`tps_2` are sensor-entity producers (`SIG_TPS_1/2`), full
  acquire→decode→cal→filter→diagnostics pipeline, DTCs P0122/P0123/P0121. → consume, don't recreate.
- **Consumers — EXISTS.** `FuelCalculator` (Alpha-N/blend), `TransientThrottle`, `IdleControl`,
  launch/knock/alternator all read `SIG_TPS_*` off the bus independently. Unchanged by this work.
- **Throttle *control* module — none.** → adding `ThrottleControl`.
- **`EngineFrame.etb_target_pct`** — exists, never written. → **deleted** (bus is king; everything
  on the bus so the Lua plane can sit between demand and output).
- **`HBridgeControl` + `etb[2]` HAL slots — EXISTS** but open-loop (demand %→duty, no feedback). →
  the IHBridge HAL underneath is reused for actuation; the open-loop module is not the control loop.
- **No accelerator pedal (APP)** anywhere. Out of scope — this ETB has **TPS feedback, no pedal**.

## 2. Hardware reality (jaytek_v1)

Each ETB = an **IFX9201SGAUMA1** H-bridge driven by three lines:

| Line | Pin (ETB1 / ETB2) | Sense |
|---|---|---|
| `DIS` | PD5 / PD2 | **active HIGH disables**; must be driven **LOW to enable**. Floating/reset/unpowered → disabled. |
| `DIR` | PD7 / PD4 | rotation direction |
| `PWM` | PD6 / PD3 | TIM2_CH4 / TIM2_CH2 (see TIM2 sharing note in hardware doc) |

Confirmed by Jason: DIS is **fail-safe by default** — "do nothing" = disabled = the throttle spring
returns the plate to its limp position. This is the safe state, and it is the hardware default.

**Not present:** no external watchdog IC, no motor-supply relay, and the IFX9201 **ERR/SPI
diagnostics are not wired** (the chip self-protects on OC/OT but firmware cannot read the trip).
Consequence: **the MCU is the sole authority over the throttle motor.** Acceptable for bench /
no-pedal; the independent-authority gap is the road-use roadmap item (§11).

## 3. Architecture

One bus-isolated `EngineModule` (`firmware/Engine/EngineModule.h` contract: `update(pos,bus,frame)`
@ 1 kHz + `on_engine_start/stop`), modelled on `TransientThrottle`. **Everything in/out is bus
signals — no pins, no direct actuation** — so the Lua plane can intercept demand without the module
knowing.

```
                    ┌──────────────────────────────────────────────┐
   SIG_TPS_A ─┐     │ ThrottleControl (Level 1: function)           │
   SIG_TPS_B ─┼────▶│  state machine + per-ETB PID                  │
   target ────┘     │  publishes etb_duty_N                         │──▶ etb_duty_N (bus, Lua-touchable)
                    │                                               │
                    │ ThrottleSafety (Level 2: monitoring)          │──▶ etb_en_N   (bus, fail-safe enable; 0 = LATCHING veto)
                    │  A/B disagree · range · runaway · staleness   │       ▲
                    └──────────────────────────────────────────────┘       │ raise-only (incl. Lua)
                                       │                                    │
            etb_duty_N ── [Lua plane] ─┤                                    │
                                       ▼                                    │
                    ┌──────────────────────────────────────────────┐       │
                    │ HBridgeControl (actuator)  reads duty + dis ──┼───────┘
                    │  relays bus → HAL, decides no safety          │
                    └───────────────────┬──────────────────────────┘
                                         ▼
                    IHBridge / IFX9201 HAL  (OWNS the DIS/DIR/PWM pins;
                                             reset default = disabled)
```

Authority split: **HAL owns the pins**; **`ThrottleSafety` owns the latch decision**;
**`HBridgeControl` is a dumb relay** bus→HAL and never decides safety.

## 4. The module is ETB-only — cable needs nothing

There is **no cable mode in the module** and **no `mode` enum**. A cable throttle is *just a sensor*:
the user configures `tps_1`, it publishes %, the existing consumers read it — there is nothing for a
control module to do, so on a cable engine `ThrottleControl` is simply left disabled.

The module gate is therefore just `enabled` + the per-ETB array. Per enabled ETB: dual analog TPS
feedback → A/B resolve → PID → publish duty + resolved position; full safety + autocal.

**Resolved-position ownership lands exactly where voting happens.** A cable has one TPS, so the
sensor *is* the throttle position (`tps_1`) — no owner needed. An ETB has two TPS that must be
A/B-voted, so the **ETB module publishes the resolved/validated plate position** (`etb_position_N`).
This is the clean answer to the previously-parked ownership question: ownership exists only where
there is something to resolve, and is absent where it is trivial.

**Consumers bind to the active throttle.** `TransientThrottle`, Alpha-N `FuelCalculator`, and
`IdleControl` already read their throttle load via a configurable source id (e.g.
`TransientThrottle.tps_src`, default `tps_1`). The user points that at the active throttle: `tps_1`
on a cable engine, or `etb_position_N` on an ETB engine (the primary ETB when several). No new
mechanism — the free signal-id binding already covers it.

## 5. Config model

Runtime config-array of ETB elements, **count user-configurable**, array sized from a `MAX_ETB`
define (no hardcoded magic — derive from the define). Shape mirrors `output[]`/sensors.

Per-ETB element (working set — names to firm up at schema time):

| Field | Type | Purpose |
|---|---|---|
| `enabled` | uint8 | per-ETB enable (within `mode=ETB`) |
| `tps_a_src` | uint8 (sig id) | feedback TPS A — **must resolve to an analog sensor** (§7) |
| `tps_b_src` | uint8 (sig id) | cross-check TPS B — analog |
| `tps_disagree_pct` | uint8 | A/B disagreement threshold (plate-%) |
| `tps_disagree_ms` | uint16 | disagreement debounce |
| `kp` / `ki` / `kd` | int16 | position PID gains |
| `target_pct` | uint8 | **bench/manual** target (no pedal); also a live CLI setter |
| `duty_out_sig` | uint8 (sig id) | bus signal this loop publishes (default `etb_duty_N`) |
| autocal sweep params | … | duty cap, per-step timeout, motion-plausibility tol |

Top-level: a single module `enabled`. The user enables the ETB module, then enables/configures
however many ETBs in the array. PID/position state lives in the module (per-ETB), reset on
`on_config_change` / `on_engine_stop` (no stale wind-up — same discipline as AlternatorControl).

## 6. Published signals

Per ETB `N`: `etb_position_N` (the **resolved/validated A/B plate-%** — the canonical throttle
position consumers bind to on an ETB engine), `etb_duty_N` (% demand, Lua-touchable), `etb_en_N`
(fail-safe enable; 0 = latching veto, the HBridge slot drives only while it is high), plus telemetry `etb_target_N`, `etb_state_N` (state-machine enum),
`etb_fault_N`. Declared in the schema `signals:` section with `owner: firmware`; codegen emits the
`SIG_*` ids. (Final per-ETB vs array-indexed signal naming decided at schema time.)

## 7. TPS A/B contract

**ETB loop feedback requires its own analog mV TPS wired to the ADC.** A closed-loop position servo
needs low-latency local feedback; CAN/SENT round-trip is unsuitable, and an ETB's TPS is physically
on the body. CAN/SENT TPS may still exist as a *reported/monitor* value, just not as loop feedback.
A misconfigured (non-analog / implausible) feedback trips DIS via the plausibility check anyway —
fail-safe by construction.

Calibration contract: **both TPS are calibrated to publish plate opening 0–100% in the same sense**,
regardless of whether the raw sensors track together or in opposite directions (the opposite-slope
sensor just gets a *descending* cal curve). The module's cross-check is then a trivial,
interface-agnostic **`|A − B| > tps_disagree_pct` (debounced) → fault**; no same/opposite mode in
firmware. **Recommended wiring: opposite-slope raw sensors** — a common-mode raw fault (rail short,
ground offset) then drives the two calibrated %s apart → caught. The supervisor also gates on each
sensor's own `valid` flag (open/short already handled by the sensor layer).

## 8. Safety model

Bosch **E-Gas 3-level** mapping, with the defining property: **the safe state is "remove motor
drive"** (spring returns the plate). Every fault reaction is *stop driving + assert DIS*, never
"drive closed."

- **Level 1 — function:** the per-ETB position PID (`ThrottleControl`).
- **Level 2 — function monitoring:** a **distinct** `ThrottleSafety` participant (separate from the
  PID it watches — E-Gas principle that the monitor must not share state with the function). Checks:
  A/B disagreement, TPS range/validity, position-error runaway, **stale compute** (reuse the
  sync-epoch fresh-compute interlock pattern), config-disabled. Owns the latch + the restricted clear.
- **Level 3 — controller monitoring (independent):** NOT achievable on this board (only MCU GPIO
  drives DIS). See roadmap §11.

**The latching fail-safe DIS** — the core safety primitive:

- DIS is driven **LOW to enable**; default/reset/unpowered = HIGH = disabled.
- **Raise (assert disable) is open to anyone** — supervisor fault, CLI, **and Lua** — and the raise
  **latches**.
- **Clear is restricted** — only an explicit authorized clear (fault-clear / key-cycle) lowers it;
  merely ceasing to assert does **not**. Lua gets a one-way `etbDisable(n)` (a kill it can stab) but
  **no `etbEnable`** — it can veto, never un-veto. Composes with the priority bus (priority =
  authority): the latch is a top-authority write no lower writer (incl. Lua) can override.

**Enable equation** — DIS goes LOW (enabled) only when *all*: module `enabled` and this ETB
`enabled`; supervisor healthy; **no latched disable** outstanding. Any fault or any raise → DIS
HIGH, latched, until authorized clear.

**MCU IWDG** (windowed) — catches a hung control loop → reset; the hardware DIS default makes reset =
de-energized. The IWDG does **not** substitute for Level 3 (a logic bug that keeps petting the dog
while commanding the plate wrong is invisible to it).

## 9. State machine

```
  UNCALIBRATED ──(explicit autocal, engine stopped)──▶ AUTOCAL ──(success)──▶ READY ◀─┐
       ▲                                                  │                    │      │
       │                                              (abort/fail)        (any fault) │
       └──────────────── FAULT/LATCHED ◀────────────────┴────────────────────┘       │
                              │                                                       │
                              └────────────(authorized clear + key-on verify pass)────┘
```

- **UNCALIBRATED** — no valid cal / boot. **DIS latched, no closed loop.** ETB never energizes
  without a cal (safe by construction).
- **AUTOCAL** — entered *only engine-stopped* (like live-reconfigure). The normal %-domain Level-2
  trips are **inhibited** (they'd false-trip on intentional stop-to-stop motion, and the %-mapping
  isn't trusted yet — chicken/egg). Replaced by an autocal-specific envelope: hard duty cap (creep),
  per-step timeout, and **motion-plausibility** — command open ⇒ raw TPS must move the expected way;
  if not (stuck plate / dead / miswired sensor) ⇒ **abort → FAULT/LATCHED**. Autocal does not blind
  safety; it swaps the (invalid) %-envelope for a raw-domain, engine-stopped, bounded one.
- **READY/RUNNING** — full closed loop + full Level-2 supervisor.
- **FAULT/LATCHED** — DIS asserted; restricted clear.

## 10. Calibration: autocal + storage + key-on verify

- **Storage = the tune (config), in RAM.** Autocal writes the learned closed/open endpoints straight
  into the feedback sensor's `cal_raw[]/cal_val[]` (the analog mV cal, `sensors_config.h`); the
  sensor pipeline re-reads live. **No BKPSRAM / no separate learned-store** for this feature.
- **Autocal also sets the raw-fault DTC thresholds.** Since the sweep observes the actual mV at both
  stops, it writes each TPS's `diag_raw_min_mv` / `diag_raw_max_mv` (short-to-GND / short-to-VCC) from
  the observed range ± a margin — so the wire-fault window tracks the real operating range, not a
  guessed default. Computed from the raw min/max *observed* (interface-correct for either slope: the
  opposite-slope B sensor's closed stop is its high mV), not from closed/open ordering.
- **Persistence = the user's normal save-to-flash.** Autocal *changes* RAM cal like any edit; the
  user saves the tune to keep it. **Explicit `findlimits` is the only thing that mutates the cal** —
  nothing learns silently.
- **Key-on = verify, never re-learn.** Config loads flash→RAM as always; the module then validates
  the loaded cal before leaving the latched state: at-rest readings plausible vs the learned limp
  position, A/B agree via the cal. Fail ⇒ stay latched, flag "needs autocal." Pass ⇒ eligible for
  READY.
- **Bench target** (no pedal): per-ETB `target_pct` scalar + a live CLI setter (`throttle <etb>
  <pct>`, mirroring the existing `fire` command) to drive position on the bench.

## 11. New plumbing required

- **`signal_to_sensor[SIG_COUNT]` reverse map — MISSING, add it.** Built once at `Sensors::init()`
  from `SENSOR_CATALOG` (`primary_channel`). Lets autocal resolve `tps_a/b_src`'s signal id → the
  sensor element whose `cal_*` to write. O(1) lookup; small additive change.
- **Delete `EngineFrame.etb_target_pct`** — vestigial; everything is on the bus now.
- Cal writeback into RAM (EXISTS), persistence via tune-save (EXISTS), `HBridgeControl`/`etb[]` HAL
  actuation (EXISTS) — no changes needed beyond binding `demand_sig = etb_duty_N` and pointing it at
  the ETB bridge.

## 12. Roadmap (not now — road use)

True independent **Level 3**: add an external windowed-watchdog IC (or 2nd MCU) doing
challenge-response that gates a **motor-supply relay** (or the DIS line) independently of the main
CPU; and **wire the IFX9201 ERR pin** to a GPIO so firmware can read the chip's own OC/OT trip.
Bench / no-pedal use proceeds without these; they are required before driving a real plate on a
road car.

## 13. Decisions resolved (log)

- Throttle control = its own bus-isolated module, **ETB-only** (no cable mode, no `mode` enum); a
  cable throttle is just a configured TPS sensor with nothing module-side. User enables the module +
  N ETBs.
- Resolved-position ownership exists only where A/B voting happens: ETB publishes `etb_position_N`;
  a cable's single `tps_1` is the position. Consumers bind their throttle source to whichever applies.
- No pedal — bench/manual target only for now.
- Closed loop **publishes duty** (Lua sits between demand and output); does not drive pins directly.
- "Multiple ETBs" = user-configurable count (config-array, `MAX_ETB`).
- A/B both calibrated to plate-% same sense; cross-check `|A−B|`; recommend opposite-slope raw.
- DIS in the H-bridge HAL; latching veto, raise-by-anyone-incl-Lua, restricted clear; Lua raise-only.
- DIS hardware default = disabled (confirmed); active-LOW-to-enable.
- Autocal lives in the module (so DIS doesn't false-trip); engine-stopped; motion-plausibility guard.
- Cal stored in sensor mV cal in RAM; persistence = save-to-flash; key-on verifies, never re-learns.
- ETB requires its own analog mV TPS (resolves the CAN/SENT-has-no-mV problem; mismatch → DIS anyway).
- `etb_target_pct` deleted; bus is king.
