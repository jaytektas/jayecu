# DTC system — redesign

Status: **proposed, not built.** The table it describes is the one in the ECU today; the shape it
proposes is a change to how producers talk to it. Written after four instances of the same bug were
found in one night, because the fourth one made it clear they are not four bugs.

## What is wrong

### Healing is the raiser's job, and 68 call sites have to remember

`raise()` and `heal()` are two verbs for one fact — *is this condition true right now* — and every
producer re-derives the transition between them for itself. Twenty files, sixty-eight calls.

The failure mode is always the same and never looks like a code bug. A module that returns early when
it is switched off never reaches its own `heal()`, so a code it raised while running stays CURRENT for
ever. Found, in order:

| where | what happened |
|---|---|
| Knock | enable it, let it raise, disable it — code stays current. Fixed per-module with a `was_enabled_` block. |
| Boost, Launch, Stepper, TransientThrottle, TractionControl, CruiseControl | the same block, copied. |
| Sensors, operating-window codes | the policy loop SKIPS op codes when the window did not evaluate — and a check that has been unticked does not evaluate. Nothing could heal it. |
| Misfire | never healed at all: not when the engine came good, not when the module was switched off. |
| H-bridge | healed correctly, but on every frame — 15 µs of every 1 kHz frame retiring codes that had never been raised. |

Six modules carry bookkeeping to work around the design; two more got it wrong; one got it right and
paid for it. That is the design, not the modules.

### The cost was pushed onto the callers, so they avoided paying it

`heal()` walked 64 records of 36 bytes: about 600 cycles. Sensors' own comment records the
consequence — healing every frame "was measured in milliseconds" — so it was narrowed to "on a config
change", which was still every disabled sensor healing six codes on every write to anything: 2.0 ms of
a single engine frame. The optimisation that hid the cost is the thing that stranded the codes.

(Both are fixed as of 2026-09-16 — a presence filter makes an absent code O(1), and the table ages
what nobody asserts. The *incentive shape* is unchanged: a producer that calls less often is faster.)

### Latching versus level is chosen per call, invisibly

A pre-ignition strike, a misfire tally and a Lua `setDtc` are EVENTS: nothing re-asserts them and they
must stand. Everything else is a CONDITION: it is true while the judge says so. Today that distinction
lives in whether a particular call site happens to pass `DTC_TTL_LATCH`, which a reader cannot see from
the code's declaration and a new module will get wrong.

### Severity and source arrive with the call, not with the code

The same code can be raised at different severities from different paths; `source` likewise. Both are
properties of the fault. The sensor layer already does the right thing — severity comes from the tune,
two bits per diagnostic slot — and every other producer passes a constant at the call site.

### Dead concepts in the record

`DTC_CONFIRMED` is declared and never set. `heal_cycles` is zeroed and never counted. OBD's
pending-versus-confirmed (a fault seen in one drive cycle versus two) is not implemented, so Mode 07
cannot mean what it means on any other ECU.

### The source id space is copied into the studio

`DtcSource` numbers live in the firmware and are duplicated in `DtcDock::sourceName()`. That has
already bitten once: the catalogue grew past `SUBSYS_BASE` and sensors 91-98 reported as "Pin arbiter"
and "Config". A `static_assert` stops the overlap; nothing stops the copy drifting.

## The design

Jason's, and it is better than the publish-only one this document proposed first. The difference is
not elegance, it is which way the mistake falls.

**Producers keep `raise()` and `heal()`. The table sweeps what is ACTIVE and retires anything nobody
is still asserting.** A module that is switched off simply GATES — returns early, as it already does —
and says nothing at all. It needs no heal block, no `was_enabled_` flag, no list of its own codes.

### Why keeping two verbs is the right call

The publish-only version made forgetting impossible by removing the thing you could forget. But the
rule underneath is the same either way — *a condition producer re-asserts while the condition is true*
— and what actually matters is what happens when somebody breaks it:

