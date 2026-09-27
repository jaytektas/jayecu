# Fuel trim + VE autotune — design

Status: **built**, both halves. Supersedes an earlier draft of this document that described a
multi-scope, telescoping, per-cylinder trim; everything in the "what goes" section below is gone from
the tree. What the firmware actually shipped differs from this design in three deliberate places, and
they are marked **BUILT DIFFERENTLY** where they occur.

## Problem

Two things are wanted and they are not the same thing.

1. **The ECU must correct and learn on its own.** It is standalone. The studio is not connected when
   the car is being driven, so closed-loop correction and the memory of it belong in firmware. This
   is the primary requirement and nothing about the studio changes it.
2. **The studio needs a VE autotuner** for ECUs it drives through an imported definition, which
   have no learned trim of their own. Since it will exist, the native ECU may as well be able to use
   it too.

The firmware half is the important half. The studio half comes for free.

## What went wrong the first time

The previous design had fifteen learned scopes (overall, two banks, twelve cylinders) with a
telescoping decomposition between them, a two-pass sensor priority, per-cylinder PI controllers, a
twelve-condition learn gate, a dwell timer, and a plan for a separate authority map. None of it was
justified by evidence — a twelve-cylinder learned surface fed by one wideband can never be filled.

The design below is smaller than what is currently in the tree everywhere: one table on the fuel
table's axes, one gate, one learn rate, two authority limits, and an Apply-to-Base command.

## The design

### STFT — the fast loop

One PI controller on `(lambda - target) / target`, mean of every enabled wideband. **Nothing is
stored.** It is runtime state and it dies with the key.

- `stft_enable`
- `stft_kp`, `stft_ki`
- `stft_max_enrich`, `stft_max_disenrich` — asymmetric, because leaning out is the dangerous
  direction and a wideband that fails lean would otherwise pull fuel until something melts

Runs at `cadence::Lambda` = 1 kHz / 30 ≈ **33 Hz**, against a wideband that updates at roughly 20 Hz.

### LTFT — one table, shaped like the map it corrects

**One learned table. It mirrors `ve_table` exactly**: the same axis arrays, the same signal ids, the
same 32×32×4 allocation. `int16` at 0.01 % per count (±327 %, far beyond any authority the config
permits) = **8,192 B**.

Sharing the axes is the whole point: LTFT cell (i,j,k) *is* VE cell (i,j,k). Apply-to-Base is then a
copy rather than a resample, and resizing a VE axis resizes the trim with it instead of silently
putting the two grids out of step.

- `ltft_enable`
- `ltft_gain` — one number. How fast settled STFT migrates into the table.
- `ltft_max_enrich`, `ltft_max_disenrich` — asymmetric, as above
- `ltft_learn_when` — `type: expression`, the learn gate (below)

**The surface holds the correction it measured, and nothing added to it.** A rich bias was specified
here once and built; it is gone. See open question 2.

**Disabling either trim ZEROES its correction, it does not freeze it.** A trim still applying its
last value after being switched off corrects the engine invisibly and hides the very error an
autotune is trying to measure. Already built.

### The gate is an expression, not a pile of scalars

`ltft_learn_when` is a `type: expression` field — bytecode for the firmware's stack VM, exactly like
`datalog.log_when`. The tuner writes the condition:

    clt > 60 and rpm > 1200 and not dfco_active and fuel_corr_poststart < 1.01

This replaces, in one field: our twelve hardcoded conditions
(`learn_min_clt_c`, `learn_min_rpm`, `learn_max_rpm`, `learn_min_map`, `learn_max_map`,
post-start, ign-cut, launch, shift-cut, pit-limit, DFCO, traction), the `ltft_dwell_ms` timer —
**and the gain map**.

A gain map is there to stop unstable learning at low RPM, and it is what a tuner
asking to "protect idle from autotune" actually needs. But an expression already answers that:
`rpm > 1200` simply does not learn there. A 4096-cell table to say the same thing more slowly is not
worth its weight, and the learn rate goes back to being one number.

### Dual bank without a second table

Bank-to-bank fuelling difference is dominated by injector flow variance and manifold bias — close to
a constant offset, not a surface that varies across the whole operating range. So the common
correction is learned in the one table and each bank holds **one learned scalar**. Four bytes, not
eight kilobytes.

