# Knock control — design

Status: **built and bench-validated, on master.** This document describes the system AS BUILT; where
it diverges from the original design intent that is called out explicitly rather than quietly
dropped, because those are the decisions worth revisiting.

Software knock detection built on jayecu's existing seams. The hardware is ADC3,
KNOCK1=PF4/IN14, KNOCK2=PF5/IN15.

## The pipeline

```
 crank-angle window (ISR)  →  mailbox  →  knock worker task  →  Knock  →  Ignition
 arm_knock_window(cyl)        (SPSC)      ADC3 DMA burst        threshold   apply_knock_retard()
                                          Biquad + RMS dB       accumulate
                                                                decay
```

Four hops, each on its own side of a seam, because the DSP must not run in an ISR and must not run
on the 1 kHz engine task.

### 1. Window scheduling — `firmware/Scheduler/EnginePositionHal.cpp`

When `knock_window_en_` is set, the per-cylinder arm (`:503-506`) schedules a window at
`angle_atdc(scheduler_.tdc_angle(cyl), knock_window_start_, cyc)` — a POSITIVE offset is ATDC, i.e.
after the spark, which is where knock lives. `main.cpp:428` refreshes the enable + start angle from
live config every pass, so retuning the window does not need a reconfigure.

The window comes due as `EventAction::ADC_TRIGGER` (`:405`) → `on_knock_window(cyl)` (`:413-420`),
which is an ISR. It does exactly two things: push the cylinder into a power-of-two SPSC ring
(`knock_ring_`, drops on full rather than blocking) and pend the worker. `knock_window_fires_`
counts every window independent of the mailbox, so a bench session can tell "the window never fired"
from "the window fired and the worker could not keep up". The `fire` CLI reports the three counters
together: `knock_windows=` (dispatched), `knock_cap=` (bursts armed), `knock_trunc=` (windows the
buffer cut short).

### 2. Burst + DSP — `firmware/main.cpp:277-320` (the knock worker task)

The worker drains the mailbox and for each cylinder:

- Resolves the sensor from `cyl_bank[cyl].sensor`; skips anything above 1 (two inputs exist).
- Reconfigures the DSP band only when the frequency changed — worker-owned, so no race with
  `process()`.
- Scales the sample count to `window_duration_deg` at the current RPM
  (`one_deg_us = 1666666.7 / rpm_x10`), floored at 100 samples and clamped to the 2048-sample buffer.
  Every clamp bumps `s_knock_truncs` — see "The window closes on a sample count" below.
- Arms `board_knock_start_burst()` (non-blocking) and **sleeps on a semaphore** given by the
  DMA-complete ISR, with a 10 ms timeout guard. No CPU poll-spin.
- Runs `KnockDetector::on_burst()` → Biquad bandpass + RMS → dB, publishes `SIG_KNOCK_1/2`, and
  calls `Knock::on_knock_sense(cyl, db)`.

The sample buffer `s_knock_buf` lives in `.dma_nocache` — mandatory for any DMA target now that the
M7 D-cache is enabled. It is **2048 samples (4 KB)**; the `.dma_nocache` region is a hard 8 KB
reservation in the linker script and the three DMA buffers now total 4736 B.

### The window closes on a sample count, not an angle

`window_start_deg` is a real angle-domain scheduler event. `window_duration_deg` is **not** — it
never reaches the scheduler. The worker converts it to a sample count, so the window is bounded in
TIME by the buffer and therefore truncates in ANGLE at *low* RPM, which is the opposite end from
where you would expect to run out. At 281.25 kHz, 2048 samples is 7.28 ms. For the default 40°
window (`count = 1875000 / rpm`):

| RPM | samples wanted | window actually sampled |
| --- | --- | --- |
| 500 (cranking) | 3750 | 21.8° — clamped |
| 915 | 2048 | 40.0° — the crossover |
| 2000 | 938 | 40.0° |
| 6000 | 313 | 40.0° |

The buffer was 512 samples until the crossover was measured at ~3660 rpm — meaning a 40° window at
idle was silently sampling ~11°, precisely where lugging knock lives. `s_knock_truncs` counts every
clamp and is reported by the `fire` CLI as `knock_trunc=`, so the ceiling is visible on the bench
instead of reading as a quiet engine. **A non-zero `knock_trunc` means the window you configured is
not the window that was sampled.**

**The ADC3 either/or is real.** Arming a burst sets `s_adc3_knock_active` and re-tasks ADC3 from the
AV13-16 scan to a single-channel one-shot DMA on the knock pin
(`board_hal_jaytek_v1.cpp:484-503`); the completion callback routes to knock instead of the AV13-16
fold. Enabling onboard knock **sacrifices the AV13-16 analog scan**. That is the whole reason the
external-signal source exists.

### 3. Control — `firmware/Engine/Modules/Knock.cpp`

`on_knock_sense(cyl, db)` (`:38-52`) applies `cyl_bank[cyl].gain` (0.1 dB units), peak-holds
`level_db_`, and on `db > threshold_db` accumulates `retard_step_deg` clamped to `max_retard_deg`,
bumping `count_`.

`update()` (`:54-113`) runs the rest:

- **TPS suppression.** Below `suppress_min_tps` it zeroes the retard and sets `suppressed_`, which
  also gates `on_knock_sense` — the worker can land a measurement between our ticks, so the flag has
  to bar both paths, not just this one.
