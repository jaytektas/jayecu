# Module cadence + signal TTL

Working design doc. Goal: run each engine module at the frequency its **function** needs — no faster —
to reclaim CPU on the 1 kHz frame (which is allowed to degrade under load, so every cycle matters). Then
give each published signal a freshness TTL matched to its producer's rate, so a stopped producer's outputs
expire and consumers fail-safe.

## The model (settled)

- **The bus decouples rates.** Every signal carries its own freshness. A consumer reads each input at *that
  input's* rate — fast ones fresh, slow ones held-valid between updates. So a module's cadence is **not**
  derived from its inputs (a module may consume a 1 kHz input *and* a 20 Hz CAN input); it's set by what its
  **own output** must do.
- **Cadence ceiling = `min(actuator bandwidth, meaningful source rate, responsiveness need)`.** The actuator
  usually wins: a duty into a 250 Hz PWM solenoid can't be applied faster than 250 Hz, so updating it at
  1 kHz is wasted work. Cost/profiling does **not** decide cadence — function does.
- **TTL is a property of the _producer_** = `ttl_for(hz)` = **period + grace** (additive, `EngineModule.h`),
  where grace = `max(FRAME_SLIP_MS, period/2)`. NOT `period × N` — that made a 10 Hz signal linger 100× a
  1 kHz one. Now 1 kHz→5 ms, 30 Hz→49 ms, 10 Hz→150 ms: each ≈ its own period + slack. It only exists so a
  signal expires if the producer *stops*. Independent of consumers and inputs.

## As-built (mechanism)

- **Cadence is a firmware constant, NOT a tune field.** Each module declares `cadence_hz` in the schema
  (module-level attribute, sibling of `config:`). Codegen emits `generated/module_cadence.h`
  (`cadence::<Module>`); it never enters `EcuConfig` / the tune, so a tuner can't touch it — only an
  edit-schema → codegen → reflash changes it.
- **The scheduler owns decimation** (not each module). `add_participant(m, name, phase, cadence, cadence_hz)`
  computes `period = round(1000/hz)` frames, a staggered phase (same-rate modules spread across frames), and
  stamps the derived `ttl_for(hz)` onto the module. `run_phase` runs a module only on its due frames.
- **Modules stay pure** — they read nothing about cadence; publishes just pass `ttl()` (the injected ttl).
- **`ignition_trim`** is `cadence_hz: 1000` (full-rate); its internal one-table-per-frame round-robin is a
  private load-spreading detail, orthogonal to cadence. No special case.

Per-module rates now live in the schema (source of truth); the table below is the *rationale* record.

## Cut chain: bus + ttl (was EngineFrame-coupled)

The `fuel_cut`/`ign_cut` OR and EngineProtection's `prot_*` hand-offs USED to live on the per-frame
`EngineFrame`, which forced every cut/protection module to run the same 1 kHz frame in lockstep (they could
not decimate). They now travel on the SignalBus:

- **Cuts are validity-OR.** Each requester publishes `wk::fuel_cut`/`wk::ign_cut = true` **only while it wants
  a cut**, each at its own cadence with a ttl. Nobody ever publishes `false`. The OR is the signal's
  **validity**: `bus.valid(wk::fuel_cut)` == "someone is cutting"; it expires when the last requester stops.
  Consumers (FuelCalculator, Ignition, Lambda, EngineProtection status) check `bus.valid`, not the value
  (`get()` ignores validity). A stopped requester's cut simply expires — releases up to one ttl late (safe:
  over-cut, not under-cut).
- **Prot hand-offs are bus signals** (`wk::prot_rev_limit`, `wk::prot_rev_cut_fuel`, `wk::prot_boost_corr`),
  published while a protection level is active; RevLimiter/Boost read them with a safe default when absent.

