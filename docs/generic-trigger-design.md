# Generic Trigger Decoder — design record

**Status:** BUILT — merged to main (merge `1094abd`, 2026-06-02), then reworked heavily since. This
document describes the decoder **as built**, with the code as the authority. Where the shipped
decoder diverges from what was designed, that is listed under *Deviations from the original design*
rather than quietly dropped, because those are the decisions worth revisiting. Everything below cites
a file so it can be checked. The original 2026-06-02 brief is kept at the end, unedited.

The job is unchanged from that brief: from N signal streams, produce (a) a velocity estimate and (b)
an absolute angular reference, and feed the PLL — and then say honestly whether the position being
reported is one the decoder can prove.

## The shape of it

```
capture channel per stream (ISR, prio 1)
  → EnginePositionHal::gen_edge_trampoline → on_gen_edge(stream, tick)
  → GenericTrigger::on_edge → matcher.feed(T) → fuse()            GenericTrigger.h
  → VirtualTrigger::on_real_tooth (PLL / uniform angle grid)      VirtualTrigger.{h,cpp}
  → EventScheduler (sees only virtual teeth; never learns the wheel)
```

The 1 kHz engine task (`EngineTask.cpp`) reads decoder telemetry, publishes the trigger channels and
runs protections; firing is computed on the per-cycle task. Everything in the chain above runs in the
capture ISR.

## Streams — the slot IS the role

`trigger.streams[]` has one **fixed slot per role**: 0 Crank Primary, 1 Crank Secondary, 2–5 the four
cams (`StreamSlot`, `SchedulerTypes.h`). A stream's identity is where it sits — there is no role field
that can disagree with it, so two streams cannot claim one role, and an engine with phasers on the
intakes alone enables slots 2 and 4 and leaves 3 empty. Absence is `enabled == 0` on the slot:
`build_generic()` skips it and `on_edge()` refuses an edge on an unconfigured slot.

`MAX_STREAMS = 6` (`GenericTrigger.h`) and `TRIGGER_STREAMS_COUNT = 6`
(`generated/modules/trigger_config.h`), with a `static_assert` in `GenericTriggerBuilder.h` so a schema
that outgrows the core is a compile error rather than a silently truncated tail of `streams[]`. A
second `static_assert` pins `StreamsConfig::cell[]` to exactly `MAX_PATTERN_TEETH` (96) for the same
reason — a tune must not be able to carry a pattern the decoder quietly decodes as something shorter.

### Repeats replaced the CRANK/CAM rate

A stream carries `repeats` — how many times its pattern occurs per **engine cycle** — and its angular
period is `cycle / repeats` (`set_repeats`). This replaced the CRANK/CAM binary, which could only
express periods of 360° and 720° and therefore had no room for the wheels that sit between and beyond
them: a symmetrical crank whose pattern recurs twice a revolution (repeats 4), a 12-tooth Honda crank
(24), a V10 with 5-fold symmetry (10), or a distributor on an odd-cylinder engine — 5 pulses per cycle
is 2.5 per revolution, which is not expressible as a crank-rate wheel at all. `repeats == 1` is the
phase (cam-mounted) stream; `repeats >= 2` is a periodic stream that can locate itself inside one
repeat and no further. `repeats == 0` takes the slot's default: `cycle/360` for a crank slot (2
four-stroke, 1 two-stroke, 3 rotary), 1 for a cam slot (`build_generic`).

### The cycle is configured, not hardcoded 720°

`set_cycle()` takes 3600 / 7200 / 10800 decidegrees (two-stroke / four-stroke / rotary) and must be
called **before** the streams are added, because a phase stream's period *is* the cycle and a rotary
needs three revolutions distinguished rather than two. `revs_ = cycle/360`, and `rev_` is a **counter,
not the 720° bit the original model assumed** — a rotary's three e-shaft revolutions cannot be told
apart by a flag, and without that its three rotor faces are indistinguishable. The grid ceiling follows
the longest cycle: `MAX_VTEETH = 108` (1080°/10°), not 72 (`SchedulerTypes.h`).

## The primitive pool — three, not five

| Primitive | Params | Replaces | How it identifies |
|---|---|---|---|
| `GAP` | `slots`, `gap_ratio`, `cell[]` = gap positions (≤ `MAX_ANOMALIES` = 8) | EVEN, DISTRIBUTOR, MISSING_TOOTH, PATTERN_EXCEPT | A uniform base wheel of `slots` teeth with `ngap` missing-tooth gaps at known **present-tooth** indices, each spanning `gap_ratio` pitches (missing teeth + 1). `ngap == 0` **is** the even/distributor wheel (relative lock only). Stays parametric: a 36-1 is two numbers, never a 35-entry cell. |
| `SEQUENCE` | `cell[]` = inter-event angles in 0.1°, up to 96 | ODD_FIRE, cam-pulse phasing (6g72) | Match the cyclic interval **ratio** sequence — rpm-invariant and independent of any other stream's angular resolution, which is what lets it phase a multi-pulse cam sitting on a 120°/tooth crank. Uniform cell ⇒ relative lock (no unique origin); asymmetric ⇒ absolute. |
| `WIDTH` | `width_min/max`, `width_target`, `edge` | WIDTH_IDENTIFICATION | The pulse whose **angular** width — high-time in ticks × the crank's live rate, not a crank-tooth count — falls in `[min,max]` is the reference; every other pulse is ignored outright (it neither anchors nor breaks a lock). Fed both edges; the fusion is fed at the trailing edge, which is why `edge_angle()` adds the measured width back. |

