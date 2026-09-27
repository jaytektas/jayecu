# Fueling — models + jayecu architecture (working design)

Status: **forming** (v0). Part A is reference; Part B is the proposed jayecu design with open
decisions called out. Nothing here is built yet.

---

## Part A — Fueling models (reference)

Every fuel calc resolves to one thing: **injector delivery (pulse width / fuel mass) per cylinder
per cycle to hit a target lambda.** Models differ on two independent axes.

### Axis 1 — air-charge estimation
| Model | Load source | For | Weakness |
|---|---|---|---|
| **Speed Density** | MAP + VE table (RPM×MAP) via ideal-gas law | boosted, most engines; easiest to tune | big-cam reversion at idle, needs charge-temp |
| **Alpha-N** | TPS × RPM table | ITBs, cammed NA, no stable vacuum | no boost/baro awareness alone |
| **MAF** | direct mass measurement | stock-ish, great part-throttle/transient, auto temp/alt | reversion, placement, flow ceiling |
| **Blended** | MAP+TPS or MAF+SD weighted by RPM/load | cammed + boost, robustness | more tables |

The VE/fuel-table **load axis** is itself a choice: MAP, charge-mass-corrected MAP, TPS, MAF mass.

### Axis 2 — fuel conversion (air → injector command)
1. **Raw PW / fuel table** (ms or %): a required-fuel constant × VE%. No physical
   meaning — injector/stoich/displacement all baked in; hardware change = re-tune.
2. **VE-physical speed density**: VE table = real volumetric efficiency %; fuel mass from gas law.
   Portable across injector/fuel changes. **← jayecu is here.**
3. **Fuel-mass / modeled**: compute required fuel *mass* from a charge model → an **injector model**
   converts mass→PW. Decouples injector characterization from the tune.
4. **MAF charge model**: air mass from MAF → fuel mass → PW.

Canonical SD chain:
```
squirt = injector_lag(V_batt)
       + warmup(CLT) · ( Vd · VE(RPM,MAP) · MAP / (R · charge_temp(CLT,IAT,TPS)) )
         / target_afr(RPM,MAP) / injector_flow
```

### Corrections / options (bolt onto any core model)
Startup: cranking fuel, prime pulse, after-start enrich (ASE), warmup enrich (WUE/CLT).
Air-state: IAT/charge-temp density, charge cooling (fuel vaporization), baro/altitude.
Transient: accel enrich (TPS-dot / MAP-dot), **wall-wetting / X-τ / fuel-film** (X = wall-puddle
fraction, τ = evaporation time const), decel fuel cut (DFCO).
Delivery: dead-time vs **battery voltage** (+ vs ΔP fuel pressure), **short-pulse adder** (injector
non-linear <~2 ms), fuel-pressure ΔP comp (flow ∝ √ΔP), flow units cc/min or g/s.
Fuel-specific: **flex** (ethanol% → stoich shift E0 14.7→E100 9.0, fuel multiplier, inj-flow scale,
cold-start/ign trims).
Closed-loop: lambda feedback (NB/WB), **STFT/LTFT**, VE self-learn; per-cylinder trims.
Scheduling: batch/bank/sequential, injection timing (EOIT), staged (port+secondary, port+DI).

---

## Part B — jayecu fueling architecture (proposal)

### B0 — design principles
1. **Tune vs Learned are different stores.** Calibration (VE, target, injector data) lives in the
   **tune (flash, burned deliberately, reproducible)**. Adaptive/learned data (LTFT, etc.) lives in
   **RAM + a non-glitching durable store** and NEVER rewrites the tune. This is the whole answer to
   "non-flash-glitching" *and* keeps tunes reproducible (the engine never silently edits the map).
2. **The fuel calc is a DAG of bus-connected modules** — one concern each, run in EngineTask's
   module phase in dependency order, composing *downward* via the SignalBus. Matches the
   polymorphic-pipeline direction.
3. **Corrections are first-class bus signals** (named factors), composed by an assembler — so every
   correction's contribution is visible in telemetry and individually tunable/diagnosable.
4. **Model-selectable** (SD / Alpha-N / MAF / blend) behind one `air_mass` signal — downstream fuel
   math is identical regardless of how air was estimated.
5. **Everything is live-tuneable** — aftermarket-ECU default. Calibration lives in `g_config` RAM,
   written live (no burn glitch); modules read it live + re-derive on `g_config_generation` (the
   existing model — fuel inherits it). **Structure vs calibration**: the firmware *structure* (which
   modules/corrections exist, the registry membership, cadences) is build-time schema/codegen; the
   *calibration* (every table, gain, authority, enable flag, AE domain, target) is live config.
   A fixed structure never freezes tuning — it defines the feature set the live config drives.
