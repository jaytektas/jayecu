# Trigger wheel designer — design

**Status:** built, in the studio (`apps/studio-jf`). This is the record of how it works.

**Why it exists:** the firmware's trigger config (`trigger.streams[]` and their cells) is a generic,
low-level description — `primitive=Gap, slots=36, gap_ratio=2, cell=[0]` is "36-1", but nobody reads it
that way. The designer lets a user pick or draw a wheel as a wheel, see it exactly as the decoder will
see it, check it against a capture off the engine, and install it. The decoder itself is described in
`docs/generic-trigger-design.md`.

## Parts

| File (`apps/studio-jf/src/`) | What it is |
|---|---|
| `model/TriggerWheel.h` | The pure-logic core: the `Wheel` data model, the form ⇄ wheel round-trip, wheel → config pairs, config → wheel. No UI, no Cache. |
| `model/TriggerGeometry.h` | Wheel → drawable geometry (tooth and edge angles, gap arcs, cam pulses), mirroring the decoder primitives exactly. |
| `model/TriggerFit.h` | Reads a trigger-log capture and says what trigger system produced it, stream by stream. |
| `ui/TriggerDiagram.h` | The painter: the dial and the per-stream trace, over geometry alone. |
| `ui/TriggerDesigner.h` | The designer: `TriggerLibraryDock` (left) and `TriggerDesignerView` (a centre tab with **Visualizer** and **Settings** tabs). |
| `surface/widgets/TriggerDiagramWidget` | The same picture as a dashboard widget, drawing the wheel currently in the config. |

Tests: `tests/test_trigger_core.cpp`, `test_trigger_library.cpp`, `test_trigger_fit.cpp`,
`test_trigger_log.cpp`, `test_wheel_validate.cpp`. `tests/wheel_emit.cpp` prints the config the
studio would write for a named wheel, so `tools/bench_studio_apply.py` can apply those exact bytes to a
spinning ECU and check that it syncs.

## The wheel

A **wheel** is a crank stream plus zero or more cam streams, with authoring metadata (name, id, TDC
offset, pickup bearing). A **stream** is one decode lane:

- **primitive** — `GAP` (missing-tooth: `slots`, `ratio`, gap indices), `SEQUENCE` (inter-edge spans,
  0.1°), or `WIDTH` (a cam pulse picked out by its width window).
- **rate** — crank (360°) or cam (720°), plus how many times the pattern repeats per engine cycle when
  that is not the default (symmetrical wheels, odd-cylinder distributors).
- **slot** — which of `trigger.streams[0..5]` it goes in. **The slot is the role**: 0 Crank Primary,
  1 Crank Secondary, 2 Cam Intake B1, 3 Cam Exhaust B1, 4 Cam Intake B2, 5 Cam Exhaust B2.
- per-cam facts the pattern cannot carry: edge, nominal angle, and whether the cam is phased (VVT).

The **pickup bearing** is visual only: it is saved with the wheel and drawn, so the dial shows the gear
the way the user sees it, but it never reaches the ECU. The ECU's whole knowledge of the wheel-to-engine
relationship is the TDC offset.

## The library

Two sources, shown together in the library dock:

- **Shipped** — the `trigger_wheels` section of `definition/ecu.schema.yaml`, carried to the studio in
  the meta. The wheels on offer are therefore the ones the connected firmware can decode, not the ones a
  particular studio build happened to contain.
- **User** — `<data>/trigger_wheels.json`, the same shape, written when the user saves a wheel.

Selecting a wheel loads it into the designer to look at or edit. It never writes the config.

## The designer

**Settings** is the form: crank type and its parameters, a second crank stream if needed, cams (type,
slot, edge, phasing), TDC offset. Every edit redraws the Visualizer. Unsaved edits are tracked, and the
studio asks before anything would throw them away.

**Visualizer** draws the working wheel: the dial (the gear from the front of the engine) and the trace
(one lane per stream on the engine-angle axis, which shows how the streams line up over a 720° cycle).
The picture can be dragged into agreement with the engine: dragging the pickup changes the TDC offset,
because the offset *is* the angle between the reference tooth and the pickup. Turning the engine over on
screen is a view control only — never saved, never written.

**Checking against the engine.** When a trigger-log capture arrives, `TriggerFit` fits it and the
measured wheel is drawn as ghost ticks under the drawn one. Nothing changes until the user chooses to
adopt the measurement, because a capture off a misfiring engine is still a capture.

## Installing a wheel

Saving to the library and installing on the ECU are separate acts: a wheel can be worth keeping without
being the one this ECU runs.

**Apply to ECU** (Settings) calls `applyWheelToConfig`, which takes the wheel's config pairs in
engineering units and converts each to the raw value the Cache stores, writing every trigger slot so
that applying a wheel *replaces* the trigger setup rather than leaving old fields behind. The write goes
to the ECU's RAM; burning is the Burn button, as for every other edit.

`capture_index` — which board input the crank and cams are wired to — is deliberately not part of a
wheel. It is wiring, set separately, so one wheel works on any board.

The inverse, `wheelFromConfig`, reads the live config back into a wheel, which is how the dashboard's
trigger widget draws what the ECU is actually running.
