# Sensors — design spec

**Status:** BUILT + bench-validated — the type-driven catalog + per-sensor pipeline (acquire →
raw-diag → calibration curve → filter → operating-diag → publish) shipped in `firmware/Sensors/`,
with analog / switch / frequency / SENT / pulse-width capture. Now being **re-architected** per
[polymorphic-pipeline-architecture.md](polymorphic-pipeline-architecture.md), which generalises a
"sensor" into an **Input** — a composable pipeline of those same stages. This doc remains the
detailed reference for the catalog, types, calibration, and diagnostics.
**Goal:** turn "Sensors" into a **catalog of named sensor functions**, each an instance of a small
set of typed processing **pipelines**, that publish typed **channels** and raise **DTCs** with a
severity. The type is the unit of code reuse; a named sensor is data. Adding a sensor is a schema
row, not a code path.

This composes onto the [[modular-platform-architecture]]: Sensors is the **input catalog**, Engine
Functions is the **output/controller catalog**, both over a shared channel namespace + board
resource pool. Compose downward, never sideways.

## Why a type-driven model

Every coolant-temp sensor does the same job: fit a curve, filter, (optionally) derivative-filter,
check open/short and plausibility. That sameness is real — but it belongs to the **processing
pipeline**, which correlates with the *electrical/quantity class*, **not** with the specific named
role. Three sensors can share a quantity yet differ wildly upstream:

- **CLT / IAT** — temperature, thermistor on a divider → resistance/temp curve + open-short window.
- **EGT** — temperature, but thermocouple via a CAN amplifier → no divider curve, different faults.
- **Oil temp over CAN** — temperature, value arrives cooked → no curve, no open/short concept.

So "TYPE" is two orthogonal things and must be split:

1. **Quantity / type** (code) — drives units, sane range, the pipeline shape, which diagnostic
   checks exist, and what consumers expect.
2. **Interface / source class** (per-input config) — analog-voltage, on-board, digital-frequency,
   switch, CAN-device — drives *what raw means* and which conditioning stages apply.

## Three tiers

| Tier | Lives in | Declares | Example |
|---|---|---|---|
| **1 — Sensor type** | code (~10 types) | ordered pipeline, available stages, units, diagnostic kinds | `temperature`, `pressure`, `switch`, `frequency` |
| **2 — Named sensor** | schema catalog (~100 rows) | type ref, group, default channel(s), legal interfaces, default calibration, default DTC map | `Coolant Temperature`, `Manifold Pressure` |
| **3 — User config** | tune (per-sensor pages) | enable, chosen interface + assigned pin, calibration, filter, diagnostics enable/threshold/severity | this car's CLT on AT1 |

## The pipeline

Every analog sensor type runs the same ordered pipeline; the diagnostic stage appears **twice**
(raw before the transfer, operating after it). Stages marked *opt* are present only when the
type/named-sensor enables them (MAP exposes a derivative; oil pressure does not).

```
1. acquire raw            ADC volts / resistance / frequency / switch / CAN message
2. RAW diagnostics        raw < min | raw > max (Volts)        → circuit DTC (e.g. P0107/8)
3. transfer / calibration breakpoint table  raw → engineering units  (file-loadable, linearise)
4. filter           opt  low-pass (scale / time-constant)
5. OPERATING diagnostics  value out of range, or conditional   → range DTC (e.g. P0105/6, P0191)
6. derivative       opt  rate-of-change → publishes a "<x> D" channel  (e.g. TPS D %/sec)
7. publish               raw channel(s) (V/Ω/Hz)  AND  conditioned channel(s) (kPa/°C/…)
```

Both the **raw** (`AVI1 Voltage`, `AVI1 Resistance`) and the **conditioned** value are published as
channels — both are loggable and consumable.

## Object model