`EVEN` and `LEVEL` from the original pool did not survive as separate primitives: EVEN is a GAP with
`ngap == 0`, and LEVEL (bit-pattern of edge states) was never built — a wheel whose position lives in
its edge states is authored as a SEQUENCE of per-edge spans instead, which is exactly how the shipped
LS1 24x is described (48 inter-edge spans, `edge: 2`, because its 24 evenly spaced edges are the
**falling** ones — counted on the rising edge the same wheel reads as wildly irregular and a 24-slot
even pattern never locks).

Two details in these matchers are load-bearing:

- `WidthMatcher::feed` takes the crank rate as an **unreduced ratio** (`period_ticks`, `pitch_deg10`) and
  divides once, in 32 bits. It previously received an already-integer-divided ticks-per-decidegree, so
  two truncations compounded: at 8000 rpm on a 180-slit wheel the true rate is 2.083 and the integer
  carried 2, and an 80.0° pulse measured 83.3° — an rpm-dependent error sitting directly in the
  anchored position.
- Both matchers keep the last accepted interval *and the angular span it covered* (`last_T_`, `last_gr_`)
  so the next interval is predicted from the tooth just measured rather than from a lagging average.
  Seeding that span with 1 rather than the gap's own pitch made the first locked prediction short by
  the gap ratio and unlocked the wheel on the very next tooth.

## The lock rule: position is proven, or it is not

The largest change since the design was written, and worth reading before the code.

**Acquisition** is as designed: an even wheel locks *relative* after `REQUIRED_CONFIRMS = 3` steady
teeth; a SEQUENCE locks when the argmin cell ratio advances one index three consecutive times; a GAP
locks when the observed inter-gap tooth-count sequence matches the cyclic sequence the wheel describes,
and is absolute from there.

**Once locked, every edge is judged against a prediction, not against a presence test.** Predict the
next interval as the one just measured, scaled by the ratio of the next slot's angular span to the one
just traversed — the same ratio form SEQUENCE uses, which tracks acceleration exactly because it rides
on the immediately preceding tooth (`GapMatcher::feed`, `SequenceMatcher::feed`). Anything outside the
window means *this edge is not the edge that was due*, so the decoder no longer knows where the engine
is: `errored_` + `err_kind_`, `reset()` back to ACQUIRING (wheel config kept), and the fusion turns that
into a sync loss. **There is deliberately no tolerated-miss count any more** — `MAX_MISSES` survives in
both matchers as an unused constant. The old shape checked only gap *presence*, tolerated four
mismatches, and then advanced the angle by the pitch it had **assumed**: the one event proving the
position wrong was handled by stepping forward on the assumption it was right, forever. A chipped tooth
or a marginal VR gap gives one bad tooth every few revolutions, which never reaches four *in a row*, so
sync was never dropped while the angle walked.

The tolerance is stated as a percentage of **one tooth pitch** (`tol = pitch_est * window_pct / 100`), so
the whole classification is one line — *an interval within half a pitch of N pitches IS N pitches*, and
the schedule says what N should be — and the acceptance band shares its 1.5× boundary with the rule that
defines a gap (`T*2 >= nominal*3`) instead of two boundaries that can disagree. A percentage band of the
*interval* could not do that: at ±25% a two-pitch gap accepted [1.5, 2.5] and a tooth accepted
[0.75, 1.25], so the tolerance shrank as the wheel got coarser. SEQUENCE keeps the same knob but applies
it to the predicted interval — see *Open questions*.

`window_pct` follows rpm: `set_rpm()` blends the cranking window (default 75%) into the running window
(schema default 25%) between `kCrankRpm = 400` and `kRunRpm = 1500`, because a starter slows into every
compression and surges over the top — tooth-to-tooth speed swings hardest exactly where the decoder has
least history — while by 6000 rpm inertia has made consecutive teeth near-identical and a tight band
costs nothing. One constant cannot serve both ends: tight enough to catch a marginal wheel at speed is
tight enough that the engine never starts.

Three corollaries, each of which was a real fault:

- **No noise filter in `on_edge()`, deliberately.** Every edge goes to the matcher and is judged against
  the schedule; one that does not belong drops the lock. An earlier version rejected "too soon to be a
  tooth" and carried on decoding, which treats noise as something to be tolerated rather than as
  evidence the trigger cannot be trusted. There is no acceptable amount of noise on a trigger input.
- **The even-wheel path gets the same rule** (`feed_even`). It used to accept *any* interval once locked,
  so a wheel with no missing tooth had no per-tooth validation whatsoever. Every pitch being equal does
  not make every interval right; it makes the check "the same as the last one".