So all nine (EngineProtection, RevLimiter, Launch, FlatShift, PitLimiter, LambdaProtect, EgtProtect, Dfco,
Boost) now decimate freely. **Testing note:** unit tests must model per-frame expiry — invalidate the cut
signals before each `update()` on a REUSED bus (real `expire_stale` runs every frame; the tests don't).

## Reference rates (grounded)

- Actuator PWM (OutputManager latches duty once per carrier): H-bridge **20 kHz** (ETB/stepper motor),
  VVT solenoid **500 Hz**, generic PWM output **250 Hz** (boost/idle/alt; MAC boost valves often ~15–30 Hz).
- CAN O2 (e.g. a CAN wideband controller): **20 or 50 Hz**. `lambda.update_hz` default **30**.
- Spark/injection are **per-cycle** (crank-scheduled), already their own cadence.

## Per-module proposal

Grouped by what determines the ceiling. "Consumed per-cycle" = read by the per-cycle Ignition/FuelCalculator,
so producing faster than the engine turns buys nothing.

### A. Fast servo / fast transient — stay 1 kHz (ttl 5 ms)
| Module | Function | Why fast |
|---|---|---|
| electronic_throttle | throttle-plate position PID → duty (H-bridge 20 kHz) | fast actuator, safety-critical **[done]** |
| transient_throttle | tip-in accel enrichment | must catch throttle *movement*; header says "fast, un-windowed" |
| app (pedal) | pedal → throttle_demand (feeds ETB) | keep ETB demand responsive — **200 Hz–1 kHz? your call** |

### B. Per-cycle — unchanged (ttl TTL_CYCLE_MS)
| Module | Function |
|---|---|
| ignition | base spark timing + sums trims/retards, per-cycle |
| fuel_calc | injection PW/angle, per-cycle |

### C. PWM-solenoid duty — cadence ≈ a few × the solenoid PWM
| Module | Output | Actuator | Proposed | ttl |
|---|---|---|---|---|
| boost | wastegate_duty | MAC valve (~15–30 Hz) / generic 250 Hz | **~50 Hz** | ~100 ms |
| vvt | vvt_duty_1..4 | VVT solenoid 500 Hz | **~200 Hz** | ~25 ms |
| idle | idle_duty (+trims) | idle PWM valve / stepper | **~100 Hz** | ~50 ms |
| alternator | alternator_duty | field, slow | **~50 Hz** | ~100 ms |
| wmi | wmi_duty | meth pump, slow | **~50 Hz** | ~100 ms |
| stepper | step_demand/pulse | already self-limited by `step_period_ms` | gate to step rate | ~step×5 |

### D. Cut issuers — consumed per-cycle by fuel/ignition
| Module | Output | Proposed | Note |
|---|---|---|---|
| rev_limiter | fuel_cut/ign_cut, soft_cut_pct | **per-cycle? or ~200 Hz** | rpm-driven cut — judgment |
| launch | fuel_cut/ign_cut/retard | ~100–200 Hz | |
| flat_shift | ign_cut | ~200 Hz | shift-cut wants prompt |
| pit_limiter | fuel_cut/ign_cut | ~100 Hz | |
| dfco | fuel_cut | ~50 Hz | decel fuel cut, slow condition |
| lambda_protect | fuel_cut | ~50 Hz | acts on slow lambda |

### E. Retard / trim producers — consumed per-cycle by Ignition
| Module | Output | Proposed | Note |
|---|---|---|---|
| knock | knock_retard | **~100 Hz** | capture is event-driven (Stage C); module only decays/applies |
| ignition_trim | ign_advance_trim | already 1 table/frame round-robin | slow env corrections; effectively decimated — maybe leave |
| nitrous | nitrous_retard/active | ~50–100 Hz | |
| anti_lag | antilag_retard/active | ~100 Hz | |
| egt_protect | fuel_corr_egt + fuel_cut | **~10–20 Hz** | thermal, very slow |

### F. Slow env / estimate / logic
| Module | Function | Proposed | ttl |
|---|---|---|---|
| lambda | STFT/LTFT fuel trims from O2 | **~50 Hz** (honor update_hz=30) | ~100 ms |
| cruise | road-speed hold → throttle floor | ~20–50 Hz | ~100 ms |
| gear_detect | gear from rpm/speed ratio | ~20–50 Hz | ~100 ms |
| traction | wheel-slip → torque cap | **~100–200 Hz? your call** | slip reaction may want faster |
| torque_model | torque/power estimate | ~50 Hz | ~100 ms |
| vvl | high-lift cam solenoid on/off | ~20–50 Hz | ~100 ms |
| fuel_trim | ? per-cyl fuel trims | **? your call** | may be per-cycle |
| engine_protection | overtemp/overboost monitors + retard/cut | **~100 Hz?** | overboost wants quick, overtemp slow |

## Open questions for you
1. **rev_limiter / cut-issuers**: per-cycle (naturally aligned with fuel/ign apply) or a fixed ~200 Hz?
2. **app (pedal)** and **traction**: how fast must these react? (both feed fast paths)
3. **fuel_trim**: what is it — per-cylinder fuel trims? cadence?
4. **engine_protection**: split fast (overboost) vs slow (overtemp), or one medium rate?
5. Config-driven rate (add an `update_hz` per module, tuner-set) vs a fixed compiled rate per module?
