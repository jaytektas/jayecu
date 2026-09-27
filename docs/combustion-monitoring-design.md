# Combustion monitoring — design

Status: **proposed.** Replaces the knock control described in `knock-control-design.md`, which stays
as the record of what is on master today. Software only — the board, ADC3, and KNOCK1/2 are fixed.

Three questions about every cylinder, every cycle:

| Question | Source | Response |
| --- | --- | --- |
| Is it **knocking**? | knock sensor, after the spark | retard ignition |
| Is it **pre-igniting**? | knock sensor, at or before the spark | per-cylinder DTC + protect. **Never retard.** |
| Did it **fire at all**? | crank segment timing | per-cylinder DTC (+ optional cut) |

They are one subsystem because they share per-cylinder identity, crank-angle windows,
cycle-synchronous statistics and per-cylinder fault reporting. They are *not* one detector: two
independent front-ends feed one classifier.

## Why this is a redesign and not a patch

The shipped pipeline's primitive is **one scalar dB per cylinder per cycle**. `KnockDsp::process()`
(`KnockDsp.cpp:11-28`) sums squares across the whole burst and returns `10·log10(mean_sq)`. That
average destroys the phase information which is the *only* thing distinguishing knock from
pre-ignition — they excite the same bore resonance, so no amount of filtering separates them. And
`window_start_deg` is `uint16, 0-180` ATDC, so the window cannot even open before TDC, where
pre-ignition evidence lives.

Two independent walls. Neither is reachable by tuning, and both are load-bearing for everything
below. Hence blank canvas.

---

## Part 1 — The detection primitive

Replace the scalar with a **phase-resolved energy profile**: the burst divided into N buckets
(16-32), each contributing its own RMS, so the output is *where* the energy sat within the window as
well as how much there was.

This is close to free. The Biquad already runs per sample (`KnockDsp.cpp:18-22`); bucketing means
accumulating `sum_sq` into `bucket[i * N / count]` instead of one accumulator. Cost is N floats and
a divide.

Two changes come with it:

- **The window must straddle the spark.** `window_start_deg` becomes signed (BTDC negative) so the
  burst can open before the spark and run well past it. `angle_atdc()`
  (`EnginePositionHal.cpp:597-603`) already adds a signed offset and wraps; the restriction is in
  the schema, not the maths.
- **Each bucket carries its crank angle**, computed from the sample rate and the RPM the burst was
  armed at. The profile is in the angle domain by the time anything classifies it — consistent with
  how the rest of this firmware refuses to hand angles off as time.

Buffer arithmetic is unchanged from the current build: 2048 samples at 281.25 kHz is 7.28 ms,
covering a 40° window down to ~915 rpm, with `s_knock_truncs` counting what the buffer cut short.

---

## Part 2 — The reference, and why threshold-vs-RPM is not enough

**This is the part that decides whether the system works.** You guessed threshold tables vs RPM;
that is the right shape and the wrong variable.

Absolute thresholds fail because the knock-band background is *engine mechanical noise* — valve
seating, injector actuation, piston slap, gear rattle — and it rises steeply with both RPM and load.
A threshold that catches light knock at 2000 rpm is buried in the noise floor at 7000. Chase it with
an RPM curve and you are hand-fitting the noise floor, per engine, per sensor, per mounting torque.
That is what a per-cylinder `gain` trim is really compensating for today.

Production practice, and what this design adopts:

1. **Track the noise floor per cylinder.** Maintain a reference level `R[cyl]` — what this cylinder
   normally sounds like *at these conditions*, when not knocking.
2. **Intensity is a RATIO, not a level.** `I = L − R` in dB (a ratio in linear terms). Threshold `I`.
3. **Never update the reference from a knocking cycle.** A reference that learns from knock chases
   it upward and goes progressively deaf — the classic failure mode.
4. **Threshold `I` against RPM *and load*.** This surface is far flatter and more tunable than an
   absolute one, because the noise floor has already been divided out.

### The reference must be a map, not an average

A single exponential average per cylinder lags exactly when it matters: open the throttle and the
noise floor jumps, the reference trails it, and every cycle in the transient reads as knock. Knock
happens *in* transients.

So `R` is a **learned noise map over (RPM, load), per cylinder** — the same shape as any other table
here, updated cell-locally from non-knocking cycles. A throttle transient moves to a different cell
that already knows its own floor, instead of dragging one number across the whole operating range.

This lands on infrastructure that already exists and is bench-validated: the learned-region SD
totems survive a reset, so a learned noise map persists across key cycles and an engine is not deaf
for its first minutes every start. Cold-start seeding (an unlearned cell) should fall back to a
conservative configured floor and mark the cell unlearned rather than trusting a default.