- **The nominal-pitch estimate cannot ratchet shut.** A too-low estimate is a one-way trap: it snaps down
  instantly and only rises on a tooth judged "not a gap", so once a spurious short interval drags it to
  a fraction of a pitch every real tooth reads as a gap and the refresh never runs again — measured:
  three edges of ignition crosstalk and a 36-1 that never re-acquired in 4000+ teeth (114 revolutions).
  No gap on any wheel is longer than its widest configured ratio, so an interval past
  `nominal * (max_ratio + 1)` means the *estimate* is wrong, not the wheel, and re-primes it.

## Absolute or relative — anchors and counters

Position comes from an **anchor**: a stream that can state its own position within its pattern. A wheel
with a unique feature (a gap, an asymmetric cell, a matched width) can; an even one cannot, and no
amount of counting turns one into the other. The fusion carries this as `anchored_` / `pending_anchor_`;
the matchers have always computed the property (`is_absolute()`), and for a long time the fusion threw
it away.

- **Absolute crank stream** — names its own position every tooth: `angle_ = nf + rep_ * period_`.
- **Relative crank stream** — knows how fast the engine is turning and how far it has moved, not where
  it is. It takes its angle from a queued anchor and then advances it by whole pitches. Deriving the
  angle from the pattern origin instead (`angle_ = nf + rep_*period` for every wheel) reported an even
  wheel out by an arbitrary number of teeth forever, at PHASE, with full confidence — measured 45° on a
  clean run on a 24-tooth wheel, 165° after one missing tooth re-locked it somewhere else.
- **The anchor is applied on the next fine tooth, with the sub-tooth remainder measured** as elapsed time
  against the pitch that just closed (`pending_anchor_tick_`, clamped to one pitch). The point is not
  accuracy, it is order-independence: when the reference edge lands on a crank tooth, whether the ISR
  services the cam first or the tooth first decided whether the anchor applied on that tooth or the
  next — a whole pitch apart (measured 15.0° on a 24-tooth even wheel and 2.0° on a 180-slit Nissan,
  flipped by a single tick). A constant offset a timing light can cancel, versus one that alternates.
  When there is no plausible carry to measure it falls back to centring half a pitch: better a coarse
  true angle than a precise invented one.
- **A phased cam cannot anchor** (`s.absolute() && !s.phased_ && has_crank_ && !any_crank_absolute()`). An
  anchor must be a fixed reference and a phaser is a controlled variable — anchoring a featureless crank
  to one injects the phaser's current displacement into engine position, silently, and changes it every
  time the cam moves. A crank with no unique feature whose only absolute source is phased can never
  know where it is; that configuration belongs in the reject list, not running.
- `sync_always` (see the config gate below) is the case where every tooth *is* a sync tooth: this edge
  anchors, and the revolution is not a question.

## Fusion — one clock, one spokesman (`GenericTrigger::fuse`)

- **One crank stream is the clock.** `nominated_crank_stream()` (lowest enabled locked crank slot — which
  is what "primary" *means*) supplies velocity, the fine pitch and the position increments. A second
  crank stream is not a second clock: previously every locked crank stream did all of it, so the
  velocity, fine pitch and increments came from whichever edge landed last — two clocks fighting over
  one position. What a second crank stream *can* do, if it is absolute, is **anchor**, which is exactly
  what a Motronic flywheel's single TDC mark and a GM 3800 coarse track are for.
- **One phase stream speaks for the cycle** (`nominated_phase_stream()`: Cam Intake B1 wherever it exists,
  falling back in a defined order rather than to whichever edge arrived last — on a V engine with a cam
  per bank, both can identify the cycle, and letting the most recent win makes the answer depend on
  scheduling). Every other cam is still decoded, still position-checked against its own bank's nominal,
  still measured for VVT — but its verdict never demotes phase. The cross-check **is** the position
  check, each cam against the *crank's* answer; comparing the cams' revolutions to each other would
  false-fault every normal V engine, because cams sit at different points in the cycle by design.
- **A cam edge is windowed before it may move anything** — when the crank can state its own position
  (`has_crank_ && !sync_always_ && any_crank_absolute()`). Only the reference edge (`at_origin()`) may
  speak; it must be in the **right place** (its nominal, or where the last accepted sighting sat, ±
  `phase_band`) and at the **right time** (consecutive sightings one engine cycle of *crank teeth* apart,
  ±1 — rpm-invariant by construction, costs no timer because the crank *is* the clock, and rejects a
  decoy that is in the right place but the wrong revolution); and phase is granted on the **second**
  sighting, never the first. An edge that fails is *ignored*: it moves nothing and does not take a good
  phase away, and it is counted as a noise edge — visible, but not in the crank's error %. An edge that
  lands in the same wrong place a cycle later is not noise, it is the cam (a jumped chain, a phaser
  stuck out), and that is `PHASE_LOST` (`stray_cam_edge`). Without a stated nominal the cam has simply
  moved, and it is learned again from there.
- **The repeat and the revolution are asked of the crank, not the cam's nameplate.** `A / fine_period_`
  alone picks the repeat from where the cam is *configured* to be: an edge set at 358°, or carried across
  a revolution boundary by its phaser, arrives after the crank has already wrapped, and the engine is
  placed 360° out — PHASE_LOST a cycle later, re-acquire, repeat. Where the crank knows its own position
  the repeat is the one that puts the crank's angle nearest the cam's (right for any cam error under half
  a period), and a WIDTH decoy pulse moves nothing at all.
