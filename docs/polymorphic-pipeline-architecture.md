# Polymorphic Pipeline Architecture — design spec

**Status:** design / concept agreed. No branch, no code. The runtime sketch below is
illustrative of *shape*, not a literal API to implement.
**Goal:** dissolve the special-case identities — "sensor", "switch", "CAN input", "SENT
device" — into **one** runtime primitive: a polymorphic pipeline. Everything that moves a
value across the ECU boundary, in or out, is the *same* pipeline pointed in a direction. The
[SignalBus](sensors-design.md) is simply where two pipelines meet.

Related: [modular-platform-architecture.md](modular-platform-architecture.md) (the typed
channel + arbitrated-actuator substrate — this doc names the **Input** leg and the universal
pipeline that produces/consumes those channels) and [sensors-design.md](sensors-design.md)
(the type-driven sensor catalog this generalises into an *Input* catalog).

## Why

The firmware grew a `Sensors` module that is, in effect, a universal decoder: one function
with an `if (iface & …)` chain that knows how to read analog mV *and* frequency *and* SENT
*and* pulse-width — and a separate, parallel world where CAN RX handlers decode frames and
write the bus on their own. Two consequences:

- **CAN is special-cased.** A sensor set to `can_device` is *skipped* by the sensor loop
  ("populated by the CAN RX path, not read here"); the CAN handler does extract + math +
  `bus.set` in isolation, with none of the calibration, diagnostics, or filtering the analog
  path gets. Adding a CAN device means hand-writing a new C handler.
- **Decode is bundled into "the sensor".** The `switch(iface)` is a polymorphism turned
  inside-out — a hand-rolled dispatch where there should be composed stages. SENT sits
  awkwardly on a boundary ("is it in or out of the sensor module?") that only exists because
  the decode logic was bundled into a monolith.

The fix is not to bundle harder (a `CanSensorStrategy` next to an `AnalogSensorStrategy` is
just the `switch` wearing a vtable, and it duplicates the conditioning logic). The fix is to
**decompose** the monolith into discrete, composable stages, and notice that inputs and
outputs are the same machine in mirror.

## The trio and the bus

Three roles, one bus. Each role is distinguished **only by its endpoints**.

```
   Input ──►  ┌─────────┐  ──► Output
  (world→sig) │  Signal │     (sig→world, arbitrated)
   Input ──►  │   bus   │  ──► Output
              └────┬────┘
                Module
            (sig→sig, internal)
```

| Role | Endpoints | Examples |
|---|---|---|
| **Input** | world → bus | analog sensor, switch, CAN device, SENT device, on-board baro/battery |
| **Module** | bus → bus | fuel calc, ignition, rev limiter, engine protection |
| **Output** | bus → world | ignition coil, injector, PWM, CAN broadcast |

The bus is **not a component** — it is the rendezvous. An Input pipeline ends at a bus
`Publish`; an Output pipeline begins at a bus `Acquire`; a Module is a pipeline with both ends
on the bus. Consumers never know how a signal was produced, and producers never reach into one
another — that decoupling is the entire point.

## "Sensor" → "Input"

A *sensor* is just the measurement-flavoured name for an **Input**. The precise definition
that keeps "Input" from becoming a synonym for "everything":

> An **Input** is a *boundary producer* — it brings a value across the ECU boundary
> (world → signal). A **Module** is an *internal producer* — it transforms signals already on
> the bus (signal → signal). The boundary crossing is the distinction.

A thermistor, a clutch switch, a WB1 wideband on CAN, the on-board barometer — all Inputs.
`base_pw` from the fuel calculator is **not** an Input; nothing crossed the boundary, it is a
Module output.

What this dissolves, concretely:

- The **`Sensors` module stops being a module** — it becomes *the Input pool*, a set of Input
  pipelines.
- **sensor / switch / CAN-device / on-board are no longer kinds** — they are different
  **Acquire** stages of one uniform Input.
- The schema's **sensor catalog becomes an Input catalog**, and the per-sensor `interface`
  field is simply *which Acquire stage* the Input uses.

## The one primitive: the polymorphic pipeline

