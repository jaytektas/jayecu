# Injection mode & fuel-correction delivery — plan

## Decisions

- **Per-stage modes.** Each stage fires its **own** outputs on its **own** `mode` +
  `injections_per_cycle`. Stages **decouple** — a stage-2 in Multi-Point does not ride stage-1's
  sequential timing. (The schema already models this; the firmware only half-implements it — see
  *Current state*.)
- **Drop "batch", add "Bank"** — a grouped mode whose groups are `cyl[i].bank` (the old channel-index
  batch is retired).
- **No `Generic – No Sync`** — unsupported.
- **Per-cylinder correction is Sequential-only.** Retire `cyl_bank_correction_stages` — each stage's
  mode already says what it can deliver.

## Correction delivery — the principle

The finest correction a **stage** can deliver is bounded by *its own* mode's output grouping — you
can only apply a value uniform across each firing group:

| Mode (per stage) | Output grouping | Delivers |
|---|---|---|
| **Sequential** | one output/cyl, own TDC, 1×/cycle | overall + bank + **cyl** |
| **Semi-sequential** | one output/cyl, 2×/cycle | overall + bank |
| **Sequential-Any-Sync** | one output/cyl, *a* TDC | overall + bank |
| **Bank** | outputs grouped by `cyl[i].bank` | overall + bank |
| **Multi-point** | every output together, N×/cycle | overall |

- **Per-cyl = Sequential only.** Semi-seq/Any-Sync keep individual outputs so they carry the coarser
  **bank** term (each hole gets its bank's value), but not a trustworthy single per-cylinder shot.
- **Bank correction is a group value** (one slow collector O2/bank, can't discriminate cylinders), so
  it needs bank-grouped-or-finer delivery. It rides Sequential (per hole) for free, or Bank (per group).
- **Overall** is uniform, rides anything, and is already applied to the base PW **before** staging
  (`mass_mult`, `FuelCalculator.cpp:299-301,316`).

`effective on stage k = correction terms uniform across stage k's own groups; cyl only if stage k is
Sequential`.

## Current state (what's really wired)

The struct is per-stage (`inj_stage[].{num_outputs, mode, injections_per_cycle}`), but:

- `num_outputs` — **per-stage, used**: each stage claims its own LS-pool block (`EventScheduler.cpp:180,243`).
- `mode` — **`inj_stage[0]` only** (`EventScheduler.h:110`, `FuelCalculator.cpp:344`). Stages 2-4's mode is stored, never read.
- `injections_per_cycle` — **`[0]` only** (`EventScheduler.h:117`, `FuelCalculator.cpp:346`).

So set stage-2 to Multi-Point with 3 squirts today and it silently fires on stage-1's schedule — a
meta↔firmware mismatch the per-stage plan closes.

## Scheduler — recommended design (the meaty refactor)

**Today:** `InjEvent { OutputMask mask; OutputMask staged_mask[4]; uint8 ref_cyl; AngleDeg10 base_angle; }`
bundles the primary + every staged injector of one group into one event at one angle; built from the
primary mode; runtime fires `mask` then each `staged_mask[s]` (`SchedulerTypes.h:353`, firing
`EventScheduler.cpp:576-614`). Staged stages are welded to the primary's timing.

**Recommend: flatten to per-stage events.** One event = one stage's outputs at one angle:

```c
struct InjEvent { uint8_t stage; OutputMask mask; uint8_t ref_cyl; AngleDeg10 base_angle; };
```

- `build_inj_events` becomes a **loop over active stages** (`0 .. num_inj_stages-1`); each runs its
  own mode's event generation (the existing per-cyl / grouped code, parameterised by that stage's
  outputs + mode + `injections_per_cycle`) and appends to the shared pool.
- Runtime fires `event.mask` at `event.base_angle` with the PW for **(stage, ref_cyl)** —
  `stage==0 ? frame.cyl[c].inj_pw_us : frame.cyl[c].staged_pw_us[stage-1]` (PW **source**, not baked;
  it re-reads the frame each cycle, as `ref_cyl` does today).

Why it's better, not just different:
- **Uniform** — every event is one stage's firing; drops `staged_mask[]` and the runtime's inner
  staged loop. Simpler and smaller per event (~8 B vs ~24 B with the 4-mask bundle).
- **Decoupled** — a grouped stage gets its own angles instead of borrowing the primary's, which is the
  whole requirement.
- **Correction falls out** — a per-cyl (Sequential) stage's event reads `frame.cyl[c]`; a grouped
  stage's event reads `frame.cyl[ref]` (one PW for the group) — exactly matching what each mode can
  deliver.

Costs: more events in the pool (Σ over active stages, not one bundled set). Re-derive `MAX_INJ_EVENTS`
(`SchedulerTypes.h:93-103`) as a sum over up to 4 stages × their per-stage worst case. Coincident
events (two stages at the same angle) just fire in sequence — no special handling.

## Correction (fuel calc)

Per-stage, keyed on each stage's mode; corrections stay **separable by scope** (never pre-multiplied
into one per-cylinder `trim` applied post-split, as today):
- **overall** → base PW, all stages (already correct).
- **bank** → per-`cyl[i].bank`, on every bank-capable stage.
- **cyl** (manual `CYL_FUEL[i]` table + closed-loop `STFT_CYL/LTFT_CYL`) → **Sequential stages only**.

Sensing self-zeros (absent scope → 0 → ×1.0); delivery is the only gate.

**Resolution.** Closed-loop bank+cyl trims store at **0.01 %/LSB** (`int16×0.01`, ±50 %, `schema:853`);
the manual per-cyl table at **0.1 %/LSB** (`int16×0.1`, `schema:3009`); overall ≈ 0.1 %. The *delivered*
floor is coarser: injector PW is whole µs (`frame.cyl[i].inj_pw_us`), so a fine % trim rounds to 1 µs
(~0.05–0.1 % on a short idle pulse, finer as it lengthens). Tables out-resolve delivery; the floor is a
PW-µs question, on par with the rest of the tier.

## Build list

1. **Enum + schema** (small): `Batch`→`Bank`; delete `cyl_bank_correction_stages`; regen.
2. **Scheduler** (the big one): flatten `InjEvent` to per-stage; per-stage event compilation; add the
   `BANK` (group-by-`cyl[i].bank`) branch; re-derive `MAX_INJ_EVENTS`. Bench-validate delivered edges
   per stage.
3. **Fuel calc**: per-stage, scope-separated, mode-gated corrections; bitmask gone.
4. **Studio/dashboard** rebind (`cyl_bank_correction_stages` binding dies), **tests**, **flash**.

## Open

- Keep per-cyl closed-loop, or ship only the per-cyl manual table and cap closed-loop at bank (the
  mainstream norm)? It already self-gates off without per-port widebands.