```
NamedSensor
  enabled
  type            temperature | pressure | throttle/position | switch | frequency/speed |
                  lambda | level | flow | composition | generic
  group           Engine | EngineSync | O2 | Transmission | Chassis | Boost | Nitrous |
                  Atmospheric | AirCon | VehicleSpeed | Other        (UI grouping only)
  input_types     legal set, e.g. {analog_voltage, on_board} or {switch, can_device}

  inputs[]                                   // 1..N — usually 1; N for fused/redundant sensors
    InputBinding
      enabled                                // per sub-input (the wheel/GPS/TPS toggles)
      interface  analog_voltage | on_board | digital_freq | switch | can_device
      resource   Assign from shared board pool → conflict-arbitrated (pin_arbiter)
      digital:   edge_select, pull_up, sensor_type (hall/…), frequency_time_constant
      freq:      pulses_per_unit (+ Calibrate learn), teeth, max_derivative (plausibility)
      can_device ref to a defined CAN device + message (device is itself health-DTC'd)
      calibration  per-input transfer table (raw↔units) or pulses/unit

  source_strategy  identity | pick(i) | average | min | max | group(undriven|driven)
                   // the recurring fusion primitive: vehicle-speed wheels, dual TPS, dual APP

  filter           opt   low-pass scale / τ
  derivative       opt   → "<x> D" rate channel

  diagnostics[]    DiagnosticCheck (below)

  outputs[]        1..N channels (flex-fuel = 2: ethanol% + fuel temp); raw + conditioned

DiagnosticCheck
  kind          raw_min | raw_max | operating_min | operating_max | operating_conditional
  enable
  threshold(s)  Volts (raw) | engineering units (operating)
  preconditions optional: min_rpm, precondition_channel, min_map, min_tps, …   (img: fuel pressure)
  delay_ms      persistence before latching (e.g. 250 ms)
  dtc           OBD-II P-code (default per named sensor)
  severity      None | Low | Medium | High | …   → drives the reaction (consumed by protection)
```

### Input cardinality — three shapes, one model

| Shape | Example | Structure |
|---|---|---|
| 1 → 1 | Coolant Temp | `1 binding + identity → 1 channel` |
| 1 → N | Flex Fuel | `1 binding → ethanol% + fuel temp` |
| N → 1 | Vehicle Speed, dual TPS/APP | `N bindings (each own enable/interface/pin/cal) + source_strategy → 1 channel` |

## Interfaces

- **analog_voltage** — external sensor on an AV/AT pin; transfer table; raw V (+ Ω for thermistor).
- **engine_sync_voltage** — analog voltage sampled many times across a **crank-angle window and
  averaged** (cylinder-pressure-pulsing sensors). The **Averaging Window** is a *group-level*
  setting (e.g. "2 Cylinders / Rotors"), shared by all engine-sync sensors — not per-sensor. Some
  sensors are **locked** to this mode (Manifold Pressure is always engine-sync); others select it
  vs plain `analog_voltage` (Exhaust Manifold Pressure, Fuel Pressure). Implementation deferred —
  model as an interface choice now, build the windowed acquisition later (plain analog first).
- **on_board** — an ECU-internal resource (on-board MAP / pressure sensor `OBPS`).
- **digital_freq** — frequency/pulse input: `sensor_type`, edge, pullup, frequency time-constant,
  `pulses_per_unit` + Calibrate-learn, teeth, `max_derivative` plausibility.
- **switch** — digital on/off: edge select, pull-up, debounce.
- **can_device** — a value from a **defined CAN device** (wideband controller, thermocouple-amp
  box, IO expander, ABS, IMU, dyno). Each device carries its own presence/health DTC + severity,
  terminating-resistor config, and bus selection.

## Diagnostics, DTCs and the Engine-Protection boundary

Detection lives **in the sensor**; reaction lives in **Engine Protection**. The contract between
them is a **DTC code + severity**.

- **Raw checks** (Volts, at the rails) — open/short / circuit faults → "circuit low/high" P-codes.
- **Operating checks** (engineering units) — plausibility/range → "range/performance" P-codes.
- **Operating conditional** — gated by preconditions (`min_rpm`, precondition channel, `min_map`,
  `min_tps`) + a `delay_ms`. This is how state-dependent protections (low fuel pressure *under
  load*) are expressed **on the owning sensor**, not in a central monitor.

**Consequence (agreed):** the bulk of EngineProtection's old threshold monitors — including simple
overtemp/overpressure *and* the conditional ones — become **per-sensor diagnostics emitting
DTC+severity**. EngineProtection shrinks to:

- a **severity → reaction policy** engine (CEL / limp / fuel-cut / spark-cut / latch / log), and
- the genuinely **cross-sensor** strategy a single sensor can't own (e.g. derate from knock + IAT +
  lambda together).

Disabled sensors raise no DTC, so protection can never trip on an unconfigured input (kills the
phantom-timeout class of bug).

### DTC layer

A small new subsystem: an **OBD-II P-code namespace** with `{code, default per-check assignment,
severity}`. Sensors emit DTCs; the fault log stores them; `CanBroker` already serves OBD Mode 03
(report) / 04 (clear). Standard P-codes keep it scan-tool compatible.

## Wiring & resource arbitration