If a tuner finds a bank difference that genuinely varies with RPM and load, the twelve manual
`cylN_fuel_corr_table`s already exist for it and the studio can autotune them.

**A single-bank engine uses the table alone** — there is no separate "overall" concept, and no
special case.

**Delivery.** A per-bank correction reaches the engine through the injector bank grouping, which
`FuelCalculator` already gates: `deliver_bank = mode != MULTI_POINT`. Sequential, semi-sequential and
bank modes can all address the banks separately; **MULTI_POINT cannot**, because every injector fires
on one common pulse. In that mode the two bank scalars must collapse to their mean.

**Risk, stated plainly:** the scalar is a bet that bank difference is flat. If the bet turns out wrong the fix is a second table at +8 KB, which is affordable —
it is simply not simple, so it is not the starting point.

### Per-cylinder is manual

There is no per-cylinder learned trim. Per-cylinder widebands are **reference sensors**: they
publish telemetry and feed the studio, so a tuner who cares can set the manual
`cylN_fuel_corr_table`s from real per-cylinder data. Twelve learned surfaces fed by twelve sensors
was modelling far past the evidence.

### Apply to Base Table

The command the whole thing exists for: fold the learned table into `ve_table` and reset it to zero.

    ve[i][j][k] *= 1 + ltft[i][j][k] / 100

Exact, because the grids are the same grid. The bank scalars fold into nothing and stay resident —
they are a difference between banks, and a single VE table cannot hold one.

The studio drives it: read the learned page, compute, write `ve_table`, write zeros back to the
learned page. `CommsManager.cpp:740,761` already routes the learned region through the ordinary
config r/w block protocol, so **no new firmware command is needed**.

## Memory

STM32F767ZI: **512 KB SRAM** — DTCM 128 KB + a 384 KB region — and a 4 KB VBAT-backed BKPSRAM that
this project deliberately does not use (no battery is fitted, so it forgets on every boot; the
learned region moved to SD totems and `region_cap` was lifted from 4096).

Measured at the current build: DTCM 22,704 B free, RAM 19,344 B free, **42,048 B total**.

| | |
|---|---|
| LTFT table, `int16`, 32×32×4 | 8,192 B |
| Bank scalars ×2 | 4 B |
| Learned region today (13 lambda blocks) | 13,008 B |
| **Region after** | **≈ 14,550 B** |

Roughly +1.5 KB, against 42 KB free. The gain map would have been another 4,096 B in `EcuConfig`;
the expression gate costs 64 bytes of bytecode.

The learned region is plain `.bss` and is never DMA'd (SD goes through FatFs buffers), so it can move
to DTCM if RAM ever gets tight — which would hand its whole footprint back to the 384 K region.

## What goes

- The 12 `lambda_ltft_cyl_*` learned blocks and their 12 PI controllers
- The telescoping decomposition, the scope resolution (`assign_mode`/`assign_to` for the loop), and
  the two-pass bank-defers-to-cylinder measurement
- `offset_stft_authority_pct`, `offset_ltft_authority_pct`
- `ltft_dwell_ms`, `ltft_learn_pct`, and the ten learn-gate scalars — replaced by one expression
- `lambda_ltft_x_axis`, `lambda_ltft_y_axis` and the 28 `_x_src`/`_y_src` selectors — the table takes
  VE's axes
- The per-cylinder STFT/LTFT telemetry signals stay (reference sensors still report), so
  `signal_ids.lock` is untouched and no ids shift

## The studio autotuner

**BUILT** — `Tools ▸ Auto Tune` (`apps/studio-jf/src/ui/AutotunePanel.*`, engine in
`src/model/Autotune.*`, host tests in `tests/test_autotune.cpp`). Bench-validated on the rig with
`tools/bench_autotune.py`: a wideband injected 7.69 % lean of the commanded target at 2200 rpm /
90 kPa produced +7.7 % in the two cells the operating point straddles, and Apply moved the map by
exactly that.

One view, two sources — the difference is where the correction comes from and what committing means.

**Shared:** the grid on VE's axes, the live cell cursor, a hit/weight overlay, the proposed surface,
before/after diff, and an accountability panel — Total / Filtered / Used records, Cells Altered,
Average and Max Cell Change, Active Filter. That panel answers the first question a tuner asks when
autotune appears to do nothing.