- **Hierarchy.** Losing the crank takes phase and everything else: the test is
  `has_crank_ ? !any_crank_locked() : !any_locked()`, *not* `any_locked()`, which a still-locked cam
  satisfies — so a crank that unlocked used to leave `level_` reading PHASE unchanged. Phase outliving
  the crank sync it is measured against is the most dangerous state the decoder can be in: full
  confidence in a position nothing is still measuring. The reset is a full discard, not a downgrade:
  level, anchor, angle, velocity and fine pitch all go to zero, because something always eventually
  reads a stale value.
- `level_` asserts what is **known**, not that something locked: `!anchored_ ? NONE : rev_known_ ? PHASE : CRANK`.
- A cam that goes **overdue** is measured in crank teeth — `crank_teeth_ - phase_mark_ >
  phase_expect_teeth_ + 1` — and costs PHASE and nothing else: the crank is proving itself tooth by
  tooth, so the engine keeps running wasted-spark / batch. `phase_expect_teeth_` is derived from the
  wheel (present teeth per period × repeats), never typed in. While the cam is gated out by the rpm band
  its last sighting is carried a whole cycle at a time, or the teeth counted while it was ignored would
  all land at once on the first tooth back inside the band: hundreds of teeth "overdue", PHASE_LOST on a
  healthy cam, every time rpm crosses the band edge.

## Sync level, and the ceiling the configuration sets

`NONE → CRANK → PHASE`. The level drives firing capability through `EventScheduler::set_phase_known()`
and the grid span: `vtrig_reconfigure(ANGLE_360, 36)` at CRANK/NONE, `(cycle, cycle/100)` at PHASE
(`handle_sync_level_change`). Any level change opens a new firing epoch (`sync_epoch_++`), so a schedule
computed against the old position reference can never fire after a re-sync.

`TriggerConfigCheck.h` answers "CAN THIS CONFIGURATION EVER KNOW WHERE THE ENGINE IS?" at configure time,
and returns the **highest level it can ever reach** rather than a yes/no — so "this wheel can never do
sequential" is decided on the bench instead of by a car running badly on one bank. Faults: `NO_STREAMS`,
`NO_ANCHOR`, `ONLY_PHASED_ANCHOR`, `EVEN_WHEEL_COILS`, `EVEN_WHEEL_INJECTION`. `EnginePositionHal` runs it
*before* `wire_from_assignment()` (so the streams it adds take the result up), keeps the ceiling and the
fault, publishes `trigger_sync_ceiling`, and refuses to arm firing at a `NONE` ceiling; `EngineTask`
raises P1652 edge-triggered (a per-frame raise would rewrite the freeze frame every ms) and heals it.

Its `sync_always` result is the distributor case: a single even wheel + `SINGLE_COIL_DISTRIBUTOR` + a
tooth count that divides the cylinder count, on an even-fire non-rotary engine. A uniform wheel cannot
say which tooth is which, and with one coil and a rotor it does not need to — the rotor picks the plug,
so **every tooth is a sync tooth** and the first one syncs the engine. `set_sync_always(true)` drops the
matchers to one confirm and the decoder stops waiting for an anchor that wheel will never produce. It
is engine configuration, so it survives `drop_sync()`.

## Liveness — the decoder owns a clock

Matchers, fusion, sync level and every trigger DTC are edge-driven, so **absence** — the failure that
matters most — was the one thing none of them could see. It used to live in the DCO's match ISR, which
is reset by every sync transition (`handle_sync_level_change` → `reconfigure()`), so between a
CRANK⇄PHASE change and the next real tooth there was no detection at all, and an ECU could hold PHASE
sync and a plausible rpm indefinitely with no wheel connected. `VirtualTrigger.h` now carries a comment
saying *do not put it back*: two implementations of one decision, one of them disarmable by another
layer, is how the original fault survived.

It is now the decoder's own deadline — `EnginePositionHal::arm_tooth_deadline` / `on_tooth_deadline`, on
`assignment_.tooth_alarm`, a compare-match nothing else resets:

- The due time is the decoder's own prediction, `expected_fine_ticks()`: the interval just measured,
  scaled by the ratio of the next slot's angular span to the one just traversed. **Angular on purpose**
  — the tooth after a gap is due *one pitch* later, not one gap later, so any "same as last time"
  estimate is wrong exactly at the gap, the one place on the wheel where being wrong is expensive.
- The arm point is that plus `fine_tol_ticks()` (half a tooth pitch on the stream that carries position),
  clamped to `[1 ms, one crank revolution at kStallRpmFloor = 30 rpm]`. The 1 ms bound guards a
  pathological prediction, not noise; the old 0.1 s bound swamped the prediction entirely, so at 600 rpm
  (a 2.8 ms tooth) every missed tooth was reported 100 ms late or not at all.
- On a fire, **firing is disabled in the ISR** (`scheduler_.set_firing_enabled(false)`) and `drop_sync()`
  runs — not a frame later. The 1 kHz gap was up to 36° of crank at 6000 rpm spent firing on a position
  the decoder had already said it could not prove. The heavier teardown (volatiles unlinked, outputs
  de-energised, grid re-narrowed) follows on the next `service()`, which is soon enough for a coil that
  is mid-dwell and far too late for a spark that is not.
