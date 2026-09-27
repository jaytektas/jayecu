# Generic Trigger Decoder — design spec

**Status:** DONE — merged to main (merge `1094abd`, 2026-06-02). The `SyncStrategy` zoo is
replaced by a generic stream/primitive pool; the legacy `TriggerDecoder` + `SyncStrategy` +
old host suites are deleted. This doc is the design record.
**Goal:** replace the per-wheel `SyncStrategy` zoo with a generic *pool of decode primitives*
applied to one or more signal *streams*, fused into engine position. Wheel knowledge
(presets) moves entirely client-side (the studio's preset selector); the firmware only ever
sees a generic stream config. "All primitives, all wheels."

## Why

The strategy enum conflates two independent things — *what kind of crank signal* and *what
kind of cam signal* — into one value, so every crank-type × cam-type combination needs its
own code path (the gaps we kept hitting: PATTERN_EXCEPT had no cam promote; 6g72 = even crank
+ multi-cam fit nothing). And the matchers built this session (ODD_FIRE, PATTERN_EXCEPT, the
cam-pulse matcher) are secretly **one algorithm** — match a cyclic event-interval sequence —
applied to three different streams. The PLL/VirtualTrigger is already wheel-agnostic. So the
generic model is mostly *deleting duplication*, and it makes the firmware simpler, not bigger.

## Model

The engine cycle is the 720° (4-stroke) tracked by the existing **VirtualTrigger PLL/DCO**
(unchanged — a generic angle clock). The decoder's only job: from N signal streams, produce
(a) a velocity estimate and (b) an absolute angular reference, and feed the PLL.

### Stream
One captured signal: `{ capture_index, edge, rate, primitive, params }`
- **rate**: `CRANK` (pattern repeats every 360°) or `CAM` (every 720°). Sets the stream's
  period = 3600 / 7200 decideg. A "wheel" mounts at crank-rate or cam-rate — that's the only
  crank/cam distinction left.
- **primitive**: which reference-finder runs on this stream's edges (pool below).
- **params**: small, primitive-specific (a few scalars + a ≤8-entry cell).
- Up to `MAX_STREAMS` (4). Typically 1 (crank-only) or 2 (crank + cam). Each active stream
  has its **own** param block (streams run concurrently — no union).

### Primitive pool (all stream-agnostic: edges → reference)
Each primitive advances the stream's intra-period angle per edge and reports a reference as
`NONE` (relative only) or `LOCKED@angle` (absolute within the stream's period).

| primitive | params | replaces | notes |
|---|---|---|---|
| `EVEN`    | N | DISTRIBUTOR | uniform teeth, relative lock (no unique point) |
| `GAP`     | N, missing[] | MISSING_TOOTH, PATTERN_EXCEPT | one or more missing-tooth gaps at known indices; gap(s) → absolute. Parametric (compact) — a 36-1 stays N=36/miss=1, NOT a 35-entry cell |
| `SEQUENCE`| cell[≤8] of inter-edge angles | ODD_FIRE, cam-pulse phasing | match the measured interval-RATIO sequence (argmin, RPM-invariant) → absolute index. The workhorse for irregular teeth AND cam pulse patterns (6g72). Crank-resolution-independent |
| `WIDTH`   | min,max,target (deg) | WIDTH_IDENTIFICATION | the pulse whose width — measured in **time × velocity → degrees**, NOT crank-teeth — falls in [min,max] is the reference. (The crank-teeth count broke on coarse cranks like 6g72) |
| `LEVEL`   | pattern,len | STATE_TRANSITION | bit-pattern of edge states (low priority; few wheels) |

`GAP` stays parametric (not folded into `SEQUENCE`) so dense single/multi-gap wheels store a
few numbers, not a full per-tooth cell (honours "no large patterns").

### Fusion (streams → engine position)
Each locked stream offers: an intra-period angle (mod 360 for CRANK-rate, mod 720 for CAM)
and whether it's an absolute reference. The decoder keeps one 0..720 cycle position:
- **velocity / fine angle** ← the *finest* locked stream (most edges → best resolution); it
  feeds the PLL exactly as today.
- **revolution (720° bit)** ← any stream that resolves uniquely over 720°.
- Result: `CAM`-rate stream resolves → PHASE; `CRANK` absolute only → CRANK; `CRANK` fine +
  `CAM` rev → PHASE (this is exactly today's cam-phase, generalised).

Every case we hit reduces to this:
- missing-tooth crank + 1 cam pulse → crank fine (GAP) + cam rev → PHASE
- even crank + cam pattern (6g72) → crank velocity (EVEN, relative) + cam SEQUENCE (abs) → PHASE
- CAS cam-only → one CAM-rate stream provides everything
- dual crank wheels → two CRANK-rate streams fused

## Config / schema (firmware side only — presets are client-side)
```
Trigger:
  stream_count : uint8
  # live sync-health tolerances stay global (sync_window_pct, sync_loss_revolutions, ...)
  config_arrays:
    streams[MAX_STREAMS]:           # each ~ {u8 capture_index, u8 edge, u8 rate,
                                    #         u8 primitive, u16 n, u16 missing_mask/…,
                                    #         i16 p0, i16 p1, CellEntry cell[8]}
```
No `strategy` enum, no presets, no preset storage in firmware. The studio's preset selector writes
`stream_count` + each stream's fields for a chosen wheel; the future visual configurator
writes the same. Per-stream cell buffers are separate (concurrent streams) — ~4×8×2 = 64 B.

## Reuse vs rewrite
- **KEEP unchanged:** `VirtualTrigger` (PLL/DCO), `EventScheduler`, `EnginePositionHal` pool/
  firing, capture HAL, codegen machinery, the schema-driven shadow/reconfigure plumbing.
- **REWRITE:** `TriggerDecoder` → a generic multi-stream decoder (per-stream primitive state +
  fusion). The trigger schema config block → generic streams. `SyncStrategy` enum → per-stream
  `primitive` enum.
- **PORT the proven math** into clean per-primitive functions: gap-detect, the interval-
  sequence matcher (one copy, shared), width (time-based), even-lock. The 22 bench-proven
  wheels + ~110 host assertions are re-expressed as stream configs and become the regression
  harness.

## Build order (test-first throughout)
1. `SEQUENCE` primitive as a standalone, exhaustively-tested unit (subsumes odd-fire / multi-
   gap-spacing / cam-pulse). Solves 6g72.
2. `GAP`, `EVEN`, `WIDTH` primitives (port + test).
3. Stream abstraction + per-stream state.
4. Fusion + the CRANK/PHASE level machine.
5. Generic schema + codegen; regenerate.
6. Re-run every wheel on the bench as a stream config; port the host suites.

## Risks
- **Fusion state machine** is the new complexity — multi-stream concurrency, level transitions.
  Mitigate: heavy host tests with synthetic crank+cam streams (harnesses already exist).
- **Hot-path determinism** — per-stream dispatch via function table vs `switch`; keep the
  capture ISR tight.
- **Config surface** — solved by the client-side-preset decision: firmware exposes only the
  generic config; complexity hidden behind studio presets / the future visual tool.