A pipeline is a fixed *shape* with polymorphic *stages*:

```
Input:   world → [ Trigger · Acquire · Decode · Condition* · Publish ] → bus
Output:  bus   → [ Trigger · Acquire · arbitrate · Encode · Emit     ] → world
Module:  bus   → [ Trigger · Acquire · …compute… · Publish           ] → bus
```

The stages **mirror across the bus**:

- **Decode** (raw → engineering) on the way in ↔ **Encode** (engineering → duty / PWM / frame)
  on the way out.
- input **Condition** (filter, diagnostics) ↔ output **arbitration + limits** (the "many
  writers, one actuator" stage — *value bus first, then actuator arbitration*).

So one engine, one stage-contract, one live-rebind mechanism covers all three roles.

### The seam: normalize at Acquire

The only type hazard is letting the *raw representation* leak past Acquire (a frame-acquire
feeding a mV-decode). We **eliminate** it rather than guard against it by coupling:

> **Acquire's job is `source → raw scalar`, full stop.** The CAN bitfield extraction
> (offset / length / endianness) *is part of Acquire* — it is how you get the scalar off that
> source, exactly as reading mV is how you get it off a pin.

Once Acquire normalizes, **everything downstream is type-uniform**, so:

- **Decode** (`raw scalar → engineering float`) is *freely composable*. A linear scale does not
  care whether the scalar came off a CAN frame or a freq pin; an N-point curve works on either;
  Steinhart-Hart works on either.
- **Condition** (`float → float + validity`) is *freely composable* — already true.

This gives real reuse that coupling would forfeit:
- *Same Acquire, different Decode:* analog mV → 2-point linear (pressure) **or** N-point curve
  (thermistor) **or** Steinhart-Hart.
- *Same Decode, different Acquire:* `decode_linear(scale)` over a CAN-extracted scalar **or** a
  freq scalar **or** a SENT scalar.

The hazard is replaced by a **build-time contract check**, not a coupling rule: each stage
declares the raw-kind it produces/consumes, and the pipeline builder rejects a mismatched chain
(reuse the producer→catalog validation pattern already in codegen). The intermediate is a small
tagged union, e.g. `{u32 | i32 | f32} + kind`.

### Stage taxonomy

| Stage | Type contract | Composition | Examples |
|---|---|---|---|
| **Trigger** | — | uniform poll-tick by default; event opt-in | poll @ frame-tick · event @ CAN-RX · event @ capture-ISR |
| **Acquire** | source → raw scalar `{u\|i\|f}` + kind | one per source; owns addressing + extraction | pin mV · freq Hz · pulse µs · switch level · cached-frame bitfield · SENT fast-channel · I²C on-board |
| **Decode** | raw scalar → eng float | **freely composed** | linear scale · N-point curve · Steinhart-Hart · ÷N · sentinel→invalid |
| **Condition\*** | eng float → eng float + validity | **freely composed** | EMA filter · raw-window · operating-window · stuck · derivative · staleness |
| **Publish** | → `bus.set` | fixed | `bus.set(signal, value, valid, ttl)` |

(Output side: **Acquire**-from-bus → **arbitrate** (many→one) → **Encode** (eng → actuator
command) → **Emit** to hardware.)

### Trigger: uniform by default

Do not make Trigger genuinely polymorphic per pipeline by default. Make the **poll tick the
universal trigger**, and have the CAN RX path merely **refresh a frame cache**; a CAN
pipeline's Acquire reads the *cached* frame on the next tick. This buys:

- **One execution model** — the whole pipeline runs in the task loop, never heavy work in an
  RX ISR (capture/fire ISRs stay lean, above the syscall ceiling, for the trigger subsystem's
  reasons).
- **Multi-signal frames fall out for free** — a gear + 4-wheel frame is cached once; five
  per-signal Input pipelines each Acquire their own slice. No special "one producer writes five
  signals" case.
- Cost is **≤ one tick of latency** on a CAN signal — negligible for devices broadcasting at
  20–100 Hz against a ~1 kHz Input loop.

A true event-trigger remains *available* as an opt-in for any signal that genuinely needs
sub-tick latency (the crank/cam path is its own subsystem, not a bus pipeline).

## Anchor 1 — the Publish-Invalid rule

A pipeline **does not abort early** when a stage sets `valid = false`. The context flows all
the way to `Publish`, which **actively broadcasts the invalid status** to the bus. This is
load-bearing:

- The **output / arbitration** side learns a source has *gone dark* (so it can drop that writer
  or fall back), which it cannot learn from a silent absence.
- **TTL / staleness** logic downstream depends on a fresh-but-invalid publish to distinguish
  "producer says bad" from "producer stopped".

An out-of-window or stale value still publishes, marked invalid (`bus.set(sig, v, /*valid=*/false, ttl)`).
True early-exit is reserved for the rare case where Acquire finds *no data at all* (e.g. no
cached frame ever arrived).

## Anchor 2 — runtime reconfiguration via the proven lock-free flip

Pipelines are **runtime-rebindable** — a live `interface: analog → CAN` swap on the bench
re-points Trigger + Acquire + Decode with **no reflash**. The mechanism is the *exact*
lock-free double-buffer pointer swap already proven on this STM32F767 in the generic-trigger
live reconfigure ([generic-trigger-design.md](generic-trigger-design.md), and the
`g_force_reconfig` / `reconfigure()` path):

1. Build the new pipeline into the **inactive** double-buffer slot.
2. **Atomically flip** the active pointer (an aligned 32-bit store is atomic on Cortex-M7) so
   the executor switches to the new pipeline between two ticks.

The executor/reader path is therefore **lock-free** — it only ever dereferences the current
active pointer; it never blocks on the writer. This is the same discipline that fixed the
high-RPM race in the scheduler double-buffer.

## Anchor 3 — the memory layout contract

- A pipeline is a **flat, contiguous array of uniform `PipelineStage` POD structs** — a stage
  is `{ fn-ptr, config-ref }`, nothing more. No object graph, no virtual classes.
- Pipeline instances live in a **static pool**, sized from `MAX_*` defines (per the
  derive-sizes rule — never hardcoded). **No heap, ever.** No dynamic allocation in the hot
  path or at reconfigure.
- The configuration engine installs a new pipeline by **direct memory copy** of POD into the
  inactive buffer, performed **within a short interrupt-guarded critical section**, then the
  atomic flip of Anchor 2 publishes it. This **completely eliminates runtime pointer-chasing
  and dynamic allocation** — reconfiguration is a `memcpy` + a pointer store, both bounded and
  deterministic.

Illustrative POD shape (not a committed API):

```cpp
struct PipeCtx {                 // flows down one pipeline, POD, no heap
    uint16_t signal;
    union { uint32_t u; int32_t i; float f; uint8_t bytes[8]; } raw;
    uint8_t  raw_kind;           // tag: u32 | i32 | f32 | frame
    float    value;
    bool     valid;
    uint16_t ttl_ms;
};
using StageFn = void(*)(PipeCtx&, const void* cfg);   // cfg → offset into the flat config struct
struct PipelineStage { StageFn fn; const void* cfg; };// POD
struct Pipeline {                                     // one per active producer, from a static pool
    uint16_t   signal;
    StageFn    trigger;
    PipelineStage stages[MAX_STAGES];                 // fixed capacity
    uint8_t    n;
};
```

> The Qt/`QVariant`/`QVector` sketches used while designing this were **host-side illustration
> only**. The executing pipeline is bare-metal firmware: POD, static, no Qt, no heap, and
> near-zero-cost hot-path logging (compiled-out or the existing `g_dbg` probe style).

## Codegen vs runtime — it is both

This dissolves the "decode-time or runtime" question:

- **Codegen emits the toolbox** — the stage functions, the per-device source-decoder pairs
  (`canbus_wb1`, `generic_wb`, …), and the *default* stage composition per catalog signal.
- **Runtime selects and rebinds** among those codegen'd stages (the live flip of Anchor 2).

Not baked-flat (could not live-reconfigure); not interpreted-from-scratch (too heavy). Codegen
the primitives, runtime wires them.

## Structural Translation Map (Before → After)

A Rosetta Stone from today's code to the pipeline. **No build order here** — this only shows
what each existing piece *becomes*, so the implementation is obvious from the architecture.

### `Sensors.cpp` — the `switch(iface)` block

Today one `update()` loop runs a single big conditional; each limb reads a source and then
shares the same tail (curve, diag, filter). After: each limb is split at the normalize seam,
its halves becoming independent, reusable stage primitives.

| Today (inline limb in `switch(iface)`) | After (stage primitive) |
|---|---|
| `platform_read_ain_mv(source)` | `acquire_analog` (→ raw u32 mV) |
| `platform_read_freq(source)` + freq fuse | `acquire_freq` (→ raw u32 Hz) |
| `platform_read_din` + debounce | `acquire_switch` (→ raw u32 0/1) |
| `platform_read_sent(source, crc)` | `acquire_sent` (→ raw u32 fast-channel) |
| `platform_read_pulse_us(source)` | `acquire_pulse` (→ raw u32 µs) |
| `platform_read_battery_v` / `platform_read_baro_kpa` | `acquire_onboard` |
| `curve_cal(raw)` (N-point breakpoint) | `decode_curve` |
| `raw_diag(raw)` (raw-window) | `cond_raw_window` |
| EMA low-pass (`filt_[i] += a*(…)`) | `cond_ema` |
| operating-window diag (precondition-gated) | `cond_op_window` |
| `bus.set(primary_channel, value, …)` | `publish` (fixed) |

The proven math (`curve_cal`, `raw_diag`, the EMA, the operating-window check) is **already
written as separate functions** — this is *lifting* tested logic into composable stages, not a
green-field rewrite. An analog thermistor Input is then
`[acquire_analog · decode_curve · cond_ema · cond_op_window · publish]`; a CAN lambda Input is
`[trigger:can-rx · acquire_frame · decode_extract+scale+sentinel · cond_staleness · publish]` —
same primitive, different fills.

### CAN RX handlers

Today `rx_generic_wideband` & co. do extract + math + `bus.set` *inside the RX callback*,
bypassing all conditioning. After: the RX handler shrinks to a **thin, hardware-level
frame-cache** that copies the frame bytes into a per-frame cache slot and **fires the
pipeline's async `Trigger` slot** (or simply marks the cache fresh for the next poll tick).
The extract becomes an `acquire_frame` stage; the math becomes a `decode_*` stage; the
conditioning it never had (staleness/sentinel, and optionally op-window) becomes ordinary
Condition stages. The handler stops "running nested logic"; it only caches and triggers.

## The one asymmetry: output fan-in

Inputs and Outputs are mirrors, with one genuine exception. An Input *owns* its signal (one
selected producer per signal). An Output has real **fan-in at the actuator**: ignition timing,
dwell, rev-cut, and a protection limiter all want the *same coil*. That many→one contention is
the **arbitration** stage, and it has **no input-side equivalent**. The detailed arbitration
policy (priority / last-writer / min / max / safety-clamp) is deferred to the actuator
arbitration plane (see [modular-platform-architecture.md](modular-platform-architecture.md));
this doc only fixes its *position* in the output pipeline (between Acquire-from-bus and Encode).

## Open questions (deliberately unresolved)

- **Input vs Module boundary for Lua and virtual signals.** Lua can read the bus *and* bring
  outside data in; a derived/estimated value (e.g. `boost_est`) has no transducer. These sit on
  the boundary line and need a ruling.
- **The stage library enumeration.** The concrete set of Acquire / Decode / Condition / Encode
  primitives that ship, and each one's exact contract — the next design step.
- **Schema shape of an Input catalog.** How the current sensor catalog + `interface` field
  re-expresses as an Input whose `interface` selects an Acquire (and, for bus sources, references
  a device source-decoder).
- **Per-stage config encoding.** How each stage's `cfg` (curve points, filter tau, windows,
  frame offset/len/endian/sentinel) is laid out in the flat config struct and referenced by the
  POD `PipelineStage`.