- The rpm **at the moment of loss**, read against `engine.cranking_rpm`, decides whether anything is
  recorded. Every stop ends the same way — the teeth slow, one arrives later than the window allows, the
  deadline fires — and recording that raised P0336, then 2 s later reported every stopped engine's
  trigger wire missing (P0338, Level 3) for as long as the key stayed on. Below cranking speed the sync
  is dropped identically (firing stops) with nothing recorded.
- A missed tooth re-arms at the floor bound purely for the **diagnosis**: a tooth that did not arrive and
  a trigger wire that is gone need the same cut and send an operator to two completely different places,
  and only time tells them apart (`MISSED_TOOTH` → `SIGNAL_ABSENT` / `trigger_absent_`).

`fine_advanced_` is cleared at the **top of `on_edge()`**, not inside `fuse()`. Four ordinary edges never
reach `fuse()` — a WIDTH cam's leading edge (every cam pulse, on every phase-synced engine), a WIDTH
edge with no lead primed, any stream's first edge (and `drop_sync()` clears `primed` on every stream, so
that one recurs at every re-acquisition), and a zero-length interval. While the flag only gated the PLL
feed, a stale `true` merely fed the grid a stale angle, which the slew clamp absorbed; once the stall
watchdog was hung on the same flag (the decoder's own clock), a stale `true` stopped meaning "re-aim the
grid" and started meaning **the engine is still turning**: measured as PHASE sync and a frozen rpm held
for as long as stray edges kept arriving on another stream with a dead crank, and never once reporting
SIGNAL_ABSENT.

## Sync health — kinds, counters, codes

`TriggerErrorKind` (`SchedulerTypes.h`) names **what went wrong**, not which matcher noticed — naming
the primitive made `TOOTH_WINDOW` unreachable on every missing-tooth wheel there is and left the P-code
gated on it as dead code. Values 1/2 are frozen because telemetry and the DTC map are keyed on them.

| Kind | Meaning | Raised by |
|---|---|---|
| `TOOTH_WINDOW` | an interval fell outside the predicted window | SEQUENCE streams |
| `GAP_MISMATCH` | a normal interval where the **gap** was due — a wheel that is not the wheel the tune describes | GAP, locked |
| `MISSED_TOOTH` | a tooth that was due never arrived, or arrived too late where one was scheduled | deadline, GAP |
| `NOISE_EDGE` | an edge that is not a tooth at all (too early; or a cam edge outside its window) | GAP, cam rejection |
| `SIGNAL_ABSENT` | no teeth at all past the floor bound — the wheel stopped or the wire is gone | deadline follow-up |
| `PHASE_LOST` | a cam is not where the crank says it should be, or did not arrive at all | phase checks |

Every fault records **which stream** raised it (`last_error_stream()`, 0xFF = none): a V engine has a cam
and a chain per bank, so "the trigger is unhappy" is not actionable and "bank 2's cam has moved" is.

Two families of counters, deliberately:

- **Per-cycle** — `teeth_last_cycle` / `errors_last_cycle`, snapshotted at a period boundary, giving
  `trigger_error_pct = errors × 100 / teeth` (`EngineTask::read_position_raw`).
- **Free-running, never reset** — `trigger_noise_edges`, `trigger_missed_teeth`, `trigger_phase_lost`. The
  per-cycle pair is snapshotted at a boundary that is itself counted in teeth, so when the teeth stop it
  never updates again: useless for reporting the very fault that stopped them. Phase is a count rather
  than a flag because the fault on a VVT engine is not "phase is down now" but "phase keeps going down"
  — a position check that disagrees with the phaser drops and re-acquires every cycle, which reads as
  healthy in any single sample.

Channels (`ecu.schema.yaml`, published by `EngineTask`): `trigger_teeth`, `trigger_absent`,
`trigger_noise_edges`, `trigger_missed_teeth`, `trigger_phase_lost`, `trigger_fault_stream`,
`trigger_sync_ceiling`, `trigger_error_pct`, `trigger_last_error_kind`, `trigger_last_error_tooth`.
`trigger_teeth` is written *when it changes*, which is what makes `age(trigger_teeth)` mean
"milliseconds since the last tooth" — the LED status needs that because rpm reads 0 without sync.

Codes (`EngineProtection.cpp`; detail in `docs/dtc-codes.md`): P0335 sync loss (sev 3), P0336 tooth
(sev 1), P0337 gap mismatch (sev 2), P0338 trigger absent (sev 3), P0339 noise (sev 1), P0341 phase
lost (SAE, sev 1), P1652 trigger config (L3). `trigger_error_pct_limit` is a **noise-tolerance floor**
(0 = record any single miss). Absence is deliberately *not* gated on that floor, for the same reason the
free-running counters exist; noise and phase use a held free-running count so they read as a fault
rather than a storm.

## VVT — the cam's displacement, measured against the crank

`check_phase_position()` compares the crank's angle **at the cam edge** (`crank_angle_at`: the open pitch
interpolated with the same elapsed-time arithmetic the anchor uses) against the cam's `nominal_angle`.
That residual *is* the displacement, and it is recorded whether or not it passes the band — "how far the
cam has moved" is the measurement, "too far" is a judgement about it. Quantising it to the last tooth
rounded in the fault's favour: on a 36-1, 2°, 5° and 9° of chain stretch all measured 0.0 while 45°
measured 40.0 and passed a 40° band.