The Wiring page binds each input to the **shared board resource pool** (`s_capture_resources[]` /
ADC pool) via Assign/Clear, showing the pin + wire colour. Conflicts are detected **across the
whole config** ("APP 2 Input conflicts with MAF 1 Input") and surfaced — this is the existing
`pin_arbiter` + `FAULT_PIN_CONFLICT` path. No sensor owns a pin sideways; all claims go through the
pool.

## Generic / user-defined sensors

A **generic** sensor is the same `NamedSensor` object with a user-named output channel and a chosen
type/interface. Consumers reference channels (named or generic) through a condition/expression
layer (Traction Control's "When `Generic Sensor 1 Value` is … "). In jayecu the **Lua plane** is
that expression layer over the same channel registry — `signalRead`/`signalWrite` by name.

## The catalog (Tier 2, full scope)

Each entry below becomes a schema row: `{name, type, group, default channel(s), interfaces,
default DTCs}`. Type legend: **P** pressure · **T** temperature · **SW** switch · **F**
frequency/speed · **POS** position · **L** level · **λ** lambda · **FL** flow · **C** composition.

**Engine Synchronous** (crank-windowed averaged ADC; some locked to engine-sync):
Manifold Pressure (P, locked sync) · Exhaust Manifold Pressure (P) · Fuel Pressure (P)

**Engine:**
Oil Pressure (P) · Oil Pressure Switch (SW) · Coolant Pressure (P) · Fuel Pressure Pre Filter (P) ·
Crank Case Pressure (P) · Air Temperature (T) · Air Temperature Pre Intercooler (T) ·
Oil Temperature (T) · Oil Temperature Return (T) · Oil Level (L) · Coolant Temperature (T) ·
Exhaust Gas Temperature (T) · Coolant Temperature Return (T) · Coolant Level (L) ·
Coolant Flow Switch (SW) · Fuel Temperature (T) · Flex Fuel Composition/Temperature (C → 2 ch) ·
Flex Fuel Composition (C) · Mass Air Flow (FL) · Fuel Flow (FL) · Knock Detection (special)

**Vehicle Speed:** Vehicle Speed Sensor (F, N→1 fused: 4 wheels + driveshaft + GPS)

**O2:** Wideband O2 (λ, CAN-device or analog) · Narrowband 1 (λ) · Narrowband 2 (λ) · O2 Heater (out)

**Transmission:**
Transmission Line Pressure (P) · Torque Converter Pressure (P) · Differential Oil Temperature (T) ·
Transfer Case Pressure (P) · Transmission Input RPM (F) · Transmission Oil Temperature (T) ·
Clutch Pedal Switch (SW) · Clutch Pressure (P) · Reverse Switch (SW) · Neutral Switch (SW)

**Air Con:** Air Conditioner Pressure (P) · Air Conditioner Temperature (T)

**Chassis:**
Brake Pressure Front (P) · Brake Pressure Rear (P) · Brake Pedal Switch (SW) ·
Brake Fluid Level Switch (SW) · Handbrake Switch (SW) · Power Steering Pressure (P) ·
Power Steering Switch (SW) · Steering Angle (POS) · Shock Travel Sensors (POS) ·
Ride Height Sensor Front (POS) · Ride Height Sensor Rear (POS) · Wheelie Bar Pressure (P) ·
Wheelie Bar Pressure Left (P) · Wheelie Bar Pressure Right (P) · Vehicle Dynamics (special)

**Boost/Turbo:**
Boost Pressure (P) · Boost Pressure Pre Intercooler (P) · Wastegate Pressure (P) ·
CO2 Bottle Pressure (P) · Wastegate 1 Temperature (T) · Wastegate 2 Temperature (T) ·
Turbo Speed 1 (F) · Turbo Speed 2 (F) · Wastegate Valve Pos 1 (POS) · Wastegate Valve Pos 2 (POS)

**Nitrous:** Nitrous Pressure 1–4 (P)

**Atmospheric:** Barometric Pressure (P) · Ambient Air Temperature (T) · Humidity (special)

**Other:**
Fuel Tank Pressure (P) · Fuel Level 1 (L) · Fuel Level 2 (L) · Track Temperature (T) ·
Electrical Load Switch (SW) · Tumble Generator Valve 1–2 (POS) · Rotary Trim Module 1–6 (special) ·
ABS Mode (CAN) · ECU Temperature (T, on-board)

## Mapping onto existing jayecu primitives

| Concept | Existing piece |
|---|---|
| Calibration breakpoint table | resizable table/curve editor (TS + storage) — see [[resizable-tables]] |
| Filter / oversampled raw | ADC DMA + oversample + EMA — see [[adc-dma-rework]] |
| Wiring → Assign board pin | board resource pool + `pin_arbiter` (`s_capture_resources`) |
| Output channel namespace | `SignalBus` (the `signals:` catalog, to be reinstated as sensor outputs) |
| Frequency capture | EXTI/TIM5 capture path from the trigger subsystem |
| DTC + severity → OBD | `CanBroker` OBD Mode 03/04 + RAM fault log |
| Condition/expression consumer | the Lua plane (`signalRead`/`signalWrite`) |
| Catalog-of-functions architecture | [[modular-platform-architecture]] |

## Schema / codegen shape

**Built (config_version 20):**
- `sensor_types:` (Tier 1) — id, units, `raw`, ordered `stages`, `diagnostics` kinds, `sane` window.
- `sensors:` (Tier 2 catalog) — one flow-map row per named sensor: `type`, `group`, `channels`
  (default `[id]`), `interfaces`, optional `locked`, optional `dtc` (P-codes per check). The first
  nine rows are ordered to keep channel indices legacy-stable (`map_1=0 … battery=8`).
- `extra_channels:` — pure-virtual channels (lua gauges, boost_est, launch_sw, gear).
- Codegen: `derive_channels()` builds the channel namespace from the catalog → `signal_ids.h`
  (the old `signals:` is gone; channels are never hand-listed). `gen_sensors_catalog_h()` →
  `generated/sensors_catalog.h`: `SensorType` / `SensorGroup` / `SensorInterface` enums + a const
  `SENSOR_CATALOG[]` of `SensorDescriptor{ id, name, type, group, primary_channel, interface_mask,
  locked_interface, channel_count, dtc_raw_min/raw_max/op_min/op_max }`. 92 sensors, 105 channels.

**Built (Tier 3 — per-sensor config, config_version 21):**
- The `Sensors` module is a single config-array `sensor`, `count_from: sensors` → one row per
  catalog entry (codegen resolves `count_from` to `len(schema[key])`, so the dimension tracks
  `SENSOR_COUNT` with no hardcode). `config.sensors.sensor[i]` configures `SENSOR_CATALOG[i]`.
- Element `SensorConfig` (151 B): `enabled, type, interface, source` (board resource idx), `flags`
  (invert/pullup/edge/derivative-enable), a RESIZABLE calibration curve (`cal_n` live points into
  `cal_raw[16]` / `cal_val[16]`, ADC counts → engineering value ×type scale), the diagnostic checks
  (`diag_enable` bits, `diag_raw_min/max`, `diag_op_min/max`, `diag_severity`, `diag_delay_ms`,
  `diag_stuck_ms`, `diag_max_deriv`) and a `precond_expr` bytecode gate.
- There is deliberately NO `filter_tau_ms` and no per-sensor rate. A sensor publishes what it
  measured: smoothing is the consumer's decision, because only the consumer knows whether it is
  fuelling off the value or closing a 1 kHz loop around it. The cadence comes from the sensor's
  TYPE (`SENSOR_TYPE_CATALOG[].update_hz` — temperature 5 Hz, percent 200 Hz, switch 50 Hz), raised
  by any consumer's `sigrate::need` claim, never lowered.
- 121 rows → `sensor[]` 18271 B. Wired into `ecu_config.h` and `default_config.cpp` (uniform safe
  defaults — disabled, unassigned).

**Built (runtime pipeline — `firmware/Sensors/Sensors.{h,cpp}`):**
- An `EngineModule` that walks `SENSOR_CATALOG[i]` + `config.sensors.sensor[i]` each frame and runs
  acquire → raw-diag → calibrate → filter → op-diag → publish to the sensor's primary channel.
  Interface resolution: user choice → catalog `locked` → lowest allowed bit. Covers ANALOG
  (voltage/engine-sync/on-board) and SWITCH inputs; 2-point linear cal; EMA filter; the four
  diagnostic checks set the channel `valid` flag and a per-sensor `healthy(i)` accumulator.
- Wired back into `EngineTask` as the first (producer) module; re-added to the firmware CMake.
- Host test `tests/test_sensors.cpp` (registered) — passes: cal, battery, raw-diag-invalidates,
  switch debounce, EMA filter.

**Built (frequency interface — config_version 22):**
- `platform_read_freq(pin)` added to the HAL (stub returns 0; STM32 returns 0 pending a real
  per-pin timer-capture facility). Sensors handles `IFACE_DIGITAL_FREQ`: read Hz → raw-diag (window
  reused as Hz) → 2-point linear Hz→units → filter → op-diag → publish.

**Built (EngineProtection rework — severity → reaction):**
- Sensors tracks per-sensor worst severity from the packed `diag_severity` of tripped checks and
  exposes `worst_severity()` (inline) + `channel_enabled(SignalId)` (inline, so protection needn't
  link Sensors.cpp).
- New fault `FAULT_SENSOR_DIAG` (bit 15; `fault_actions` 15→16). EngineProtection
  `set_sensors(&sensors_)`; maps worst severity → reaction (1=warn, 2=fuel cut, 3=fuel+ign cut) and
  raises FAULT_SENSOR_DIAG for logging/telemetry + global enable via `fault_actions[]`.
- Phantom-timeout fix: the bus-age sensor sweep is gated on `channel_enabled` — an unconfigured
  input no longer times out (gate open when no Sensors module injected = legacy behaviour).
- Host tests green: `test_sensors` (8 cases incl. frequency, severity, channel_enabled) and the
  existing `test_engine_protection` — full suite 20/20.

**Built (per-check DTC emission):**
- Sensors records, per sensor, the worst tripped check's catalog P-code (`dtc_[i]`) and exposes
  `active_dtcs(out, max)` (inline). `CanBroker::set_sensors()` (wired in EngineTask) lets
  `ObdResponder` Mode 03 report the **real catalog P-codes** (e.g. CLT op_max → P0116) ahead of the
  manufacturer `0xC100+id` fault-log codes; the aggregate `FAULT_SENSOR_DIAG` is skipped in Mode 03
  since the per-sensor P-codes already represent it. Tested in `test_sensors`.

**Built (TS layout — generated):**
- `synthesize_sensors_ui()` in codegen builds the Sensors module's TS layout from the catalog
  (too large to hand-author): one dialog per sensor (`sensor_<id>`) exposing all Tier-3 fields, and
  a grouped `&Sensors` menu (a groupMenu per `group` listing its sensors). 92 dialogs, 11 groups —
  rendered into `ecu.ini` by the existing aggregator.

**Built (TS niceties):**
- `interface` is a dropdown — codegen emits a `SensorIfaceSel` enum + `SENSOR_IFACE_SEL_TO_BIT[]`
  in the catalog and a matching `sensor_interfaces` option list; the field stores the selector index
  (0=Default), and `active_iface()` maps it to a bit. Options: Default / Analogue Voltage / Engine
  Sync Voltage / On-board / Frequency / Switch / CAN Bus.
- Per-group **Enable/Disable…** overview dialog (toggle many sensors at once) listed first in each
  group menu; per-sensor config fields are gated `visible` on that sensor's `enabled`.

**Built (frequency capture — `FreqCounter`):**
- `firmware/Sensors/FreqCounter.h` — period-based Hz from edge timestamps, staleness→0, wrap-safe;
  host-tested (`tests/test_freq_counter.cpp`: 1 kHz/50 Hz/no-signal/stopped/wrap).
- STM32 `platform_read_freq(pin)` reads a per-pin `FreqCounter` (via a `micros()` from HAL tick +
  SysTick); `platform_freq_on_edge(pin, now_us)` is the edge-ISR seam (declared in the HAL, no-op in
  the stub). **Bench-pending:** routing each frequency pin's EXTI/timer-capture IRQ to
  `platform_freq_on_edge` — until then no edges arrive and read_freq returns 0.

**Pending (next increments):**
- CAN-device interface (channels populated by CAN RX, not read in Sensors); derivative publish.
- File-loadable **breakpoint curve** calibration (replaces the 2-point linear); per-sensor
  catalog-seeded defaults (type `sane` window, DTC presence) instead of uniform.
- Retire the legacy per-`SIG_*` timeout faults entirely (now superseded by sensor diagnostics).
- `source` as a per-interface pin dropdown (needs dynamic pool selection by interface) + binding it
  through the board resource pool / `pin_arbiter`; bench the frequency-capture IRQ wiring.

## Open / deferred

- **Engine-synchronous sampling** (crank-windowed averaged ADC for MAP/fuel/exhaust pressure) —
  model as an interface flag now; implement the windowed acquisition in a later phase (plain
  analog first).
- **Knock / Vehicle Dynamics / Humidity / Rotary Trim** — `special` types with bespoke pipelines;
  spec each when implemented.
- **CAN device library** — the set of supported CAN sensor devices (wideband controllers,
  thermocouple amps, IO expanders) and their message maps is its own catalog; first-class but
  populated incrementally.
- Exact **severity levels** and the reaction policy table (severity → CEL/limp/cut/latch).