### Bootstrapping — the trap that only appears once you build it

An unlearned cell has no floor, only the configured seed. Judging against that seed is unfounded,
and one direction of being wrong never clears: **if a cylinder's real floor sits above the seed,
every cycle reads as knock, a knocking cycle is never allowed to teach, and the cell stays pinned to
the seed forever** — permanently deaf and permanently retarding, on an engine doing nothing wrong.
The learn rule and the judge rule deadlock each other.

So an unlearned cell **listens instead of judging**: the first cycle in each cell teaches, and no
verdict is reached. That cylinder is unprotected for exactly one cycle per cell, which is honest —
far better than confidently retarding an engine whose noise floor is still unknown.

A cell whose first cycle happened to *be* a knocking one bootstraps too high, and heals: anything at
or below the floor is not knock by construction, so it takes the clean path and fast-learn pulls the
floor down over the next few cycles. The one case left uncovered is a cell whose every visit is a
knocking cycle — that stays silent, and is worth revisiting if it ever shows up on an engine.

### Consequences worth stating

- **A constant per-cylinder `gain` trim now cancels out entirely.** The gain is applied to the
  measurement and the floor is learned from trimmed measurements, so it shifts both by the same
  amount and the difference — the thing actually thresholded — is unchanged. This is stronger than
  "largely redundant": for detection it is a no-op. Compensating sensor distance is precisely what
  the per-cylinder floor already does. The field survives as a manual override; nothing depends on
  it. Asserted in `tests/test_knock.cpp`.
- The system needs a **learning gate**: only update `R` when conditions are steady, the cell is
  valid, and the cycle was classified clean. Same gate discipline as the wideband trim learn gate.
- A cylinder whose reference is far from its siblings' is itself a diagnostic — a loose sensor, a
  bad injector, a mechanical fault.

---

## Part 3 — Knock

With a profile and a reference, knock is the straightforward case:

- Energy concentrated **after** the spark, in the knock band.
- `I = L − R` over the post-spark buckets exceeds `knock_threshold(rpm, load)`.
- Response: accumulate retard, clamp to `max_retard(rpm, load)`, decay at the recovery rate — the
  existing model, but with both curves becoming real tables instead of the flat scalars that shipped
  (`knock_config.h:27-29`).

The retard seam does not change: `Ignition::apply_knock_retard()` (`Ignition.h:26`), published as
`wk::knock_retard`.

**Optional second band.** A reference band away from the bore resonance discriminates *resonant*
knock from *broadband* clatter — a loose bracket or an injector rattles everywhere, knock rings at
the bore mode. Two Biquads over the same samples, one extra RMS. Worth designing the front-end to
allow it; not worth blocking stage 1 on.

---

## Part 4 — Pre-ignition

Detection is a conjunction, deliberately, because a single criterion produces false positives on a
signal this noisy:

1. **Phase** — significant energy at or before the spark angle. This is the primary discriminator
   and the reason for the whole redesign.
2. **Magnitude** — far above the knock threshold. Pre-ignition produces dramatically higher peak
   pressure than knock.
3. **Isolation** — pre-ignition (particularly LSPI) is stochastic and isolated: one violent cycle
   among normal ones, where load knock is persistent across consecutive cycles.

**Fail toward the protective response.** Any single event above an extreme magnitude ceiling should
be classified pre-ignition-class *regardless of phase*. If we cannot tell, the safe action is the
one that does not retard into a runaway. Retard is the wrong answer to pre-ignition — the spark is
not what lit the charge, so retarding does not address the cause and can worsen it.

### Reporting — your explicit requirement

