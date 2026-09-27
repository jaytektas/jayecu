# Configurable Table Engine (design of record)

Replace the per-table, compile-time-fixed-axis tables with ONE generic, runtime-configurable table
engine. The tuner picks any signal for any axis, enables 1/2/3 dimensions, and edits the breakpoints; the firmware reads the configured channels
and interpolates.

## The model

A **table** = a typed cell grid + up to **3 axes**. Each **axis** =
`{ channel: SignalId (tuner-selectable), breakpoints: float[], enabled: bool, n: live count }`.

- **X** required. **Y** optional (≤ 32 bins). **Z** / depth optional (≤ 8 bins). (A 32/32/8 grid;
  our `table_defaults: {axis_max: 32, depth_max: 8}` already anticipated this.)
- **Float breakpoints** — any signal value fits (rpm, kPa, %, lambda, °C incl. negative, mV…).
- **Channel-driven lookup** — the interp reads `bus.get(axis.channel)` for each *enabled* axis.
  Disable Z → use the z=0 plane; disable Y → use the y=0 row. So 1D/2D/3D is a runtime property.
- **Cells stay typed** (uint16 / int16 with a scale) — only the axes go float.
- The air model's **computed** load (SD's measured MAP, the blend's effective MAP) is **published as a
  signal** (e.g. `SIG_FUEL_LOAD`), so the VE / target-lambda tables are channel-driven like the rest —
  no module reaches past the table engine.

## Firmware

One generic eval backs every table:

```
float table_eval(const TableRef& t, SignalBus& bus);   // reads t.axis[k].channel, interps
```

- `TableRef` = pointer to cells (typed) + the 3 axis descriptors (channel, breakpoints ptr, n, enabled)
  + the cell scale. Built once from the generated config (which is a flat POD).
- Interp picks trilinear / bilinear / linear from the enabled-axis count. Axes are float, so one
  interp path covers every table (no more u8/u16/i8/i16 axis variants).
- Cell layout: row-major `cell[z*(Yn*Xn) + y*Xn + x]` at the LIVE strides (Xn/Yn), max-allocated.

## Schema / codegen

A table declares its **default** axis channels + which axes exist; the channel + enable + breakpoints
become live config. Per table the codegen emits:
- cells `[Xmax*Ymax*Zmax]` (typed),
- 3 axis breakpoint arrays `float[Xmax] / [Ymax] / [Zmax]`,
- `<table>_x_src / _y_src / _z_src` (uint8 SignalId), `<table>_y_en / _z_en` (uint8),
- `<table>_x_n / _y_n / _z_n` (uint8 live counts).
- `.ini`: TableEditor `xBins/yBins/zBins` + `maximumElements`; the editor follows the configured axis.

## Staged build (each stage compiles + bench-validates; defaults preserve today's behaviour)

1. **Engine + codegen**, applied to ONE table end-to-end (clt_corr → 2D CLT×MAP) as the proof: the
   generic schema form, the float-axis 3-slot table generation, the firmware `table_eval`, the `.ini`
   editor. clt_corr's MAP axis off by default = today's curve in row 0.
2. **Publish the fuel load signal** + migrate VE / target-lambda to the engine (SD/blend feed the
   load signal). Migrate ign_table.
3. **Z / depth (3rd axis)** — trilinear interp + the depth config; flex composition as VE's Z (the
   base-fuel-vs-ethanol surface). Retire predicted_map's bespoke path onto the engine.
4. **Cleanup** — delete the old per-type interp zoo (interp2d_u16/i16/u8/i8, interp1d_*) once every
   call site is on `table_eval`; the studio table editor honours configurable axes.

## Decisions locked (user)
- Full generic engine (not a clt_corr-only special case). Float axes (any signal, any axis).
- Up to 3 axes; X req, Y ≤32, Z ≤8. Absolute pressure (not gauge) for now.
- Defaults = current channels so the migration is behaviour-neutral until the tuner reconfigures.