The band (`phase_band`) is not one number. A phased cam gets ±`phase_authority` — travel the ECU
commands, symmetric on purpose so a tune never has to state which way its cam travels and cannot fault a
healthy engine by getting that backwards. A fixed cam gets the larger of `phase_allowance` (slop it does
not command: chain stretch, gear lash, torsional wind-up under load — engine properties nothing in the
trigger layer can derive, so they are configured rather than computed) and the decoder's own measurement
floor of one crank pitch. Deriving the band from the pitch **alone** was the structural defect: pitch is
a property of the crank wheel, not of the cam drive being checked, so a coarse crank (a 6g72's three
teeth = a 120° band) silently disabled cam validation entirely.

The residual is published as **advance** — negated once, at the HAL boundary — because the residual is
crank-at-the-edge minus nominal, so a cam that fires earlier reads *negative*; publishing it raw gave the
VVT loop positive feedback (advancing the cam grew the error it was correcting) and it ran to a stop
whichever direction was set. `stream_angle() − nominal` is not a substitute: a WIDTH cam's `angle()` is
its configured target, so that difference is one constant minus another and reports a cam that never
moves however far its phaser travels.

## The PLL feed — what the VirtualTrigger gets

`fine_advanced()` says *this* edge was the position source; the HAL then calls
`vtrig_feed(engine_angle(gtrig_.pll_angle(), trig_offset_btdc, cycle), tick, velocity, fine_pitch, …)`.
`pll_angle()` is mod-360 at CRANK and the full cycle at PHASE. `trigger_offset_btdc` is applied at that
one boundary and nowhere further in, so everything downstream — grid, scheduler, knock window, segment
timing, telemetry — works in **engine** angle (cylinder 1 TDC = 0) and there is never a second coordinate
system, which is the bug that follows from having two. The offset is a *position declaration*: positive
means the reference tooth occurs that many degrees BTDC #1 (a 2JZ 36-2 is 155); it is not an advance
control. `sensor_angle` is stored for the studio's drawing of the gear and never read by the firmware.

The PLL itself is unchanged from the plan and still wheel-agnostic: resample onto a uniform 10° grid,
decimate when the wheel is denser than the grid and free-run the DCO when it is sparser, `VT_SLEW_SHIFT
= 1` so one real tooth may move the pending virtual tooth by at most half a vtooth, re-anchor rather
than slew across a position discontinuity, and never re-time the DCO into the past.

## Config surface

`TriggerConfig` (`generated/modules/trigger_config.h`) is `trigger_offset_btdc`, `sensor_angle`,
`min/max_full_sync_rpm_x10`, and six `StreamsConfig` of 216 B (1308 B for the block). Per stream:
`enabled`, `repeats`, `capture_index`, `edge`, `primitive`, `slots`, `gap_ratio`, `cell_len`,
`window_pct`, `window_crank_pct`, `width_min/max/target`, `nominal_angle`, `phased`, `phase_authority`,
`phase_allowance`, `cell[96]`. The schema carries the help text; the manual's settings reference and the
studio's tooltips are generated from it.

- `capture_index` indexes the board's own TRIGGER_INPUT capability pool (VR1=0, VR2=1, DIG1=2 …) with a
  `None` (255) option ahead of it, so the picker can express the field's own unassigned state — without
  it a stream could be pointed at an input and never pointed away from one. It says *where the signal
  enters*; the stream's **index** says what role it plays.
- `edge` on a GAP/SEQUENCE stream is not a tolerance knob: those matchers count *every* edge they are
  handed, so `Both` doubles the events per tooth and the pattern must be authored in those terms. WIDTH
  always measures both edges (it needs the high time) and reads this only as which edge **leads** the
  pulse.

Two things the shape of the config enforces rather than advises:

- **Cells are private to a stream.** They used to be runs inside one shared 48-entry `cell_pool`, each
  stream carrying a `cell_off` saying where its run began — so two streams could claim the same cells and
  describe the same wheel and nothing said so; only a tuner doing arithmetic stood between a
  valid-looking config and that. A private array makes the overlap unrepresentable (the same argument the
  role-indexed array is built on), and `cell_off` had nothing left to say. 96 rather than 32 because a
  wheel whose position is encoded in tooth *widths* needs a cell per edge (LS1 24x = 48 spans) and
  nothing says the next such wheel is not bigger.
- **A GAP wheel's first gap index must be 0.** The gap is the only event a missing-tooth wheel can
  identify, so the tooth ending it is the reference; any other first index does not change where sync
  happens, it renames that tooth and drags the angle origin onto one that cannot be identified at all,
  hiding an `index × tooth_angle` offset underneath `trigger_offset_btdc` — two knobs for one quantity,
  one of them invisible. `codegen.py::validate_trigger_wheels` fails the build on it (a 2JZ 36-2 shipped
  as `gaps:[6]`, 80 crank degrees out). Later indices are free: on a 36-2-2-2 they are `[0, 1, 14]`, they
  *are* the pattern, and the decoder needs them to place itself once it knows which gap it matched.