**WHICH CHANNELS, FROM THE DEFINITION.** The one thing this section did not say is where the studio
learns what to watch — and hard-coding a channel list would have made the feature un-correctable by
any board. Our schema declares it under `autotune:`; a TunerStudio ini declares the same facts in its
own `[VeAnalyze]` section and the importer converts them, so both arrive as `MetaModel::Autotune` and
the panel has no per-ECU branch. Every name is validated at codegen: a channel that does not resolve
is a build error rather than a panel that silently never records anything.

**The thresholds are the tuner's.** The definition states starting values; the panel makes each one
editable and remembers it, because the figure that stops a rig learning is engine-specific — a bench
with no coolant sensor never clears Minimum CLT, and one at 11.9 V never clears VBatt. Both were true
on the rig this was written on.

**Imported definitions:** the studio computes the correction from logged or live lambda error and writes VE cells.

**Native:** **BUILT DIFFERENTLY, and better.** Seeding the proposal from `ve_table` folded with the
LTFT is not needed: every sample carries the correction the ECU was applying WHEN THAT GAS WAS MADE
(the declared `ego_channels`, `fuel_corr_stft` x `fuel_corr_ltft`) and it multiplies the measured
error, so the proposal is the table's own error whether the trims are running or not. Seeding would
have folded a correction measured now onto cells the engine visited earlier.

The trims should still be off, and the panel offers `Open Loop` to do it and says so while they are
on — but that is now honesty rather than correctness, and it costs nothing if a tuner forgets. The
imported path is the same equation on the same field: an ini names `egoCorrectionForVeAnalyze` for
exactly this.

**Commit is three stages**, matching our existing rule: stream into controller
RAM (opt-in, the `Apply continuously` box) → push the proposal into the table (`Apply`, one undo
step) → burn with the toolbar button that already exists. Only the burn is permanent, and Apply
discards the evidence behind it — it was measured against the cells as they were, and keeping it
would apply the same correction twice.

### Lambda transport delay

A wideband reads gas that left the cylinder some time ago — exhaust transport, plus ~100 ms of sensor
response. It is modelled as a small table sharing the VE axes, ~40 ms at high rpm and load rising
to ~350 ms at idle-ish vacuum.

**BUILT DIFFERENTLY: the firmware has one too**, and it is the right place for the numbers. The
firmware credits its own learning backwards through `lambda.ltft_delay_table`, so the studio READS
that table rather than keeping a second opinion — two answers to one physical question would credit
the same reading to different cells on the two sides of the link. An ECU that states no such table
falls back to a built-in 3x3.

## Open questions

1. **Bank scalar or bank table?** The scalar is the recommendation and the risk is stated above.
2. ~~**Does `ltft_rich_bias` interact correctly with Apply-to-Base?**~~ **ANSWERED: there is no rich
   bias.** It was removed rather than reconciled. Both candidate fixes preserved a field that should
   not have existed: either the engine is tuned correctly or it is not, the fast loop makes up the
   shortfall, and a tuner who wants three percent more fuel raises the TARGET — the one place richness
   is expressed, indexed and visible. A constant buried in a learned store is a fuel adder no map
   shows. It also cost three points of the fast loop's fifteen to hold a standoff that did nothing
   while the loop ran, and Apply to Base Table folded it into the map on every fold, compounding.
   The surface now holds the correction it measured, exactly, which is what makes the fold a copy.
3. **What does the expression gate default to?** Empty must mean something safe. Probably the
   three conditions: warm, past post-start, not in a fuel cut.

## Not built

- **Offline log replay.** The autotuner reads live telemetry only. Working from a recorded log is
  how you tune a car that is not on the bench — the engine
  takes a `Sample` from anywhere and the panel is the only thing that assumes "live", so the work is
  a log reader and a transport, not a rethink.
- **A live ECU through an imported definition.** That path is exercised as far as the contract: an
  ini's `[VeAnalyze]` block imports, resolves, and produces the same `MetaModel::Autotune` the native
  one does (checked in `tests/test_autotune.cpp`). Nothing has yet run against such an ECU on a wire.