6. **Hardware-agnostic** — board specifics (NV storage, deferred-shutdown wiring, sensor presence)
   are HAL capabilities discovered at runtime, never assumptions baked into the fuel logic.

### B1 — the module chain (each reads/writes the SignalBus)
```
[sensors publish: map, iat, clt, tps, rpm, lambda_1, battery, fuel_pressure, ethanol, baro]
      │
      ▼
1. ChargeModel      → air_mass_mg, charge_temp_k, ve_pct      (SD/Alpha-N/MAF/blend, selectable)
2. FuelProperties   → stoich_afr, fuel_mult                   (from ethanol% — flex)
3. FuelTarget       → lambda_target                            (table RPM×load + overrides)
4. FuelBase         → base_fuel_mg = air_mass / (lambda_target · stoich)
5. Corrections*     → fuel_corr_<name> factors                 (warmup, charge-temp, baro, AE/wall-wet…)
6. LambdaControl    → fuel_trim_st  (+ owns LTFT learn)        (STFT PI; LTFT cell table)
7. FuelAssembler    → fuel_mg = base · Π(corr) · (1+stft) · (1+ltft)
8. InjectorModel    → inj_pw_us[cyl] = fuel_mg/flow + deadtime(V,ΔP) + spwa ; + inj timing
      │
      ▼
[Scheduler fires injectors at pw + EOIT angle]
```
`*` Corrections that need state (wall-wetting puddle) are their own stateful modules; cheap scalar
corrections can share a module. Each still **publishes a named factor** for transparency.

### B2 — correction composition: the factor registry (formed)

**Replaces three scattered mechanisms with one.** Today corrections live in three places:
`EngineFrame.prot_enrich_pct` (EngineProtection → read inline by FuelCalculator),
`EngineFrame.clt_fuel_corr` (inline `pw *= 1+clt/100`), and `ScriptEngine::apply_corrections`
(Lua-accumulated trims applied post-tick). The registry **unifies all three into bus signals** — not
a fourth mechanism. (Per the research-before-new-mechanism rule.)

**Two commutative groups, two domains:**
- **Mass multipliers** `fuel_corr_<name>` (≈1.0) — scale fuel *mass*, applied **before** flow
  conversion: warmup, IAT/charge-temp, baro, flex, protection, accel, **STFT/LTFT**, `fuel_corr_lua`.
- **PW adders** `pw_add_<name>` (µs, ≈0) — injector-domain, applied **after** flow conversion:
  dead-time(V,ΔP), short-pulse adder. (Order within each group is irrelevant — × and + commute.)

**Composition (in the per-cylinder FuelAssembler):**
```
mass_mult = 1
pw_add    = 0
for c in FUEL_CORRECTIONS:                       # codegen registry, not a hardcoded list
    v = bus.get(c.signal, c.op==MULT ? 1.0 : 0.0)   # NEUTRAL default → disabled/invalid = absent
    if c.op==MULT: mass_mult *= clamp(v, c.min, c.max)
    else:          pw_add    += v
mass_mult = clamp(mass_mult, TOTAL_MIN, TOTAL_MAX)            # total-authority safety clamp
fuel_mass = base_mass(bus) * mass_mult * per_cyl_mult[cyl]   # per-cyl from per-cylinder state
pw[cyl]   = fuel_mass / inj_flow + pw_add + per_cyl_pw_add[cyl]
```