## The wheel library stays client-side

As designed: the firmware sees only a generic stream config. The **shipped** library now lives in
`definition/ecu.schema.yaml` (`trigger_wheels:`, 43 wheels) so the wheels a studio offers belong to the
firmware it is talking to; codegen validates it, and it reaches the studio through
`shared/tuneit-meta.json`, parsed by `apps/studio-jf/src/model/TriggerWheel.cpp`. User wheels persist
alongside it as `trigger_wheels.json`, in the same shape, so an exported wheel pastes straight into a
schema.

The library is an *authoring* format, not a firmware format: it keeps a `rate: 0|1` crank/cam shorthand,
adds `role` when a wheel must name its slot outright (a pair of cams on separate banks has to), states
`repeats` only where the slot default is wrong (5 wheels), sets `edge` per stream where the counting edge
matters, and carries wheel-level `cam_edge`, `sync`, `tdc_offset` and `sensor_angle`. The diagrams a
tuner sees are generated from the same document (`manual/reference/trigger-wheels.md`), and the tool that
authors new wheels is documented in `docs/trigger-designer-design.md`.

## Reset semantics — read before touching

`reset_all()` is a **full teardown** (wipes stream config, `n_ = 0`) and is only valid as the first half
of a rebuild, which `build_generic()` performs immediately. At runtime sync loss uses `drop_sync()`:
matchers back to ACQUIRING with their wheel config intact, per-stream tooth timing cleared, level and
anchor cleared — while the stream config, the cycle, `sync_always` and **`has_crank_`** are all kept.
`has_crank_` is engine configuration, not decode state: clearing it left the decoder believing it had no
crank at all until the next rebuild, and the branch that reads it first is "cam-only sensor: cam is
everything", which grants **full PHASE sync off a single cam edge**, names the cam the velocity source,
and lets `get_rpm_x10()` keep reporting the free-running grid's speed — an engine at a frozen rpm with a
dead crank. Three other readers changed meaning the same way.

`on_trigger_lost()` keeps the stream config so the decoder re-acquires from the next real teeth with no
reconfigure, zeroes the grid velocity (a stale one reported 1348 rpm into a dead trigger for forty
minutes — nothing that outlives the thing it was measured from is safe to keep), and deliberately keeps
`last_fine_tick_`, because the follow-up that separates "late tooth" from "dead wire" measures the
silence from the last real tooth.

## Files

| Role | Where |
|---|---|
| Decoder core, fusion, level machine, health counters | `firmware/Scheduler/GenericTrigger.h` (1041 lines) |
| Primitives | `GapMatcher.h`, `SequenceMatcher.h`, `WidthMatcher.h` |
| Tune config → decoder | `GenericTriggerBuilder.h`, `generated/modules/trigger_config.h` |
| Can this config ever sync? | `TriggerConfigCheck.h` |
| Stream roles, ceilings, error kinds | `SchedulerTypes.h` |
| Capture ISR, PLL feed, tooth deadline, VVT publish | `EnginePositionHal.{h,cpp}` |
| Angle grid / PLL | `VirtualTrigger.{h,cpp}` |
| Raw edge capture for a wheel the decoder does not understand | `TriggerLogger.h` (records *before* the decoder sees the edge — a logger downstream of decode cannot show the noise that broke the decode) |
| Telemetry → bus | `EngineTask.cpp` |
| Protection / DTCs | `Engine/Modules/EngineProtection.cpp`, `docs/dtc-codes.md` |
| Wheel library | `definition/ecu.schema.yaml` (`trigger_wheels:`), `apps/studio-jf/src/model/TriggerWheel.cpp` |
| User-facing | `manual/part3/16-trigger.md`, `manual/reference/trigger-wheels.md`, `docs/trigger-designer-design.md` |

## Tests

`tests/test_generic_trigger.cpp` (1271 lines: the fusion and level machine driven by synthetic crank+cam
streams), `test_gap_matcher.cpp`, `test_sequence_matcher.cpp`, `test_width_matcher.cpp`,
`test_generic_builder.cpp`, `test_trigger_config_check.cpp`, and two that exist only because of the
behaviour described above: `test_trigger_failures.cpp` (noise, chipped teeth, cam decoys, phase loss) and
`test_trigger_liveness.cpp` (the deadline, absence, the CRANK⇄PHASE window). Studio side:
`apps/studio-jf/tests/test_trigger_core.cpp` asserts against the shipped library in the meta rather than a
list in the test file, and `test_trigger_library.cpp` covers the library format.

## Deviations from the original design

Where the shipped decoder does something the design below did not say. Each was forced by a measurement:

1. **Six streams, role-indexed by slot**, not "up to 4, appended". Appending made contiguity
   load-bearing and undocumented: `on_edge()` is handed the *config index*, so the moment a config had a
   hole the edges landed on the wrong decoder stream, or on none.
2. **`repeats` replaced `rate`, and the cycle became configurable** (2-/4-stroke, rotary), with `rev_` a
   counter rather than the 720° bit the model assumed.