Per-cylinder DTCs, following the precedent already set for EGT probes
(`ecu.schema.yaml:917-929` — "the person holding the spanner does [care], instead of leaving twelve
cylinders to guess"):

```
PREIGN_CYL_1 .. PREIGN_CYL_12   — P1790..P179B, level 3, DtcSource::PROTECTION
```

**Built as designed, with one thing learned.** Energy fractions are summed in LINEAR power, not dB:
averaging decibels weights a quiet bucket as heavily as a loud one and understates exactly the
concentration being measured.

Manufacturer range because pre-ignition has no standard OBD code. `Dtc.h:22` confirms the code field
is a flat `uint16` where `0x0117 == P0117`, so both ranges live in one namespace.

### Response

Not retard. In escalating order: enrich the affected cylinder, cut load (boost), and cut that
cylinder's fuel and spark.

**Built.** `EventScheduler` now owns both reasons an output can be held off — the global cut (rev
limiter, protection) and the per-cylinder cut — and recomputes the execution mask from the pair, so
neither stamps over the other. The caller expresses CYLINDERS; translating to output channels via
`cyl_cfg_.cyl[c].ign_channel/.inj_channel` is the scheduler's business. `Knock` publishes
`preign_cut_mask` to the bus and `EngineTask` actuates it, which is where every other cut is applied.

**Wasted-spark caveat, and why fuel is cut by default.** In CRANK mode two cylinders share a coil, so
cutting one cylinder's ignition channel necessarily kills its companion's spark. Injection stays
independent — so `preign_cut_fuel` defaults on and `preign_cut_spark` defaults off, and cutting fuel
is the only action that isolates a single cylinder on a wasted-spark engine.

---

## Part 5 — Misfire

**Your instinct is right in substance — with one correction about where to tap it.**

Crank speed *is* the signal. A cylinder that fires accelerates the crank through its power stroke;
one that misfires does not. This is the OBD-II standard method.

The correction: **do not read the PLL error term.** `VirtualTrigger::last_phase_err()`
(`VirtualTrigger.h:114`) is pre-slew, which is the right side of the filter, but the PLL exists to
*reject* this signal — it slews toward the real tooth and folds the rest into `ticks_per_vtooth_`,
so the residual is whatever the filter failed to absorb. Its gain depends on PLL tuning. Worse, it
is a single scalar overwritten every tooth, and the virtual-tooth grid free-runs on the DCO between
real teeth, so grid timestamps between teeth are *synthesised* and carry no measurement at all.

Tap the real teeth instead. `vtrig_feed()` (`main.cpp:186-190`) already receives every real tooth in
ISR context with its period and angular span, on its way to the PLL. That is the natural seam: the
same data, before the filter that is designed to hide it.

### Method

- Define a **segment** per cylinder: the crank-angle span covering its power stroke.
- Accumulate real-tooth periods into the segment to get segment time `T[cyl]`.
- Misfire shows as a deficit in crank acceleration across that segment, relative to a running mean
  and to the sibling cylinders in the same cycle.

### The three things that make naive segment timing fail

1. **Wheel tooth-spacing error.** Manufacturing tolerance means teeth are not perfectly even, and a
   consistently-narrow tooth reads as a permanent misfire on whichever cylinder it lands in. The
   standard fix is elegant and we have the infrastructure for it: **learn the per-tooth error during
   overrun fuel cut.** No cylinder is firing, so every segment *should* be identical; the residual
   is pure wheel geometry. Learn it, store it in the learned region, subtract it thereafter.
2. **Torsional resonance** of the crank and driveline contaminates segment timing badly at
   particular speeds. Needs RPM-indexed compensation, and honesty about residual blind spots.
3. **Transients and rough road** produce accelerations that look like combustion deficits. Needs an
   inhibit gate.

### Availability is a function of the wheel

Resolution is bounded by teeth per segment. A 36-1 gives ~10° — ample. A 4-tooth wheel on a 12
cylinder gives no usable segment at all. **The detector must declare itself unavailable below a
teeth-per-segment floor and raise nothing**, rather than emitting confident garbage. The trigger
library already knows the wheel, so this is decidable at configure time, not runtime.

### Reporting

Standard OBD codes, which are already reserved — `ecu.schema.yaml:855` declares
`{bit: 11, id: misfire, codes: [P0300-P0314]}` with nothing implementing it:

```
P0301..P0312   — cylinder N misfire
P0300          — random/multiple misfire
```

A persistently misfiring cylinder is also a catalyst hazard; cutting its fuel via the same
per-cylinder execution mask is the conventional protection, and should be configurable rather than
automatic.

### The free corroborating signal

A misfiring cylinder is also *quiet* in the knock band. Once the knock front-end exists, its level
is an independent check on the segment-timing verdict — two unrelated physical measurements
agreeing. Cheap, and it costs nothing to design the record so both land in the same place.

---

## Part 6 — Architecture

```
 real teeth (ISR) ──► SegmentTimer ─────┐
                                        ├──► CombustionMonitor ──► classification per cyl per cycle
 ADC3 burst (worker) ──► KnockFrontEnd ─┘         │
                                                  ├──► Knock (retard)   → Ignition::apply_knock_retard
                                                  ├──► DTC raiser       → per-cylinder codes
                                                  └──► protection       → per-cylinder execution mask
```

- **`SegmentTimer`** — ISR, fed from the `vtrig_feed` seam. Accumulates tooth periods into
  per-cylinder segments, applies the learned wheel correction, publishes completed segments to a
  mailbox. No decisions in the ISR — same discipline as `on_knock_window()`
  (`EnginePositionHal.cpp:413-420`).
- **`KnockFrontEnd`** — the existing worker task, producing profiles instead of scalars.
- **`CombustionMonitor`** — task context. Owns the per-cylinder references, the learned noise map,
  the classification, and the statistics. The only place a verdict is reached.
- **Consumers** stay separate and dumb: retard, DTC, cut.

The two front-ends are independent by construction — one can be built, benched and trusted before
the other exists.

---

## Part 7 — Staging

Each stage host-tested and bench-validated before the next, per this repo's convention.

1. ~~**Profile front-end.**~~ **DONE** (`2cf3b7a`). Bucketed DSP, signed window, angle-domain
   profile. Host-measured: 93 dB separation between energy-early and energy-late, while the
   whole-window level cannot separate the same two bursts at all.
2. ~~**Reference + knock.**~~ **DONE.** Learned per-cylinder noise map over rpm x fuel_load in the
   persistent learned region, fast-learn then EMA, learn gate (dwell + min rpm + not suppressed +
   not in fuel cut), ratio thresholding, and real tables for threshold and max-retard. Also moved
   the worker->controller hand-off onto a mailbox: `Knock::on_knock_sense` reaches a verdict and
   mutates the learned map, and main.cpp was calling it straight from the knock worker task while
   `update()` ran in the module task. NOT bench-validated.
3. ~~**Pre-ignition.**~~ **DONE.** Conjunction of phase (energy before the SPARK, which moves with
   the map, so the test is against live `wk::advance` rather than TDC) and magnitude, plus the
   extreme-magnitude override that fails toward protection. Per-cylinder DTCs P1790-P179B at
   level 3, and a per-cylinder cut composed with the global cuts in the scheduler. The whole
   profile crosses the worker mailbox rather than a summary, because the spark angle is a
   module-task fact the burst worker has no business knowing. NOT bench-validated.
4. **Segment timing.** `SegmentTimer`, the overrun wheel-error learn, availability gate. Independent
   of 1-3 and can run in parallel if that suits.
5. ~~**Misfire classification + DTCs.**~~ **DONE.** Deviation from the neighbouring segments (which
   makes acceleration cancel), per-cylinder wheel-error correction learned on overrun, standard
   P0301-P0312 with P0300 when several cylinders are over at once, an rpm window, and an optional
   per-cylinder fuel cut. Bench: 837 segments across a 1500->3000 rpm sweep, zero false positives.
6. **Studio.** See below.

## Part 8 — The studio question you left open

I have not designed the studio surface, because you did not pick one and it does not block stage 1.
But I will state the dependency plainly: **stage 2 is very hard to tune blind.** Setting a ratio
threshold against a learned noise floor means wanting to see, on a real engine, where the energy sat
and how far above its own floor it was. A knock scope that streams the phase-resolved profile is the
natural tool, and the profile is exactly the thing worth streaming.

Firmware-first through stages 1-2 with the CLI is workable, and lets the data model settle before
committing a UI to it. That is what I would recommend, with the scope arriving before anyone tries
to tune thresholds on a running engine in anger.

## Stage 4-5 bench (2026-08-15, 60-2 crank-and-cam @ PHASE sync)

Segment timing, 1 MHz timebase: **25001 / 15003 / 10001 ticks** at 1200 / 2000 / 3000 rpm — exact
against the arithmetic, with **one tick of spread in 25,000** between cylinders. That baseline
flatness is what a misfire has to stand out against.

Misfire classifier: **282 segments at steady 1500 rpm and 837 across a 1500->3000 rpm sweep, zero
false positives**, roughness reading 0 on every cylinder throughout. The sweep is the one that
matters — it is acceleration-cancellation working on real timing data rather than synthetic.

The stim's wheel is mathematically perfect, so there is no misfire on this rig to detect and no
geometry to learn (corrections stayed at 1.000). Host tests cover the positive cases: a dead cylinder
reads +20.9% against its neighbours, an uncorrected 12% wheel bias produces 20 false events, overrun
learns it to 1.090 and the false events go to zero, and a real misfire ON TOP of a corrected bias is
still caught while the merely-crooked cylinder stays clean.

## Bench validation (2026-08-15, jaytek_v1 @ 1200 rpm, 36-1)

`tools/bench_knock.py`. **All pass.**

- **The hardware path runs.** 12,390 knock windows armed, 12,390 bursts captured, **0 dropped, 0
  truncated** — window scheduling, the ISR mailbox, the ADC3 DMA burst, the worker and the DSP all
  work on the real board. `knock_trunc=0` at 1200 rpm is the 2048-sample buffer earning its keep;
  512 would have truncated a 40 deg window here.
- **The floor learns from real bursts.** With nothing connected to KNOCK1/2, all four cylinders
  converged on ~-62.8 dB within **0.5 dB of each other** — no injection involved. That is stage 2
  end to end: window, burst, DSP, mailbox, learned region.
- **Phase discrimination holds on hardware.** The same level 20 dB over the learned floor reads as
  KNOCK after the spark (retard 1.3 deg, nothing cut) and PRE-IGNITION before it (cylinder cut,
  `pre_frac` 99%, zero retard, `P179x` raised at level 3).
- **The extreme override fires** on a 40 dB event placed squarely after the spark, with the phase
  test correctly reporting `pre_frac=0%` — the fail-toward-protection rule doing exactly its job.
- **Cuts are per cylinder**: cylinders 0, 2 and 3 cut independently while 1 kept running.

Two things the bench found that host tests could not:

1. **`on_engine_stop()` never released the cut.** `preign_clear_s` documents 0 as "latch until
   engine stop"; it actually latched until MCU RESET, so a cut cylinder arrived at the next run
   still cut with no way back short of a reboot. Fixed — the stored DTC is the record, the cut is a
   live action and does not outlive the engine.
2. **Knock was silently inert without a TPS**, and chasing that killed the gate entirely. Every
   measurement was gated on `wk::tps` (`suppress_min_tps`), and the suppressed path DISCARDS the
   worker's queue without classifying it, so an unassigned TPS made the module do nothing and say
   nothing. **Throttle position was the wrong proxy twice over:** light load is not the same
   question as no combustion, and on a drive-by-wire engine the ETB also does idle control — the
   plate sits at a few percent AT IDLE, so the 5% default straddled the idle operating point and
   would have flickered detection on and off there.

   The precise condition was already on the bus: DFCO ORs into `wk::fuel_cut`, which states "nothing
   is burning" exactly. And the per-cell learned floor normalises light-load mechanical noise by
   construction — that being the entire point of a floor per operating point — so the throttle
   threshold was doing work nothing needed done. Suppression is now `wk::fuel_cut`; the TPS gate
   survives as an opt-in override defaulting to **off**, and `P1752` raises only when a tuner has
   actually asked for it. Verified on hardware with TPS absent from the bus entirely: floors learned
   normally across all four cylinders.

Still not calibrated: every injected dB is a number the bench typed. What a real sensor's response
looks like is untested.

## Open

- ~~Load axis definition for the threshold and noise maps.~~ **SETTLED: `rpm` × `fuel_load`, the same
  pair `ign_table` uses** (`ecu.schema.yaml:3028-3031` — `x_channel: rpm`, `y_channel: fuel_load`).
  This is better than it first looked: the load SOURCE is already configurable and
  `wk::fuel_load` is its RESOLVED output — "the per-model load that the VE + lambda tables index on"
  (`FuelCalculator.cpp:69`), published once per cycle whatever the fuel model (Speed-Density / MAF /
  Alpha-N / the MAP-prediction blend). So the knock maps inherit that configurability for free, and a
  tuner who has already shaped their spark surface is working in the coordinate system they know
  rather than a second one invented here.
- **Torsional compensation** for segment timing: RPM-indexed table, learned or configured. Needs a
  real engine to characterise, so probably configured first and learned later.
- **Bucket count N**, and whether the profile is streamed at full resolution or reduced before it
  leaves the firmware.
- ~~No module DTC for a missing TPS.~~ **RESOLVED, by removing the dependency.** See the bench notes:
  suppression is now `wk::fuel_cut` (DFCO), the TPS gate is opt-in and off by default, and `P1752`
  raises only when it is enabled. `Knock`'s schema `requires:` should drop `tps` to match.
- **Aggregate misfire without phase.** Segment timing is PHASE-only, because a segment is a named
  cylinder's expansion stroke. At CRANK sync the revolutions are indistinguishable, so no cylinder
  can be named — but a *rough-running* verdict (OBD's P0300, random/multiple misfire) is still
  computable from segment-to-segment variation. Worth having for crank-only engines.
- The knock-band centre still defaults to a flat 7000 Hz (`main.cpp:288`) while the schema help
  advertises a bore derivation. The redesign should either implement `900/(π·bore/2)` or stop
  claiming it.