**Why this shape:**
- **Neutral-by-default** — `bus.get(sig, neutral)` means a disabled/invalid/never-published
  correction contributes *nothing* automatically; the assembler has **zero "is it enabled?"
  branching**. (Reuses the bus's invalid→default behaviour we already rely on.)
- **Transparent** — every correction is a *logged bus signal*; the tuner sees the exact breakdown
  (warmup ×1.05, IAT ×0.98, STFT ×1.02…), not just a final number. This is the decisive win over a
  chained-rewrite of one `fuel_mass` signal.
- **Order-independent** — commutative groups → no ordering bugs as corrections are added.
- **Extensible** — add a correction = a module that publishes its signal + one `fuel_corrections:`
  schema row; codegen regenerates `FUEL_CORRECTIONS[]`; the assembler auto-includes it. **Zero
  assembler edits.** Mirrors SENSOR_PROVIDES / well-known-signals.
- **Lua-native** — a script publishes `fuel_corr_lua`; the Lua plane binds the same registry (the
  modular-platform "Lua as a plane over the same registry" intent), retiring `apply_corrections`.
- **Cadence-agnostic** — each correction publishes from whatever cadence fits (warmup 1 kHz,
  dead-time per-cylinder); the assembler reads the latest off the bus. The bus decouples cadences.
- **Safe** — per-correction clamps (registry rows) + a total-authority clamp (runaway guard).

**Schema (proposed `fuel_corrections:` list → codegen `FUEL_CORRECTIONS[]`):**
```yaml
fuel_corrections:
  - {signal: fuel_corr_warmup,     op: mult, min: 0.5, max: 4.0}
  - {signal: fuel_corr_iat,        op: mult}
  - {signal: fuel_corr_baro,       op: mult}
  - {signal: fuel_corr_flex,       op: mult}
  - {signal: fuel_corr_protection, op: mult}        # was EngineFrame.prot_enrich_pct
  - {signal: fuel_corr_accel,      op: mult}         # AE/wall-film as a mass mult (modeled)
  - {signal: fuel_corr_lua,        op: mult}         # retires ScriptEngine::apply_corrections
  - {signal: fuel_trim_st,         op: mult, min: 0.75, max: 1.25}
  - {signal: fuel_trim_lt,         op: mult, min: 0.70, max: 1.30}
  - {signal: pw_add_deadtime,      op: add}
  - {signal: pw_add_short_pulse,   op: add, per_cylinder: true}
```

**Per-cylinder corrections** (`per_cylinder: true`: per-cyl trim, short-pulse adder, wall-film) are
declared in the registry but read from the **per-cylinder task's per-cyl state**, not a global bus
signal (one-value-per-signal doesn't fit per-cyl). The registry entry tells the assembler "pull
this from the per-cyl slot," keeping the composition uniform.

**Structure vs calibration here:** the `fuel_corrections:` list is *structure* (codegen — which
corrections exist + how they compose, fixed at build). Every correction's *behaviour* — its enable
flag, curves, gains, authority, and the AE domain choice — is **live config**. Disabling a correction
live = its module publishes neutral (1.0/0) → it drops out of the product with zero special-casing.
So the fixed registry does not limit live tuning (resolves the (a) concern).

*Sub-decisions (resolved):* (a) **dedicated `fuel_corrections:` schema list** (structure); behaviour
stays live config. (b) **AE domain is a live config option** — the AE module publishes to
`fuel_corr_accel` (mass) or `pw_add_accel` (PW) per the tune, the other staying neutral. (c) per-cyl
corrections held in **per-cylinder task state**, declared in the registry with `per_cylinder: true`.

### B3 — closed-loop fuel trim: STFT + LTFT (formed)
**`LambdaControl` module, background cadence** (self-decimated ~20–50 Hz — wideband dynamics are
slow; pointless at engine speed). It is NOT in the hot per-cycle path: it reads `lambda_N` +
`lambda_target` + conditions off the bus and **publishes two factors** (`fuel_trim_st`,
`fuel_trim_lt`) that the per-cylinder assembler multiplies in (B2). **Per bank** (1 or 2 widebands).

The two trims are a **fast/slow pair** with separated timescales:
- **STFT** — fast PI loop, lives in RAM, centred ~0, handles instantaneous error. Volatile.
- **LTFT** — slow learned RPM×load **cell table**, the steady-state map, persisted in the HAL
  `LearnedStore` (B5). Applied always (open- and closed-loop) so it helps before O2 lights and in
  CL-disabled regions.

#### Reusable PI primitive (first controller — boost/idle/e-throttle reuse it)
A small generic `PiController{Kp, Ki, out_min, out_max}` with **clamp + anti-windup** (stop
integrating, or back-calculate, at the clamp). Gains/limits are live config. STFT is its first user.

#### STFT
- Error: `e = lambda_meas − lambda_target` (the physically-correct instantaneous correction is
  `fuel × lambda_meas/lambda_target` — lean → more fuel; the PI converges to it).
- `stft = clamp(Kp·e + Ki·∫e·dt, ±AUTH_ST)`; publishes `fuel_trim_st = 1 + stft` (registry clamps it
  again, 0.75–1.25). Sign: `lambda_meas > target` (lean) → add fuel.
- **Freeze the integrator** (don't update) when ANY: O2 not lit/invalid, DFCO, **AE/transient
  active** (MAP-dot/TPS-dot over threshold), cell just changed, outside the CL region. Freezing on
  transients is essential — the transport-delayed reading would otherwise teach the integrator the
  wrong thing while AE handles the event open-loop.

#### LTFT (learning)
- Cell table (RPM × load), each cell ±`AUTH_LT` (int8/int16 %), in `LearnedStore`.
- **Bleed-learning** (separated timescales): under stable closed-loop conditions, slowly migrate
  STFT's steady bias in the current cell **into** LTFT and recentre STFT toward 0 — `LTFT[cell] +=
  LEARN·stft ; stft_integ −= LEARN·stft`. Total correction is preserved; the persistent part moves
  to LTFT, freeing STFT authority for transients. `LEARN` ≪ STFT bandwidth (LTFT is a per-cell
  low-pass of STFT).
- **Cell-dwell gating**: only learn after occupying a cell *stably* for `T_dwell` (don't learn from a
  50 ms transit) and only when **STFT is not railed** (a saturated STFT isn't the true bias).
- **Apply**: `fuel_trim_lt = interp2d(LTFT, rpm, load)`, smooth. Learn into the dwelt cell (or the
  surrounding 4 by interpolation weight); apply interpolated. Per-cell clamp; railed cell → DTC.

#### Closed-loop enable matrix (STFT active ⊃ LTFT learn)
STFT active requires ALL: O2 valid + lit (heater ready, plausible reading); CLT > CL-temp; time
since start > delay; inside the **CL RPM/load region** (config map — excludes deep power-enrich at
WOT unless wideband-CL-at-target is configured, and DFCO). LTFT learn adds: STFT active + cell-dwell
met + STFT not railed + extra-stable conditions. All thresholds are **live config**.

#### Lambda transport delay (the control subtlety — decision 4)
The reading reflects fuel from ~N cycles + exhaust transport + sensor τ ago, so naive high gain
oscillates. **v1: conservative loop bandwidth + the transient-freeze above** (AE owns the fast
events open-loop, so the slow loop only ever sees quasi-steady state — delay becomes a non-issue).
*Refinement:* model the delay (cycles = f(RPM), transport = f(flow), sensor τ) and compare
`lambda_meas[now]` against `target[now − delay]` from a small ring buffer, enabling a faster loop.
Ship conservative; add the model if bandwidth proves limiting.

#### Diagnostics
Both STFT **and** LTFT railed lean → **P0171** (bank 1) / **P0174** (bank 2); both railed rich →
**P0172 / P0175** — all inside the existing `o2_mixture` DTC category, via DtcManager. Implausible /
stuck lambda → drop closed-loop + DTC. A single railed LTFT cell → low-severity warning.

#### Per-bank / per-cylinder
v1: one STFT + LTFT **per bank** (`lambda_1`/`lambda_2`). Per-cylinder trim (individual-cylinder
lambda) is a later extension — a per-cyl offset vector applied in the per-cylinder task; the cell
LTFT stays the bank baseline.

#### Fuel-change & re-tune interactions (decisions 2 & 3 — configurable)
- **Flex re-learn (2):** default **single LTFT, re-converges** after a fuel change — LTFT corrects
  *model* (VE) error, which is largely fuel-independent; FuelProperties handles the stoich/flow
  shift. *Config option:* per-fuel LTFT sets (E0/E100) interpolated by ethanol%, if flex compensation
  proves imperfect.
- **Re-tune (3):** default **keep** LTFT (it re-converges over the new map) + an explicit
  **"reset learned"** command. *Config option:* auto-decay LTFT toward neutral when a fuel table edit
  bumps `g_config_generation` (so a re-tune washes out stale learning instead of fighting the tuner).

### B5 — persistence: HAL-agnostic learned-data store
**Two separate write models, both already correct in spirit:**
- **Tune (calibration)** is *live-tuneable* the existing way: `g_config` lives in **RAM**; studio/bench
  `'w'` writes hit RAM instantly + bump `g_config_generation`; subsystems re-derive. Flash "burn"
  (`ConfigBank`) is a *separate, deliberate, at-rest* persist (a live flash write stalls the bank →
  ISR glitch). **The fuel subsystem inherits this** — it reads live config like Sensors; no special
  live-tune machinery. Everything the tuner touches is live by construction.
- **Learned data (LTFT etc.)** must persist *without* a glitch and *without* assuming a clean
  shutdown — so it does NOT live in the tune flash, and it does NOT rely on key-off.

**The firmware is HAL-agnostic; persistence is a HAL capability, not a board assumption.** Define a
`LearnedStore` HAL service: the fuel subsystem asks for a RAM-backed region (`region(id, size)`) and
gets back a **durability capability** (volatile / battery-backed / committed). The HAL owns the
medium + commit policy; the fuel side is oblivious to which backing it got (it only reads the
capability flag, for diagnostics).

| Backing (HAL picks best available) | Survives | Commit needed | On this board |
|---|---|---|---|
| **Battery-backed RAM** (e.g. 4 KB BKPSRAM, backup domain) | abrupt power loss (battery holds it) | **none** — it's just RAM that doesn't lose state | **yes — primary** |
| **SD card** (StorageManager/sd_arbitrator) | anything | periodic commit while running + optional key-off commit | **yes — cold backup** |
| **none** (no NV on this board) | nothing | — | LTFT is RAM-only → **re-learns each boot** (degrades, never breaks) |

**Policy that does NOT assume a clean key-off** (key-off deferred-shutdown is *optional* — the board
may not be wired/configured for it):
- If battery-backed RAM is present, LTFT lives there → survives abrupt power-cut with **zero
  commit**. The win case on the current board.
- SD is the cold/cross-board backup: **periodic commit while running** (bounds data loss on an abrupt
  cut to the commit interval) + an *opportunistic* key-off commit **if** the board reports
  deferred-shutdown capability. Never block the realtime path; runs on the low-prio storage task.
- Boot load order: battery-RAM (CRC-valid) → SD → neutral 1.0. **No NV at all → RAM-only, re-learn.**
- **Tune flash is never written with learned data.** Baking LTFT into the VE table is a separate,
  explicit, engine-off operation if the tuner ever wants it.

This keeps the design portable: the current board uses BKPSRAM + SD; a board with FRAM or no NV gets
the same code path with a different `LearnedStore` capability. *Open: journaled vs whole-table SD
commit; commit interval.*

### B6 — the many other aspects
- **CL enable matrix**: warm (CLT > x), O2 lit + valid + heater ready, time-since-start, in CL
  RPM/load region, not DFCO, not WOT (configurable), not transient.
- **Authority + anti-windup**: clamp STFT/LTFT; stop integrating at the clamp.
- **Transient boundary**: AE/wall-wetting fuel must NOT be learned — freeze trims whenever AE armed.
- **Cell occupancy/interpolation**: learn the dwelt cell; apply interpolated; blend cell edges.
- **Per-cylinder trims** (future): individual-cylinder lambda → per-cyl trim vector.
- **Flex re-learn**: a fuel-composition change shifts stoich/flow; either re-converge trims or hold
  a per-fuel LTFT set. *Open decision.*
- **Diagnostics**: STFT+LTFT railed lean → **P0171** / rich → **P0172** (per bank P0170/P0173);
  implausible lambda; O2 heater not ready. Routed through the DtcManager you already have.
- **Open-loop fallback**: when CL unavailable, base · corr · LTFT only (STFT = 0).
- **Tune-change invalidation**: re-burning the VE/target table should optionally reset or scale
  LTFT (a stale learned offset over a new map fights the tuner). *Open decision.*

### B7 — execution model / timing domains  ← grounded in the ACTUAL structure
**What already exists** (investigated, not presumed — `EngineTask.cpp`, `EnginePositionHal.h`,
`EventScheduler`):
- **Firing is already ISR + per-cycle self-correcting.** The TIM5 grid/match ISR (NVIC prio 2)
  owns all schedule mutation + dispatch. A persistent event skeleton is built ONCE; each cycle the
  STATIC nodes come due in the ISR and call a **compute hook** (`recompute_ignition` /
  `recompute_dwell` / `recompute_injection`) that reads the **`shadow_[]` registers + live RPM** and
  re-arms the VOLATILE fire nodes. No per-cycle rebuild. This is tier-1, and it's already right.
- **The task→ISR handoff is already lock-free.** FreeRTOS tasks call `set_spark_btdc` /
  `set_dwell_us` / `set_inj_pw_us` → write `shadow_[]` (volatile scalars, a single aligned STR on
  M7 = atomic). The ISR reads them live; "changes take effect on the next cycle (gap tooth
  boundary)." **So I do NOT need a new double-buffer — `shadow_` + per-cycle ISR pickup IS the
  decoupler.** (Correcting my earlier proposal.)
- **`EngineTask` is one RTOS task @ prio 3**, looping `vTaskDelay(1 ms)` → `run_frame()` = INPUT
  (sensors) + MODULE (fuel/ign/protection) + OUTPUT (telemetry), then `commit_to_hal(frame)` writes
  `shadow_`. The TIM5 ISR (prio 2) preempts it, so firing deadlines are **already** protected.

**The deadline separation is therefore largely already built** (fire ISR + `shadow_` + per-cycle
self-correct). The remaining problem is narrow: the **value compute** that fills `shadow_` runs at a
fixed **1 kHz**, but `shadow_` is only *consumed* per cycle/per cylinder. At idle (~7 Hz) that's
~100× wasted recompute of an idempotent answer, aliased against the physical event.

#### The multi-cadence execution model (decided — revised after the E-2a bench)
Rather than move fuel into one new task, **keep the 1 kHz task and add position-driven cadences**;
each module registers a **cadence** and self-decimates within it. The 1 kHz task is NOT demolished.

**The hard constraint that shapes everything (verified on HW, E-2a):** the engine ISRs — capture
(EXTI prio 1) and grid/fire (TIM5 prio 2) — run *above* FreeRTOS's syscall ceiling
(`configMAX_SYSCALL_INTERRUPT_PRIORITY` = 5) **by design**, so firing can preempt the kernel for
timing. An ISR above the ceiling **may not call any `…FromISR` API** — doing so corrupts the
scheduler (a hard fault the instant the engine syncs). So **there is no `vTaskNotifyFromISR` from the
grid ISR**, and the earlier table's "notify from the per-cycle ISR boundary" was not implementable.

This forced the right question: *does the per-cycle work need to PREEMPT, or just be runnable soon?*
It has **no hard deadline** — its only output is `shadow_[cyl]`, and the fire ISR re-arms from the
*last* `shadow_` if the compute hasn't refreshed (graceful degrade). So "runnable soon, off the
1 kHz critical path" is all that's needed → **the 1 kHz task notifies it** (`xTaskNotifyGive`, plain
task context, syscall-safe, zero platform code, portable to any MCU/RTOS). And the per-cylinder
*apply* never wanted to be a task at all — it is already in the fire ISR.

| Domain | Trigger | Prio | Owns |
|---|---|---|---|
| **1 kHz task** (EngineTask) | `vTaskDelay(1 ms)` | RTOS 3 | sensors, LambdaControl (STFT/LTFT), warmup, protection, diagnostics, telemetry, **fast-transient AE detection**; **detects the cycle boundary → notifies the per-cycle task** |
| **Per-cycle task** | `xTaskNotifyGive` from the 1 kHz boundary detection + 50 ms timeout | RTOS **2 (BELOW 1 kHz)** | the **shared fuel base** (ChargeModel → air mass, target, FuelBase, global corrections) **+ per-cylinder trim/wall-film compute** → `shadow_[cyl]` + bus |
| **Fire + capture + per-cyl apply/acquire (ISR)** | TIM5 grid/match, EXTI capture | NVIC 1–2 (**above** RTOS ceiling) | decode; **injector/coil FIRE** re-arming from `shadow_`+RPM (the per-cyl *apply*, already here); engine-sync **angle-window ACQUIRE** (snapshot the DMA-filtered ADC at the window boundary) |
| **Comms / CAN / Save / LED** | own | RTOS 1–2 | I/O, persistence — **below** the compute tasks |

**Why these splits:**
- **Per-cycle is BELOW 1 kHz, not above.** Firing is protected by the ISR, not by task priority, so
  the fuel base has no reason to outrank sensor freshness / lambda / protection. At prio 2 it runs in
  the inter-tick slack; if starved it just re-uses last `shadow_` (degrade). (Bench E-2a: at CRANK
  sync the per-cycle wake rate tracked rpm/60 to within 2% across a sweep, all above the 20 Hz
  timeout floor, no fault.)
- **No per-cylinder task.** The doc's old "per-cylinder task" conflated three things living in three
  domains: the **apply** (read `shadow_[cyl]`, arm the fire) is *already* the fire-ISR compute hook;
  the angle-window **acquire** is ISR bookkeeping (a cylinder *event*); the per-cyl **compute**
  (trims, wall-film) folds into the per-cycle task writing all cylinders' values once per cycle. None
  is an RTOS task — trying to make one is exactly what forced the illegal cross-ceiling notify.
- **Per-cylinder owns angle-window in the ISR** because the averaging-window boundaries *are*
  cylinder events ("2 Cylinders/Rotors"). The ISR snapshots/averages the DMA-filtered ADC at each
  boundary and publishes it; the per-cycle base then reads a **fresh, angle-correct MAP** instead of
  a stale 1 kHz sample. This gives the deferred `engine_sync_voltage` intent (docs/sensors-design.md)
  an execution home — in the acquisition ISR, not a task.

**Registration**: generalize `add_participant(m, phase)` → `add_participant(m, phase, cadence)` with
`cadence ∈ {KHZ_1, PER_CYCLE}`. Each cadence task runs its participant list in phase order;
self-decimation stays available within a cadence. (No `PER_CYLINDER` cadence — that work is ISR.)

**Escape hatch (not built — YAGNI):** *if* some future feature genuinely needs a sub-tick, ISR-driven
task wake (true preemption, not next-tick), the portable answer is a HAL primitive
`platform_softirq_bind/raise` — a deferred soft-interrupt the board implements however its silicon
allows (on Cortex-M, a spare NVIC vector at prio ≥ ceiling, raised via `NVIC_SetPendingIRQ`; the
engine layer never names a vector). It returns -1 where unavailable → degrade to the 1 kHz-notify
path. Nothing needs it today.

**Handoff & overload (reused as-is):** cross-cadence data flows through the SignalBus (lock-free,
TTL/freshness) and the per-cylinder `shadow_[]` (atomic volatile scalars). If a compute task can't
refresh in time, the fire ISR re-arms from the **last `shadow_`** — degrade, never miss a fire; add
an overrun counter for visibility.

`shadow_` is **already per-cylinder + rich** (`CylinderShadowParams[MAX_CYLINDERS]`: primary +
trailing spark/dwell, primary + secondary injection PW, staged flag). The change is the
per-cylinder compute populating each cylinder *distinctly* (individual trims / knock retard /
wall-film / staggered sequential timing) and adding any new per-cyl fields the model needs.

### B8 — phased build order (implementable sequence)
Dependency-ordered + value-first. Each phase keeps the engine running, is independently
host-tested + bench-validated + committed. The base air/fuel **physics** (IAT/charge-temp, baro,
flex stoich) lives in ChargeModel/FuelBase; the **registry** carries enrichments/trims only.

**Stage A — open-loop accuracy, in the existing FuelCalculator (no refactor, immediate tune value)**
1. **FuelTarget — lambda target table** (RPM×load), kill the hardcoded `1.0`. Trivial, unblocks
   everything downstream (closed-loop needs a real target). *Bench: set table → read `lambda_target`.*
2. **ChargeModel charge-temp/IAT density** — replace the hardcoded `1.2 mg/cc` with the gas-law term
   over `iat` (+ CLT blend). The single biggest accuracy gap. *Bench: vary IAT → fuel scales.*

**Stage B — the framework (the one structural refactor, after a clean first win)**
3. **Correction registry + FuelAssembler + module split.** Add the `fuel_corrections:` schema list →
   codegen `FUEL_CORRECTIONS[]`; split FuelCalculator into ChargeModel / FuelBase / FuelAssembler /
   InjectorModel; **migrate the existing corrections** (`prot_enrich_pct`, inline `clt_fuel_corr`,
   `ScriptEngine::apply_corrections`) onto the registry as bus signals. Proven by those 2–3 existing
   corrections. *Host tests: composition math + neutral-default + clamps. Bench: each `fuel_corr_*`
   visible in telemetry, fuelling unchanged.*

**Stage C — enrichments pile onto the registry (each = module + schema row, zero assembler edits)**
4. **InjectorModel** — dead-time vs battery voltage, short-pulse adder, fuel-pressure ΔP comp →
   `pw_add_*`. Uses existing `battery`/`fuel_pressure`. *Bench: vary V_batt → PW dead-time shifts.*
5. **FuelProperties + flex** — `ethanol` → stoich shift (FuelBase) + injector flow scale
   (InjectorModel) + cold-start; the payoff for the flex sensor already built. *Bench: inject ethanol%
   → AFR holds.*
6. **Acceleration enrichment → wall-film** — TPS/MAP-dot detection (1 kHz cadence) → `fuel_corr_accel`
   (domain = live config), then upgrade to the X-τ/fuel-film model. *Bench: throttle stab → transient
   AFR.*

**Stage D — closed loop**
7. **`PiController` primitive + STFT** — the reusable PI block (boost/idle reuse it) + `LambdaControl`
   (background cadence) → `fuel_trim_st`; CL enable matrix + transient freeze. *Host: PI step/clamp/
   anti-windup. Bench: wideband converges to target.*
8. **LearnedStore (HAL) + LTFT** — the NV-store capability (BKPSRAM primary, SD backup, RAM-only
   fallback) + the cell table + bleed-learning + dwell-gating → `fuel_trim_lt`; P0171/72/74/75.
   *Host: learn/clamp/persist round-trip. Bench: LTFT learns, survives key-cycle (BKPSRAM).*

**Stage E — execution refactor + per-cylinder (deferred to here: it's efficiency + the enabler for
per-cyl, not tune value; the interim 1 kHz fuel compute works fine until now)**
9. **Multi-cadence execution** — add the per-cycle **task** (`add_participant(m, phase, cadence)`),
   woken by the 1 kHz task's cycle-boundary detection (`xTaskNotifyGive`, syscall-safe — NOT from the
   above-ceiling grid ISR), prio *below* 1 kHz; move the value compute off 1 kHz; overrun counter.
   **E-2a DONE + bench-validated** (cadence tracks rpm, no fault). Then **E-2c**: engine-sync
   **angle-window MAP** acquisition in the **acquire ISR** (a cylinder event), not a task. *Bench:
   CPU drop at idle; MAP angle-sampled; no missed fires under load.*
10. **Per-cylinder control** — per-cyl trims + per-port wall-film computed in the per-cycle task →
    `shadow_[cyl]`; the per-cyl **apply** is the existing fire-ISR compute hook (no per-cyl task).
    Individual-cylinder lambda (needs phase 9). *Bench: per-cyl PW/timing distinct.*

**Stage F — model options**
11. **ChargeModel air-model selection** — Alpha-N, MAP+TPS blend, MAF, then fuel-mass/modeled mode;
    all behind the one `air_mass` signal so Stages B–E are untouched.

**Critical-path note:** Stages A–D deliver a fully closed-loop, self-learning, flex-aware fuel
system **without** the execution refactor (it stays in the 1 kHz task, self-decimated). Stage E is
the deliberate efficiency/per-cylinder upgrade once the fuel math is proven — lowest-risk ordering.

### Decided (this round)
- **Execution model = multi-cadence** (B7, revised after E-2a): keep 1 kHz + self-decimation; add a
  **per-cycle task** (shared base + per-cyl compute) prio *below* 1 kHz, woken by the 1 kHz task's
  cycle-boundary detection (`xTaskNotifyGive`, syscall-safe). comms/CAN/persist below; fire ISR
  untouched. `shadow_` reused (already per-cylinder); SignalBus is the cross-cadence decoupler.
- **Per-cycle is ONE task; there is no per-cylinder task.** Per-cyl *apply* is the existing fire-ISR
  compute hook; per-cyl *compute* folds into the per-cycle task; angle-window *acquire* is ISR work.
- **The engine-sync angle-window is acquired in the ISR** (a cylinder event), not a task.
- **Overload** = ISR re-arms from last `shadow_` + an overrun counter (no new mechanism).
- **Correction composition = factor registry** (B2): two commutative groups (mass-mult `fuel_corr_*`
  + pw-add `pw_add_*`), neutral-by-default via `bus.get`, codegen `FUEL_CORRECTIONS[]`, per-correction
  + total-authority clamps. **Unifies + retires** `prot_enrich_pct`, inline `clt_fuel_corr`, and
  `ScriptEngine::apply_corrections`. Lua publishes `fuel_corr_lua`. Sub-decisions resolved:
  dedicated schema list; AE domain a live config option; per-cyl corrections in per-cyl task state.
- **Live-tuneable + hardware-agnostic** are first principles (B0.5/B0.6): calibration is live
  `g_config`; structure (registry/cadence) is build-time; board specifics are HAL capabilities.
- **Persistence = HAL `LearnedStore`** (B5): the fuel side requests a region + reads a durability
  capability; HAL owns medium (battery-RAM → SD → none) + commit policy. Does **not** assume a clean
  key-off; degrades to RAM-only re-learn if no NV. Current board: BKPSRAM primary + SD backup.
- **Closed-loop trim = STFT + LTFT** (B3): reusable PI primitive; STFT fast/RAM/±0, LTFT slow learned
  cell table in `LearnedStore`, bleed-learning with dwell-gating, applied always. Per bank. CL enable
  matrix + transient freeze + conservative bandwidth for transport delay (model later). DTCs
  P0171/72/74/75 via `o2_mixture`. Flex: single LTFT re-converge (per-fuel optional). Re-tune: keep +
  reset command (auto-decay optional). All thresholds live config.

### Open decisions to settle
1. Module granularity: coarse (4–5 modules) vs fine (module-per-correction).
2. AE/transient path: fast 1 kHz detection publishing a bus factor (lean) vs TPS/MAP-dot event
   trigger — confirm it lives in the 1 kHz cadence, not per-cycle.

*(Most of the architecture is now formed: B0 principles, B1 module chain, B2 correction registry,
B3 closed-loop trim, B5 persistence, B7 multi-cadence execution. Remaining: ChargeModel air-model
selection detail, InjectorModel detail, AE/wall-film model, then the B8 phased build.)*
