# Modular Platform Architecture — design spec

**Status:** design (full scope). No branch yet; pressure-test on paper first, then build
incrementally with Layer 0 (hardware schema) first.
**Goal:** make **hardware the single source of truth for layout and capability**, and let the
firmware, the ecu schema, and the studio layout all **compose** from a per-board manifest
plus a set of self-contained feature modules — so almost any hardware can run the firmware and
the studio shows only what that hardware can actually do. "Compose downward, never sideways."

## Why

Today the studio builds its layout from the **ecu schema** (`definition/ecu.schema.yaml`),
which is *per firmware*. But a layout's pin/channel pickers can only legitimately offer the
resources a given **board** physically has — that's *per board*. So board truth (pins, and more
importantly *capabilities*: how many CAN buses, ETB channels, ignition/injector drivers) is
currently hand-duplicated as hardcoded enums in the per-firmware schema. Consequences:

- The same firmware schema opened against a different board shows the *wrong* pins.
- The real source of truth — `s_capture_resources[]` / `s_compare_resources[]` in the C++ board
  profile (`firmware/Platform/boards/<board>/<board>_profile.cpp`) — is invisible to the studio.
- A board with no electronic throttle still shows ETB dialogs; a board with 4 ignition drivers
  still offers IGN5–12.

It is wider than pins. Hardware varies in **capability**, and the firmware is already modular: a
board with no electronic throttle should not *bundle* the ETB module — that code is absent, its
config is absent, and its layout must be absent too. The missing idea is a way to express, per
board, *what exists*, and to **compose** firmware + schema + layout from it instead of authoring a
full layout per board (the duplication trap — N boards × a complete layout, every feature change
an N-way edit).

## The three layers

Every reference points **down** to a shared substrate — never **sideways** between peers. Hold
that line and modularisation works; break it and you get cascading dialog dependencies.

| Layer | Scope | Declares | Source of truth |
|---|---|---|---|
| **0 — Hardware schema** | per board | resources (capture/output/adc/CAN pools) + capability counts | `definition/boards/<board>.board.yaml` (already exists) |
| **1 — Modules** | per feature | config + telemetry + tables + **layout fragment** + a **manifest** (`provides`/`requires`/`hardware-needs`/`events`) | each module's schema slice |
| **2 — Composed board build** | per board | firmware ∪ ecu-schema ∪ layout for the bundled modules | derived, not authored |

A board build = **Layer 0 + the set of modules whose `requires`/`hardware-needs` Layer 0
satisfies.** Firmware, ecu schema, and the studio layout all fall out of that set.

## The substrate: a typed, composed namespace

The thing every module, Lua, and CAN binds to **by name**. Three kinds of entry:

- **Channels** — one producer, many readers. A measurement or computed value (`rpm`, `map_kpa`,
  `app_position`, `tps_position`, `lambda`, `wheel_speed_fl`). A module declares the channels it
  `provides` and the channels it `requires`. Composition is valid iff every required channel has
  **≥1 provider** in the bundle.
- **Arbitrated actuators** — **many requesters → one owner → one physical output.** The hard
  cross-cut (see below). The owner module performs arbitration; requesters submit a *prioritised
  request* against the named actuator and never know about each other.
- **Events / hooks** — things a module exposes for others (and Lua) to subscribe to: `can_frame(id)`
  received, periodic `N Hz` tick, `sync` event, `fault` raised. The publisher fires; subscribers
  bind by name.

**The key move (why "sideways" dependencies dissolve):** ETB does not require *the APP module*; it
requires the **`app_position` channel**. It does not care whether that channel is produced by the
analog pedal-sensor module, decoded from a **CAN** frame, or published by a **Lua** script. The
dependency is downward into the namespace, satisfiable by *any* provider. The bundle check is
"is there ≥1 provider of `app_position`?", never "is module X present?". No module ever names a
peer.

## Arbitrated actuators (the N→1 concept)

Producer/consumer covers measurements. It does **not** cover the case where several features want
to command **one** output. Throttle target is the worst: the **pedal map**, **idle control**,
**traction control**, and **boost-by-throttle** all want to set it (on a drive-by-wire engine idle
is achieved *through* the ETB, not an IAC valve). Spark/fuel **cut** is the same: rev limiter,
launch, antilag, shift-cut, and engine-protection all request cuts. A full torque model is the
extreme — everything requests torque and a central arbiter converts to throttle/spark/fuel.

Model it explicitly:

- An **actuator** is a named namespace entry with **one owner module** (ETB owns `throttle_target`;
  a cut-manager owns `spark_cut` / `fuel_cut`; a torque arbiter owns `torque_target`).
- Requesters call `request(actuator, value, priority)` (or min/max/override semantics declared by
  the owner). They reference the **actuator contract** only — never each other.