| | forgetting, before tonight | forgetting, under the sweep |
|---|---|---|
| result | the code stands for ever | the code goes out early |
| recovery | none, until a reset | the next evaluation raises it again |
| how it presents | a warning light nobody can clear | a light that flickers |

The failure flips from permanent to self-correcting. That is worth more than one verb.

### The sweep can only expire, not verify

Worth being exact, because "verify their current state" reads like the table could re-evaluate a
condition itself. It cannot: nothing outside the producer knows whether oil pressure is low UNDER LOAD,
and asking the producer out of band means asking it at a moment its inputs may not be valid. So the
sweep's question is the only one it can ask — *has the producer said so recently* — and the rule that
makes the answer meaningful is that producers keep saying so.

**That rule already holds.** Every condition producer in the firmware re-asserts while true; the last
six exceptions were Sensors', converted 2026-09-16. So this is not a design to build — it is one the
ECU is already running, with the module-side bookkeeping left over from before.

### Which codes actually have to stick

Three were marked `DTC_TTL_LATCH` on the reasoning that they are events. Checked one at a time, and
only one of them survives the check:

**Pre-ignition — no.** It already has a release, and it is the tune's: `preign_clear_s` seconds after
the last strike, Knock clears the cut, resets the tally and heals the code itself (`Knock.cpp:400-408`).
That is a ttl with a number the user chose, written by hand. The latch flag on the raise is redundant.

**Misfire — no.** `classify()` runs per SEGMENT, and re-raises every time the tally is over the
threshold, so it asserts continuously while the engine turns. The latch only changes what happens when
the engine STOPS: latched, the code stays current with the engine off; aged, it drops to stored. The
second is the more honest statement — "currently misfiring" is not true of a stopped engine, and the
history is what STORED is for.

**Lua `setDtc` — yes.** This one is a published API. `setDtc(code)` / `clearDtc(code)` is a contract
with somebody's script, and a script that sets a code in a one-shot path and expects it to stand is
using the contract as documented. Ageing it would break scripts silently — their fault light would go
out on its own and the script would never know. (A script that calls `setDtc` inside `onTick` while a
condition holds re-asserts naturally and would be fine either way; the point is that we cannot tell
those two scripts apart, and only one of them survives the change.)

**So latching is not a category, it is an infinite ttl** — and almost nothing wants one. What looks
like an event usually wants a long FINITE freshness: "still true for N seconds after the last
evidence", which is what pre-ignition already does with a number from the tune. The useful question
per code is not "is this an event" but "how long after the last evidence should this still be
current", and for the codes that are not continuously re-evaluable the tune often already answers it.

### The ttl belongs to the producer, not to a constant

A flat second is thin for the slowest judges. The module cadences run down to 20 Hz and the sensors to
2 Hz — a 500 ms evaluation interval against a 1 s ttl is two periods of margin, and one missed
evaluation puts a real fault out.

`ttl_for(hz)` already exists, and `EngineTask::add_participant` already computes `sched_ttl_` per
module from its declared cadence — the same number the signal bus uses to decide when a value has gone
stale. A raise should carry that, so a 2 Hz sensor gets a long freshness and a 1 kHz module a short
one, and nobody has a constant to tune. (Today it is a flat `DTC_TTL_DEFAULT` of 3 s, which is safe
but blunt.)

### What it deletes

- The `was_enabled_` heal blocks in six modules, and the flags behind them. They become an
  OPTIMISATION — "go out now rather than in a ttl" — worth keeping only where the immediacy matters.
- The rule a new module has to be told.

It does NOT delete the 68 `heal()` calls, and does not need to: a producer that knows its condition has
cleared should still say so, because that is faster and more precise than waiting to be aged out.

## Open, and not for me to decide

- **OBD pending/confirmed**: implement it properly (two drive cycles, Mode 07 vs Mode 03) or delete
  the dead flag? It is the difference between "this ECU speaks OBD" and "this ECU has a fault table".
- **Severity in the schema**: moving it off the call sites means declaring it for every module code,
  which is a schema edit per code. Worth it?
- **The studio's source names**: ship them in the meta and delete the copy, or keep the duplication
  and the static_assert?