- **Decay.** `retard_deg_` falls toward 0 at `retard_reapply_rate`; `level_db_` decays at a fixed
  20 dB/s for telemetry only.
- **External source.** When `source != 0` it reads `external_intensity_sig` off the bus and feeds it
  as a **global cyl-0 sense** — one lane, no per-cylinder resolution.
- Hands the result to `Ignition::apply_knock_retard()`, which subtracts it from advance and
  publishes `wk::knock_retard`.

**Cadence is 100 Hz**, not 1 kHz: the participant is registered on the `KHZ_1` task with decimation
`cadence::Knock = 100` (`SystemComposer.cpp:122`, `generated/module_cadence.h:33`). It is ordered
before ignition so the retard is set for the same frame that uses it.

## Config surface — the `Knock` schema module

`enabled`, `source` {onboard=0, external}, `knock_frequency`, `window_start_deg`,
`window_duration_deg`, `external_intensity_sig`, `threshold_db`, `retard_step_deg`,
`max_retard_deg`, `retard_reapply_rate`, `suppress_min_tps`, and `cyl_bank[12]` of
`{sensor, gain}`. `sizeof(KnockConfig) == 56`.

## Divergences from the original design — deliberate or not yet built

These were specified in the first draft of this document and are NOT in the shipped code. Listed so
the gap is visible rather than assumed present:

- **Threshold and max-retard are flat scalars, not RPM curves.** The design called for generic
  `TableEngine` curves vs RPM; `threshold_db` and `max_retard_deg` are single values with no RPM
  dependence. This is the most consequential gap — a threshold that works at 6000 rpm is deaf at
  idle.
- **Retard is a fixed step, not fraction-to-floor.** The design called for
  `(timing − floor) · retard_aggression`; we add a constant `retard_step_deg` per event. No
  `retard_aggression` field exists.
- **No fuel-trim enrichment.** `fuel_trim`, `fuel_trim_aggression`, `fuel_trim_reapply_rate` are
  absent; knock retards timing and nothing else.
- **No `double_frequency`** (the 2× harmonic option).
- **`knock_frequency = 0` does NOT derive from bore.** The schema help still advertises
  `900/(pi*bore/2)`, but `main.cpp:288` falls back to a flat **7000 Hz**. Either the derivation gets
  written or the help text is a lie — currently it is the latter.
- **DTCs are sensor-health only.** `KNOCK_1` (0x1750) / `KNOCK_2` (0x1751), severity 1, raised when
  the corresponding bus signal is invalid, and **only for the onboard source** (`Knock.cpp:87-95`).
  There is no "sustained knock" DTC despite schema bit 9 (`knock`, P0324-P0334) existing for it.
- **No studio surface.** Stage 4 is not started: no knock config UI, no gauges, no knock scope. The
  telemetry signals (`knock_1`, `knock_2`, `knock_level`, `knock_count`, `knock_retard`) are all
  published and datalogged, so a dashboard has something to bind to today.

## Bench validation

- **DSP + ADC3 burst** (`e890565`, the `knock` CLI): an 8 kHz wave-gen tone into KNOCK1 peaks the
  band level exactly at 8 kHz (−39.9 dB) with symmetric falloff and ~13 dB margin over the
  no-signal channel — confirming the burst, the 281.25 kHz sample rate, the Biquad bandpass and the
  RMS.
- **Window + fire counter** (`4326dc1`) and **end-to-end stage C** (`53293dc`), both bench-validated.
- **Interrupt-driven burst** (`81ac317`) replaced the original blocking poll-spin.
- **NOT yet bench-validated:** the 2048-sample buffer, `s_knock_truncs`, and the semaphore timeout
  raised from 10 ms to 25 ms. All build clean and the linker region was checked against the ELF
  (4736 B of the 8 KB reservation), but no burst has run on hardware since. The timeout had to move
  with the buffer: a full burst is now 7.28 ms, so 10 ms left only 2.7 ms of margin, and a timeout
  there drops the measurement silently — which reads as a quiet cylinder.

Host tests: `tests/test_knock.cpp` (controller), `tests/test_knock_dsp.cpp` (synthetic knock/no-knock
waveforms), `tests/test_knock_detector.cpp`.

## File map

| Concern | File |
| --- | --- |
| Controller | `firmware/Engine/Modules/Knock.{h,cpp}` |
| Detector façade | `firmware/Engine/Modules/KnockDetector.{h,cpp}` |
| Bandpass + RMS | `firmware/Engine/Modules/KnockDsp.{h,cpp}`, `Biquad.{h,cpp}` (Q defaults to 3.0) |
| Window scheduling | `firmware/Scheduler/EnginePositionHal.{h,cpp}` |
| Worker task | `firmware/main.cpp:230-320` |
| HAL burst | `firmware/Platform/board_hal.h:48-63`, `boards/jaytek_v1/board_hal_jaytek_v1.cpp:427-503` |
| Config | `definition/ecu.schema.yaml` (`Knock` module) → `generated/modules/knock_config.h` |

## Open

- The RPM-curve threshold / max-retard gap above — the largest remaining correctness item.
- Spectrogram transport + format, if the studio scope is ever built (FFT stays off the control path
  regardless; the decision that control DSP is Biquad + RMS, not FFT, still stands).