3. **The pool is three primitives, not five** — EVEN folded into GAP, LEVEL never built.
4. **Every locked edge is validated against a schedule, and there is no tolerated-miss path.** The design
   described locking and sync loss; it did not describe *proving position per tooth*, which is what most
   of the shipped complexity is.
5. **Anchor vs counter is honoured by the fusion.** The design's fusion had "intra-period angle + is it
   absolute" and combined them; the shipped code refuses to derive an even wheel's angle from its own
   pattern origin and requires an anchor, applied on the next tooth with a measured carry.
6. **Cam phase is a two-sighting place-and-time window**, not "a locked CAM stream resolves the
   revolution". The design's rule granted phase on the first cam edge, which on a VR cam made a lock
   360° out a coin toss: sequential spark on the overlap TDC, no power, fuel lighting in the exhaust
   (measured on a warm restart — a full sync, a stumble, a backfire, a stall).
7. **The decoder owns a clock.** Absence detection moved out of the DCO, which is also what made
   SIGNAL_ABSENT sayable as distinct from P0335 at all.
8. **Faults carry the stream index and the error kind**, and the counters split into per-cycle and
   free-running families, because the per-cycle one stops meaning anything exactly when it matters.
9. **Cells are per stream** (96, not a shared 48-entry pool) and gap indexing is validated at codegen.
10. **`trigger_offset_btdc` lives in the config and is applied at one boundary**, so there is exactly one
    wheel-to-engine transformation in the system.

## Open questions / worth revisiting

- **The window is not one quantity in the two matchers.** GAP applies `window_pct` to a **tooth pitch**
  (as the schema help text and the deadline comments both describe it); `SequenceMatcher` applies it to
  the **predicted interval**. They agree only for a uniform wheel — with a 50% window on a 3× gap,
  SEQUENCE admits a far wider absolute band than the GAP path would. Worth one shared definition.
- **The overdue deadline uses a fixed half pitch** (`fine_tol_ticks() = pitch_ticks() / 2`) while the
  matcher uses `window_pct` of a pitch. At the shipped default of 25%, a tooth arriving at 1.3× is
  rejected the moment it lands, while one that never arrives is waited for until 1.5× — the exact
  disagreement the comment in `EnginePositionHal.cpp` says was removed. It errs in the safe direction (a
  floor can only make the report later, never earlier), so this is tidiness rather than a hazard.
- **`phase_allowance` defaults to 0**, so fixed cams are held to one crank pitch. The code comment is
  explicit that this number should be chosen from the recorded `phase_resid_[]` distribution across real
  engines and real thermal and load conditions rather than guessed there; that data has not been
  collected yet.
- **`MAX_MISSES` is unused by both matchers.** It is still declared (`GapMatcher.h`, `SequenceMatcher.h`)
  and still read by one host test as a loop bound (`test_sequence_matcher.cpp`), but nothing in the
  matcher logic consults it — a window violation drops the lock on the first occurrence. Either delete
  it and rename that test's loop, or bring back a ride-through policy: a constant describing behaviour
  the code refuses to have is how the old rule comes back.
- **`TriggerEngineShape` is assembled at one call site** (`EnginePositionHal`), so the studio cannot yet
  run `check_trigger_config` against a tune nobody has flashed, which is what that header's own preamble
  promises. The function is pure; the plumbing is not.

---

## Appendix — the original brief (2026-06-02, unedited)

**Goal:** replace the per-wheel `SyncStrategy` zoo with a generic *pool of decode primitives* applied to
one or more signal *streams*, fused into engine position. Wheel knowledge (presets) moves entirely
client-side (the studio's preset selector); the firmware only ever sees a generic stream config. "All
primitives, all wheels."

**Why:** the strategy enum conflated *what kind of crank signal* and *what kind of cam signal* into one
value, so every crank-type × cam-type combination needed its own code path (PATTERN_EXCEPT had no cam
promote; 6g72 = even crank + multi-cam fit nothing). The matchers built that session (ODD_FIRE,
PATTERN_EXCEPT, the cam-pulse matcher) were secretly **one algorithm** — match a cyclic event-interval
sequence — applied to three streams, and the PLL/VirtualTrigger was already wheel-agnostic. So the
generic model was mostly *deleting duplication*: simpler firmware, not bigger.

**Built as:** SEQUENCE first as an exhaustively-tested standalone unit (solved 6g72), then GAP / EVEN /
WIDTH, then the stream layer, then fusion and the level machine, then the generic schema and codegen,
then every bench-proven wheel re-expressed as a stream config as the regression harness. Kept unchanged:
`VirtualTrigger`, `EventScheduler`, the `EnginePositionHal` pool/firing, the capture HAL and the
schema-driven reconfigure plumbing.

**The risks it predicted, and what happened:** the fusion state machine was indeed the new complexity —
multi-stream concurrency and level transitions are where most of the later fixes landed, and the host
suites named above are what caught them. Hot-path determinism held (`switch` dispatch, 32-bit
arithmetic kept on the capture path so no `__aeabi_uldivmod` software divide appears there). The config
surface stayed client-side, though the configurator became the Trigger Designer
(`docs/trigger-designer-design.md`) rather than the "visual tool" this brief anticipated.