- The **owner arbitrates** (priority / min / max / override, owner's choice) and drives the
  physical output. Arbitration policy lives with the actuator.

So the coupling that looked sideways collapses **into the actuator**, by design. "Which module is
most sideways?" → the **actuators**; arbitration-at-the-owner is the containment.

## Diagnostics, sensors & protection

Three of the cross-cutting concerns — the **sensor/IO** modules (channel providers), the
**`fault`/DTC** event plane, and the **`*_cut`** arbitrated actuators — together form the
diagnostics subsystem. Specced here because today they are tangled into one shallow, non-scalable
lump: `EngineProtection` hardcodes every sensor's health check *and* every fault's cut; faults are
a fixed **16-bit global bitmask**; and the ADC path (`InputMapper`) publishes clamped values as
*always-valid* (a disconnected CLT silently reads its end-stop — no open/short detection exists
anywhere). The redesign **distributes detection to the owner and keeps one shared bookkeeper** —
"compose downward," applied to faults. It is a *refactor-to-replace*, not a bolt-on (see the end).

### Sensors are configured instances — not classes, not a central checker

A sensor is **one generic, configured instance** that owns its whole chain. *Not* a hand-written
class per sensor, and *not* a monolithic "sensor controller" that hardcodes each one (that's just
today's `InputMapper`+`EngineProtection` smell renamed). One reusable analog-sensor type,
instantiated N times from config:

```
Coolant = { input: <ANALOG_VOLTAGE pin>, curve: <cal table/formula>,
            valid: 0.20–4.80 V, channel: clt, faults: [P0117, P0118] }
```

Each frame the instance: reads the raw mV (the background-DMA filtered word, non-blocking) →
**range-checks the raw volts FIRST** (outside the electrical window = open/short → publish the
channel `valid=false` + raise its DTC) → else applies the **calibration curve** → publishes the
physical value `valid=true`. **Validity is set at the source, once.** Consumers (a `thermofan`
reads `clt`) read the channel + its validity and **fail-safe on invalid** (fan ON); they never
know the pin, the curve, or how the fault was found, and never re-derive sensor health. Cross-sensor
checks (TPS↔pedal correlation, P2135) belong to the **composer** of those channels (the throttle
controller), not to either raw sensor. This is the substrate's *channel* contract doing its job —
the sensor `provides` a typed channel with a **load-bearing `valid` flag** (the flag already exists
on `SignalBus`; it is just hardcoded `true` today).

### Faults are DTCs — owned by the raiser, bookkept by one plane

Faults are **diagnostic trouble codes**, not bits in a global mask. The mask was self-inflicted: an
OBD DTC is a 16-bit *code* (the whole P0xxx/P2xxx catalog + a manufacturer `P1xxx` range; J1939's
**SPN+FMI** gives the "which thing × how it failed" taxonomy for free), surfaced **on query**, never
one-bit-per-possible-fault. So "thousands of codes" is just the standard — the 16-fault wall
disappears.

- **Each controller declares + raises its own DTCs** locally — the ETB controller does
  `faults.raise(P2111, ctx)` when it sees a stuck throttle. Detection lives where the knowledge is;
  no central file lists every fault; no controller names a peer.
- **The fault/DTC manager (a plane)** owns the shared bookkeeping controllers must not each
  reinvent: the lifecycle (pending → confirmed → healing/aged — a UDS-style status byte, which is
  exactly today's active/healing/latched), the **freeze-frame** snapshot (today's `RamFaultLog`
  context), severity/lamp, and the SD store.
- **Surfaced via the existing `ObdResponder`** (already serves OBD-II modes 01/03/04 over CAN):
  upgrade it to real codes + status + freeze-frame (mode 02) + pending (mode 07), so a generic scan
  tool reads "P2111 Throttle Actuator Stuck Open" with zero JayECU knowledge. **Live telemetry stops
  being a mask** — it carries the small *active set* + worst severity + MIL/lamp; the full catalog +
  history is read on demand.
- Standard codes where they exist (overboost P0234, CLT P0117/P0118, crank P0335, ETB
  P2101/P0638/P2176), `P1xxx` for the rest. The **DTC catalog** (code → description → default
  severity/lamp) is data, codegen'd into both the firmware registry and a scan-tool description table.

### Protection is a configurable policy that *requests* cuts

Engine protection is **user-authored rules** — a table of *(signal, threshold,
hysteresis) → action (warn / limit / cut, with ramp)*. A trip does two decoupled things: it
**raises its mapped DTC** (so the reader sees *why* the engine pulled), and it **requests its action
against the arbitrated `fuel_cut`/`spark_cut` actuator** — it never writes `frame.fuel_cut`
directly; the cut-manager arbitrates rev-limit vs launch vs protection. Detection, diagnostic, and
action are three separate things linked by config: the tuner sets limits + responses, the DTC falls
out, the cut goes through arbitration.

### What this replaces (no parallel systems, no dead code)

When built, the following are **removed**, not left beside it:

- the flat global `schema/faults.yaml` 16-list + the `uint16 active_faults`/`warning_flags`
  **bitmask** (and the `FAULT_*` enum's 16-cap) → the DTC registry + active-set + log;
- `EngineProtection`'s **hardcoded per-sensor timeout checks**, its **per-fault `frame.fuel_cut =`**
  blocks, and the **`threshold_monitors[8]`** array → sensor-instance validity+DTCs, the configurable
  protection table (which subsumes the monitors), and arbitrated cut requests. `EngineProtection`
  shrinks to *running the protection table*, nothing else;
- `InputMapper`'s **analog path** (linear scale, hardcoded `valid=true`) → the sensor engine (cal
  curves + validity + DTCs); its digital/fixed-input reads either follow or stay a thin board fetch;
- `ObdResponder`'s `0xC100 + FaultId` **placeholder encoding** → proper catalog codes + status from
  the registry.

Net: every condition is detected **once**, at its owner; the FaultManager is the single bookkeeper;
the `SignalBus.valid` flag and the arbitrated `*_cut` actuators (both already present) become
load-bearing instead of vestigial.

## Module manifest

Each module's schema slice gains a manifest (alongside its existing `config`/`telemetry`/`tables`
/`ui`). Sketch:

```yaml
ElectronicThrottle:
  ui: { menu: "Throttle", title: "Electronic Throttle", order: 40 }
  manifest:
    hardware_needs:
      - { kind: etb_channel, min: 1 }      # an H-bridge w/ TPS feedback + current sense
    provides:                               # channels this module publishes
      - tps_position
      - etb_target
      - etb_duty
      - etb_current
      - etb_fault
    requires:                               # channels it consumes (any provider satisfies)
      - app_position
    owns_actuators:                         # N→1 entries this module arbitrates + drives
      - { name: throttle_target, arbitration: priority }
    requests: []                            # actuators this module drives requests INTO
    events:
      subscribes: []
      publishes: [ etb_fault ]
  config: [ ... ]                           # PID gains, limits, calibration, fault thresholds
  tables: [ ... ]                           # pedal map: app_position × rpm → throttle_target
  layout_fragment: { ... }                  # the "Electronic Throttle" dialog + gauges + lamp
```

A consumer example — idle control on a DBW engine requests throttle, doesn't own it:

```yaml
IdleControl:
  manifest:
    requires: [ rpm, clt ]
    requests:
      - { actuator: throttle_target, priority: 30 }   # ETB arbitrates idle vs pedal vs TC
```

**Composition algorithm:** start from the board's hardware schema; include a module iff its
`hardware_needs` are met; then iteratively check `requires` channels and `requests` actuators have
providers/owners in the bundle (a module whose hard requirement is unmet is excluded or flagged).
The result is the bundled set → firmware, ecu schema, layout.

## Layer 0 — hardware schema (mostly already exists)

**This is ~60% built.** The authoritative per-board description is **already** YAML:
`definition/boards/<board>.board.yaml`, consumed by `codegen/board_codegen.py` →
`generated/boards/<board>_board.h` (compile-time constants). It already declares capability counts
(`ign_count`, `low_side_count`, `high_side_count`, `etb_count`, analog/digital input counts),
clock, ADC front-end, and fixed inputs. The firmware abstraction `EcuHardwareAssignment`
(`firmware/Scheduler/HardwareAssignment.h`) already models the full capability set: `etb[2]`,
`can[2]`, `hs[8]`, `ign[12]`, `ls[22]`, `digital_in[8]`, `analog_in[22]`, LEDs. So the board YAML —
**not the C++ profile** — is the single source of truth, and most of Layer 0 is in place.

**The structural gap: facts are scattered across multiple sources.** Per-board truth lives in
*three* places today — the board YAML (capabilities/counts), the C++ profile's
`s_capture_resources[]`/`s_compare_resources[]` (per-pin pools, **labels + caps + order**, carrying
live `ICaptureChannel*`/`ITimerChannel*` pointers), and the **human reference doc**
`hardware/<board>_hardware.md` (the full MCU pin map: GPIO → net/signal → mode → peripheral/AF).
They can drift. The fix collapses them to **one authoritative per-board file**:

1. **Flatten the human pin-map doc into the board YAML.** The pin map (per-pin GPIO, net/signal,
   mode, peripheral/AF) becomes structured YAML rather than a hand-maintained markdown table — so
   the board schema is simultaneously the machine source *and* the human reference (a generated,
   readable view can still be emitted for humans). The capture/compare pool enumeration (labels +
   caps + order) falls out of the pin map, since each pin carries its signal + role. Only the
   channel **pointers** stay in the C++ profile.
2. **`board_codegen.py` generates the firmware C++ header** from it (pin map + derived counts).
3. **The studio reads the board YAML *directly*.** No intermediate `hardware/<board>.json` — that
   would be a drift-prone generated *copy* of the YAML (the same duplication removed with the
   `*_count`/`fixed_inputs` scalars), and its only consumer was ever the studio (the firmware uses
   the C++ header, never JSON). The studio already parses `ecu.schema.yaml`, so the board schema is
   the same mechanism — no new parser. The pools are already flat name-lists
   (`capture_pool: [VR1, VR2, …]`) = exactly what a dropdown needs, no resolution required. So:
   **one board YAML → two consumers** (codegen for the firmware C++ header; the studio reads it
   directly).
4. **Lock test:** the C++ profile's pool labels/order == the board YAML pool (so the pointer table
   can't drift from the declared hardware). Longer term the pointer table itself can be
   *generated* from the pin map (pin → peripheral/AF → channel), eliminating the split entirely.

The **ecu schema references hardware by hint, not by copy** — and this principle is *already
documented in `ecu.schema.yaml`* (the generic trigger's `capture_index` comment: "pin labels
VR1/DIG1/… are HARDWARE, owned by the board/hardware schema and imported by the studio … never
[hardcoded]"). A field declares `hw_ref: capture` (or `output`/`adc`) and stores a plain **index**;
the studio fills its dropdown from the matching pool in the board YAML. The generic
trigger's `streams[].capture_index` already is a raw index. **Remaining cleanup:** the two legacy
enums that still hardcode pin options against that very rule — `crank_capture_index` and
`cam_inputs[].capture_index` — convert to `hw_ref: capture`. (This also clears the deferred
"residual DEAD trigger config fields".)

### Canonical per-pin schema shape (agreed)

The board YAML carries a `pins:` list (**control-relevant pins only** — AV/AT/IGN/LS/HS/VR/DIG/
CAN/ETB/comms; `hardware.md` is retained as the human reference for passive pins: power tree,
clock, VCAP/NC, connector map, CubeMX checklist). **Each pin carries `caps:` — the SET of
functional roles it can be routed to.** A function needing role R is offered every pin whose `caps`
include R; a pin appears under every role it supports (a DIG pin is *both* a trigger input and a
digital input); the user routes, and a routed pin is consumed. **Role pools are derived** — "the
pins with role R, in `pins[]` declaration order" — that ordered list *is* the firmware index for R
(so the trigger capture pool = the TRIGGER_INPUT pins; the compare pool = IGNITION_OUT then
LOWSIDE_OUT). Capability counts derive the same way. No hand-maintained pools or `*_count`.

Per-pin fields: `pin` (LQFP physical no.), `gpio` (port+bit), `signal` (net/silkscreen name),
`caps` (set — `TRIGGER_INPUT DIGITAL_INPUT FREQUENCY_INPUT SENT_INPUT ANALOG_VOLTAGE ANALOG_TEMP
KNOCK_INPUT POWER_GOOD IGNITION_OUT LOWSIDE_OUT HIGHSIDE_OUT PWM_OUT STATUS_LED ETB_PWM ETB_DIR
ETB_ENABLE CAN_RX CAN_TX UART_TX UART_RX`), `mode` (`analog|input|output|alternate`), exactly one
peripheral-binding block (`adc:{unit,channel}` | `exti:{line}` | `timer:{unit,channel,af}` |
`af:{num,signal}`), and optional electrical metadata (`active_low`, `driver`, `polarity`,
`divider`, `assignable:false`, `note`).

```yaml
pins:
  - { pin: 34,  gpio: PA0, signal: AV1,  caps: [ANALOG_VOLTAGE], mode: analog, adc: {unit: ADC1, channel: 0} }
  - { pin: 47,  gpio: PB1, signal: AV12, caps: [ANALOG_VOLTAGE], mode: analog, adc: {unit: ADC1, channel: 9},
      net: 12V_DIVIDED, divider: 0.1091, assignable: false, note: "battery sense" }
  - { pin: 141, gpio: PE0, signal: VR1,  caps: [TRIGGER_INPUT, FREQUENCY_INPUT], mode: input, exti: {line: 0}, driver: MAX9924UAUB+ }
  - { pin: 77,  gpio: PD8, signal: DIG1, caps: [TRIGGER_INPUT, DIGITAL_INPUT, FREQUENCY_INPUT, SENT_INPUT],
      mode: input, exti: {line: 8}, driver: 74HC2G17GW }   # DIG adds DIGITAL_INPUT/SENT (clean level); VR can't
  - { pin: 1,   gpio: PE2,  signal: IGN1, caps: [IGNITION_OUT],        mode: output, driver: IX4427NTR }
  - { pin: 49,  gpio: PF11, signal: LS1,  caps: [LOWSIDE_OUT, PWM_OUT], mode: output, driver: VNLD5090TR-E }
  - { pin: 122, gpio: PD6, signal: ETB1_PWM, caps: [ETB_PWM], mode: alternate, timer: {unit: TIM2, channel: 4, af: 1} }

# resources: ONLY what isn't derivable from caps — timebase + compound multi-pin devices.
resources:
  timebase: { timer: TIM5, ticks_per_second: 1000000 }
  can: [ {name: CAN1, rx: CAN1_RX, tx: CAN1_TX}, {name: CAN2, rx: CAN2_RX, tx: CAN2_TX} ]
  etb: [ {name: ETB1, pwm: ETB1_PWM, dir: ETB1_DIR, dis: ETB1_DIS},
         {name: ETB2, pwm: ETB2_PWM, dir: ETB2_DIR, dis: ETB2_DIS} ]

# Outside-world connector map — STRUCTURED (not prose in hardware.md), because it feeds a
# generated artifact: the per-vehicle wiring diagram = f(connectors, tune). Each terminal links
# to a control signal (→ pins[].signal) OR carries a passive role (ground/power/sensor_supply);
# differential / multi-line signals (VR±, CAN H/L, ETB±) carry a `line` discriminator.
connectors:
  - id: CN3
    part: "776231-1"
    color: blue
    desc: "Sensors, analog, VR, digital"
    terminals:
      - { pin: 27, net: CON_AV1,  signal: AV1 }
      - { pin: 6,  net: CON_VR1+, signal: VR1, line: "+" }
      - { pin: 7,  net: CON_VR1-, signal: VR1, line: "-" }
      - { pin: 10, net: GND,      role: ground }
  - id: CN4
    part: "776228-1"
    color: black
    desc: "Power, HS, ETB, knock, CAN1"
    terminals:
      - { pin: 9,  net: CON_12V_RAW,    role: power,         voltage: 12 }
      - { pin: 14, net: CON_5V_SENSOR1, role: sensor_supply, voltage: 5 }
      - { pin: 10, net: CON_CAN1_H,     signal: CAN1, line: H }
      - { pin: 20, net: CON_ETB1+,      signal: ETB1, line: "+" }
```

**`board_codegen.py` generates from this one file:** `generated/boards/<board>_pins.h` (the pin/
port/ADC map + channel objects the firmware consumes) and `generated/boards/<board>_board.h`
(constants — counts now `= len(pool)`). The **studio reads this YAML directly** (no separate JSON;
see Layer 0). **Lints/lock tests:** every pool name resolves to a pin with
a compatible role/capability; every connector terminal's `signal` resolves to a pin; the C++
profile's `s_capture_resources[]`/`s_compare_resources[]` labels+order == `capture_pool`/
`output_pool`; ADC channels unique; no GPIO used twice. Endgame: generate the C++ pin bindings
(port/bit, ADC unit/channel, AF) directly from `pins:`, retiring the second source.

**The connector map is structured (graduated out of `hardware.md`)** precisely because it is
*generative*, not merely reference — see the wiring-diagram artifact below. The remainder of
`hardware.md` (power tree, clock, VCAP/NC, CubeMX checklist) stays as the human reference.

### Generated artifact: the per-vehicle wiring diagram (Layer 2)

A wiring diagram is a composed, **per-vehicle** artifact = `f(connectors, tune)`: the tune assigns
`map_sensor → AV3`, `crank → VR1`; the generator resolves `AV3 → CN3-25`, `VR1 → CN3-6/7`, adds the
fixed power/ground/CAN/sensor-supply terminals, and emits a "wire it like this" sheet (and the
studio's "wire-to: CN3-25" hint in the input-assignment dialog). Per-vehicle, because it depends on
the tune — the same way the layout is per-board.

**Fuller version (noted, not yet built): device wiring footprints.** Each sensor/actuator module
declares its **footprint** in its manifest — a 3-wire MAP sensor = `{5V, GND, signal→AV3}`; an
injector = `{12V, low_side→LS1}`; a VR = `{diff_pair→VR1±, shield→GND}`. Footprints + connector map
+ tune draw the *complete* device-to-terminal harness, not just which terminal lands where. The
connector map is the foundation for it.

## Layer -1 — the SoC seam (the HAL plane)

**Status:** design. Layer 0 answers *what this board has*. It does not answer *what silicon drives
it*, and the tree currently conflates the two — which is fine for one board on one MCU and blocks
the second of either.

### The gap, measured

- `boards/jaytek_v1/board_hal_jaytek_v1.cpp` is 799 lines, of which **277 touch STM32 HAL, ADC,
  DMA or GPIO registers directly**. That is MCU *mechanism* sitting in the board tier. A second F7
  board copies it; an F4 board cannot use it at all.
- The dependency runs the wrong way: `stm32f7xx/platform_hal_stm32.cpp` — the **MCU** tier —
  `#include`s `generated/boards/jaytek_v1_board.h`, the **board** tier.
- `platform_freq_enable()` / `_sent_enable()` / `_pulse_enable()` are declared in `platform_hal.h`
  but implemented in the **board** file, where they derive the EXTI line arithmetically as
  `BOARD_DIN_FIRST_PIN_BIT + pin`. That shortcut encodes "the digital pool is contiguous on one
  port", which is true of jaytek (PD8-PD15) and false of the next board.

### Two parallel HALs exist today; one of them is right

| | Interface-and-pool HAL | Flat C HAL |
|---|---|---|
| Surface | `ICaptureChannel`, `ITimerChannel`, `IAlarmTimer`, `IHBridge`, `ICanChannel`, `BoardProfile` | `platform_read_ain_raw(pin)`, `platform_read_din(pin)`, `board_set_ls(i)`, `board_read_adc_*` |
| Covers | trigger capture, firing outputs, H-bridges, CAN | ADC, digital in, freq/pulse/SENT, LEDs, baro |
| Names a pin above the board tier? | no | no — but assumes an *index→pin rule*, which is the same thing |
| MCU-portable | **yes** | no |

The first is the model: the board constructs typed resource objects and hands up a pool; nothing
above names a pin *or* knows how an index becomes one. The second is retained only as the
engine-facing convenience façade and stops being a per-MCU implementation.

Three interfaces in `Scheduler/ITimerChannel.h` — `IGpioOutput`, `IGpioInput`, `ICanBus` — have
**zero implementations and zero users**; `ICanChannel` (`Can/ICanChannel.h`) is the live CAN
interface. The dead three are deleted rather than carried as a second vocabulary.

### The split

```
firmware/Platform/
  *.h            INTERFACES — no MCU header, no board header: platform_hal.h, board_hal.h,
                 board_profile.h, AdcFold.h, plus the I* family in Scheduler/.
  stm32f7xx/     MECHANISM — ADC scan+burst unit, capture, compare, alarms, DMA-PWM, CAN,
                 flash, RTC, watchdog, USB. Parameterised by a channel list; knows no board.
                 A second SoC is a sibling directory (stm32f4xx/), selected like BOARD is.
  devices/       PART drivers — a sensor or chip hanging off a bus, written once against its
                 datasheet and shared by any board carrying it (e.g. Lps2xBaro over II2cBus).
  boards/<board>/ FACTS — the tables (generated from Layer 0) plus which mechanism instances
                 this board populates, and which parts sit on which bus.
```

(An earlier draft proposed grouping the MCU tier under `soc/`. `Platform/stm32f7xx/` beside a
future `Platform/stm32f4xx/` already says the same thing, and the rename is pure churn.)

`platform_hal.h` keeps its signatures — engine modules are unaffected — but is implemented **once,
generically, over the profile**: `platform_read_ain_raw(pin)` becomes a pool lookup, not
`board_read_adc_voltage(pin)` plus an index rule. `platform_hal_stm32.cpp` loses its board include
because it stops being the implementation.

### Contracts the seam must state, because silicon varies

1. **The firing sink is a single register write.** `ITimerChannel::force_output_now()` is documented
   ISR-safe and is called from the TIM5 compare ISR at NVIC priority 2. An output behind an SPI
   smart driver or a CAN PDM **cannot** honour that. So a resource must declare its latency class,
   and the `PinArbiter` must refuse to bind a coil or injector to a slow sink rather than discover it
   at 8000 rpm. Today the contract is a comment; it becomes a property.
2. **Shared-converter borrow is a board fact, not a law.** jaytek time-shares ADC3 between knock and
   AV13-16 and needs the borrow/return protocol plus a staleness stamp. Proteus puts every analog
   input on ADC1, so ADC3 is knock-only and the borrow is pure overhead. The burst interface must
   express "capture N samples at rate R" without mandating the hand-back.
3. **DMA buffer placement is SoC-specific.** `.dma_nocache` + the MPU region exists because the F7
   has a D-cache and DMA cannot see through it. An F4 has no D-cache; an H7 has a different memory
   map and more masters. This stays in the SoC tier and never appears in a board file.
4. **A capture line is not always an EXTI line.** EXTI-with-line==pin is an F7/jaytek convenience.
   Timer input-capture is the alternative on the same silicon and the only option on some. The pool
   already abstracts this correctly; what leaks is the board file's line arithmetic.

### Build order

1. **Board-neutral composition root.** `main.cpp`'s 8 `jaytek_v1_*` call sites and the three literal
   `generated/boards/jaytek_v1_board.h` includes go behind a board-selected `board_profile.h` /
   `board.h`. No behaviour change; jaytek must stay green. **DONE.**
2. **Delete the index→pin arithmetic.** Every rule the driver used to compute a pin's location
   becomes a table the board schema emits: `BOARD_DIG_PINS` (port/pin/bit/**declared** EXTI line)
   replacing `BOARD_DIN_FIRST_PIN_BIT + pin`; `BOARD_LED_<ROLE>` from the STATUS_LED pin's signal
   name replacing role-by-declaration-position; `BOARD_ANALOG_T_BASE` replacing the literal `16`
   that the firmware and **three** codegen sites each carried separately. Plus the missing
   `BOARD_LED_COUNT` / `BOARD_PG_COUNT` / `BOARD_KNOCK_COUNT`, and a lint that an EXTI line equals
   its pin bit. **DONE.**

   Deliberately *not* done: widening `BoardProfile` into runtime pointer pools
   (`IAnalogInput*`…). Generated tables get the same result with no vtable and no indirection on
   the 1 kHz sensor path. The only thing interface objects buy is a resource that is **not** an
   on-chip ADC channel or a GPIO — an external SPI ADC, a CAN-supplied input, an SPI smart driver.
   Build it when such a board exists, not before; that is the same discipline as step 5.
3. **Lift the mechanism** out of `board_hal_jaytek_v1.cpp` into `stm32f7xx/` as an ADC unit
   parameterised by a channel list. **DONE** — `Stm32AdcUnit` (scan and/or burst, the borrow
   expressed as configuration) plus `AdcFold.h`, which is host-tested. Board file 799 → 565 lines,
   register lines 277 → 153; what remains is pin MODE, the rank sequences and the pool mapping,
   all of which only the board can know.

   The environmental sensor followed, and added a tier: `II2cBus` (neutral) / `Stm32I2cBus`
   (SoC) / `Lps2xBaro` (**devices** — a PART driver, written once against the datasheet and
   shared by every board carrying that part). It probes WHO_AM_I rather than being told which
   variant it is, because all four LPS22-vs-LPS25 differences fail silently. Host-tested.
   Board file 565 → 489.

   Still board-resident and still mechanism: `board_device_uid()` (an MCU fact declared in
   `board_hal.h`, so moving it means moving the declaration to `platform_hal.h` too) and
   `Stm32DmaPwm`'s hardcoded PD6/PD3 (Proteus wants D12/D13 — same port, so bit numbers).
4. **Second board (proteus_f7)** — at that point a board YAML plus a short table file.
5. **Second SoC** — only then is the tier meaningful; do not design it speculatively before a real
   F4/H7 target exists.

Steps 1-3 are refactors with a live ECU as the regression test; each lands separately and each keeps
the bench green.

## The Lua plane

Lua is **not a module** — it is a horizontal plane that both **consumes** (read any channel,
subscribe to any event / CAN frame id) and **produces** (write actuator requests, **publish
channels**, transmit CAN). For "Lua works with every module" to be true there must be a single
published surface it binds to — and that surface is **exactly the module manifest registry**
(channels + actuators + events). *The registry that makes inter-module dependencies safe IS the
Lua API.* They are the same mechanism — which is the strongest validation of the design.

Consequences:
- A Lua-published channel (e.g. `app_position` decoded from CAN by a script) is a legal provider
  that **satisfies ETB's `requires`** with zero ETB awareness of Lua.
- Lua can be woken on a CAN frame (subscribe `can_frame(id)`) and transmit (`can.tx(...)`) — these
  are events/actuators the CAN module publishes.
- **Studio consequence:** the channel/output dropdowns must be **dynamic over the composed
  namespace** = hardware resources ∪ module-provided channels ∪ **Lua-published channels**. Not a
  static enum baked into the schema.

## Layout composition (the studio)

- The studio loads **three** things: the **ecu schema** (structure it already edits), a **board
  hardware schema** (Layer 0), and the **studio layout** it designs.
- Each module owns a **layout fragment** (its dialog(s), gauges, menu entry). The board's layout =
  core layout + the fragments of the **bundled** modules. An absent module contributes no fragment.
- All pin/channel/actuator dropdowns are populated from the **composed namespace + hardware
  schema** — dynamic, board-aware, never hardcoded.
- Layout conditional `enable`/`visible` expressions that cross modules must be **guarded on provider
  presence**, so a fragment degrades gracefully if an optional dependency isn't bundled.
- The existing **`settingSelector`** preset mechanism stays the client-side "pick a wheel /
  pick a preset → set N constants" tool, now operating over the composed schema.

## Breadth check (full feature surface)

Every top-tier feature fits one of: channel provider, channel consumer, arbitrated-actuator
owner/requester, or plane.

- **Planes (cross-cutting, not ordinary modules):** hardware/HAL, the channel namespace, **Lua**,
  the **CAN** framework, datalogging, fault/DTC manager, the tables/curves engine.
- **Engine core:** trigger/sync ✓, fuel (VE + injector char + accel/transient + flex/ethanol +
  staged inj + indiv-cyl trim + closed-loop lambda + fuel-pressure comp), ignition (dwell/advance +
  knock retard + indiv-cyl + multispark), knock DSP, VVT/cam closed-loop, cranking/ASE/warmup,
  DFCO/overrun.
- **Actuated / arbitrated:** ETB + APP, idle (IAC *or* via-ETB), boost, launch/antilag, traction,
  pit/speed limiter, shift-cut/flat-shift, nitrous, water-meth, torque model (arbitration extreme).
- **Sensor / IO:** generic analog (cal curves), generic frequency (wheel speed/flex), generic
  PWM/GPIO (fans/pumps), wideband controller, gear detection.
- **Comms:** CAN keypads/dashes/PDM/ABS, CAN sensors, USB tuning.

Several are inherently cross-cutting (lambda target → fuel; wheel speed → traction+launch+limiter;
pedal → analog *or* CAN), and each cross-cut is a **named contract**, never a peer call.

## Relationship to the existing schema

Mostly additive — and more is already built than first assumed:
- the ecu schema is **already module-grouped** (`ui:{menu,title,order}`) and codegen emits
  per-module headers;
- **Layer 0 already exists as `definition/boards/<board>.board.yaml` → `board_codegen.py` → C++ header**
  (capability counts, clock, ADC, fixed inputs);
- `EcuHardwareAssignment` already models the full capability set (`etb[2]`, `can[2]`, `hs[8]`, …);
- the **`hw_ref` principle is already documented in the schema** and applied to the generic trigger.

What's new:
1. **Capture/compare pool enumeration in the board YAML** + `board_codegen.py` generating the
   firmware pin/port map (`<board>_pins.h`) from it + the profile-pool lock test (close the C++/YAML
   split). *(Done — committed.)*
2. **`hw_ref` hints** completing the migration — convert the two residual pin enums
   (`crank_capture_index`, `cam_inputs[].capture_index`); studio renders dropdowns by reading the
   board YAML directly.
3. **Module manifest** block (`provides`/`requires`/`hardware_needs`/`owns_actuators`/`requests`/
   `events`) per module + the channel-namespace registry + composition validation.
4. **Arbitrated-actuator** runtime concept (owner + prioritised requests) in the firmware.
5. **Layout fragments owned by modules**, composed by the studio over the dynamic namespace.
6. **Diagnostics, sensors & protection** rebuilt around the substrate (see that section): sensors as
   configured instances with validity-at-source, a per-controller-owned **DTC** model bookkept by the
   fault/DTC plane and surfaced over the existing OBD responder, and engine protection as a
   configurable rule table that requests arbitrated cuts. **Replaces** the 16-bit fault bitmask,
   `EngineProtection`'s hardcoded sensor/cut logic, and `InputMapper`'s always-valid analog path.

## Build order (incremental, test-first, Layer 0 first)

1. **Consolidate Layer 0 into the board schema file** — `definition/boards/<board>.board.yaml` carries
   `pins:` (control-relevant pins: GPIO, net/signal, mode, peripheral/AF), `resources:` (ordered
   pools = index contract), and `connectors:`. `board_codegen.py` generates `<board>_pins.h` (the
   firmware pin/port/ADC map + channel objects) and `<board>_board.h` (derived counts); board_hal +
   the profile consume them. Counts/battery derive from pools/pins (no hand scalars). **DONE +
   committed.** *(Remaining: the profile-pool lock test as a unit test.)*
2. **`hw_ref` hints** — ecu schema fields reference hardware pools by index; the studio renders pin
   dropdowns by reading the board YAML directly; convert the two residual pin enums (+ dead trigger
   fields). (No separate JSON — see Layer 0.)
3. **Module manifest** — add the manifest block + the channel namespace registry (`provides`/
   `requires`); codegen validates "every required channel has a provider"; no arbitration yet.
4. **Arbitrated actuators** — the owner + prioritised-request runtime; migrate throttle + cut.
5. **Layout fragments + composition** — modules own their layout fragments; studio composes per board;
   dynamic dropdowns over the composed namespace.
6. **Lua plane** — bind Lua to the registry (read channels, request actuators, subscribe events,
   CAN rx/tx, publish channels); dynamic namespace flows into the studio dropdowns.
7. **Diagnostics, sensors & protection** (builds on 3–4: needs the channel registry + arbitrated
   cuts). Order within: stand up the **fault/DTC manager + catalog** and migrate the existing 15
   faults onto real codes (bench stays green) → convert sensors to **configured instances with
   validity-at-source** (subsume `InputMapper`'s analog path; delete the always-`true` valid) →
   upgrade `ObdResponder` to real codes/status/freeze-frame/pending → replace `EngineProtection`
   with the **configurable protection table** requesting arbitrated cuts (delete the hardcoded
   sensor checks, per-fault cuts, and `threshold_monitors[8]`). Each step removes its predecessor's
   code — no parallel fault systems left standing.

## Risks / open questions

- **Namespace typing & units** — channels need a type + units + scale contract so a provider and
  consumer agree (reuse the existing field metadata). A mismatch must be a composition error.
- **Arbitration semantics** — priority vs min/max/override per actuator; needs a small, well-defined
  policy set, not arbitrary code (Lua can do arbitrary, the core set should be declarative).
- **Composition determinism** — bundle resolution must be deterministic and diagnosable (clear
  "module X excluded because requirement Y unmet" messages), and ideally stable across versions.
- **Layout fragment format** — how a fragment expresses its dialog so the studio can place/compose
  it (menu order, cross-module `enable` guards). Biggest studio-side unknown.
- **Versioning** — `config_version` / schema interplay when the bundled set differs per board (the
  tune is board-specific once composed).
- **Scope discipline** — this is large; each build step must stand alone and ship value (Layer 0
  export is useful even if nothing above it is built yet).
- **DTC code scheme** — internal representation + assignment. Plain OBD P-codes (scan-tool native,
  but flat) vs a J1939 SPN+FMI decomposition (per-controller SPN range × failure mode — composes
  better, needs a P-code mapping for the OBD responder). Codegen must assign stable IDs without a
  central hand-maintained list, and the on-wire format a real reader expects (OBD) pins this
  down — confirm against the target tool before fixing it.
- **Sensor calibration format** — one curve mechanism (lookup table vs piecewise vs formula) reused
  across all sensor instances + a shared curve/preset library (CLT/IAT/TPS/MAP standard curves), so
  "Coolant = GM-CLT curve" is a pick, not a re-entry. Reuses the tables/curves engine.
- **Protection table shape** — fixed rows for the common limits (RPM/CLT/oil/boost/
  lean) vs a fully generic rule set vs both; and the action vocabulary (warn/limit/cut + ramp) as a
  small declarative set the cut-arbiter understands.
