# Outputs are physical outputs

Status: agreed with Jason 2026-09-18. It replaces the first build ("outputs are pins", reverted the same
day), which used cylinder tick boxes, an engine-outputs wizard dialog and firmware map validation.
Jason rejected that build as too complex.

## The model

- `outputs.output[i]` **is** physical output *i*, in board order: IGN1–12, LS1–22, HS1–8 (42 rows on
  jaytek_v1). There is no `pin` field. The H-bridges are not rows: each is a three-pin device owned by
  the HBridge module.
- A row's **function** is None / Ignition / Injector / Generic. Only an IGN pin can be a coil and only an
  LS pin can be an injector (the meta's `element_options` says which functions each pin can take).
- A coil or injector row names **one cylinder value** (`cylinder`): Cylinder 1–12 (the rotor on a
  rotary), All Cylinders, Bank 1 or Bank 2. It also has `inj_stage` (injectors) and `ign_plug` (rotary
  leading or trailing).
- A **tachometer is a Generic output**, set up from the `tachometer` output template: a PWM carrier
  of `rpm * ppr / 60`, 50 % duty, off below a squelch rpm. The TachOutput module and `tach_hz` are
  gone.

## Who assigns

**The studio, and only the studio.** It lays the coil and injector rows out when a setting that decides
them changes: cylinder count, `engine.ign_mode`, number of stages, a stage's mode, or a grouped stage's
`num_outputs`. The user can then edit any pin on that pin's own page. The standard layout is by cylinder
number:

| Setting | Rows |
|---|---|
| Coil-on-plug | IGN*n* = cylinder *n* |
| Wasted spark | one coil per companion pair, named by the pair's lower cylinder, in cylinder order (1-3-4-2 → IGN1 = 1, IGN2 = 2; Chevy V8 1-8-4-3-6-5-7-2 → 1, 2, 4, 5) |
| Distributor | IGN1 = All |
| Rotary | leading IGN*r* / trailing IGN(4+*r*) = rotor *r*; distributor IGN1 / IGN5 = All |
| Injectors | a contiguous LS block per stage from LS1; per-cylinder modes LS(base+*c*) = cylinder *c*; Bank splits `num_outputs` between the banks present; Multi-Point = All |

The firing order never moves an output.

**The firmware** fires exactly what the rows say (`OutputMap.h`, `EventScheduler::resolve_binding`). It
allocates nothing, validates nothing and writes nothing back. A cylinder that no row names gets nothing.
The one thing it works out at run time is the **wasted-spark companion**. Under `ign_mode = Wasted Spark`,
each coil also fires the cylinder half a cycle from its row's cylinder, taken from the TDCs the firing
order produced, so a changed firing order re-pairs the coils. A changed coil or injector row applies at
the next stopped reconfigure (`EnginePositionHal::output_map_pending`).
