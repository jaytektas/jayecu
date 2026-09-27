# Expression evaluator — design spec

**Status:** PROPOSED — nothing built. Written to be marked up; the open decisions are at the end and
every one of them changes the code, so they want answering before anyone writes it.

**Goal:** replace the fixed four-slot precondition with a **compiled expression**: the studio
tokenises and compiles, the firmware runs a small stack machine over the tokens. Precedence and
grouping become expressible, storage stays the size it is today, and the same evaluator serves every
other "arm this when…" setting in the platform instead of each one growing its own encoding.

## Why the current form cannot be fixed in place

A sensor carries four inline tests, each `signal <op> value` with a per-slot combine, folded
sum-of-products (`Condition.h`):

```
IF A AND B OR C AND D   ->   (A AND B) OR (C AND D)
```

One flag per test, no grouping — the header says so plainly. So `A AND (B OR C)` **cannot be
written at all**, and the fold is positional: what a slot means depends on where it sits, which is
why the schema needs a paragraph to explain slot 0 vs slots 1..3. Adding a "this test opens a
bracket" bit would buy one level of nesting and make the encoding harder to explain, not easier.

The tests are also arithmetic-free. `map > baro + 20` — a plausibility gate that follows ambient —
has no expression, only a constant.

## Shape: client compiles, firmware executes

Two halves, each doing what it is already good at.

**Studio (compile).** `MathEvaluator` already tokenises with precedence, associativity, functions
and parentheses. Shunting-yard turns that token stream into RPN; a second pass emits bytecode.
Errors are reported where the user is typing, with the source in front of them.

**Firmware (execute).** A `switch` in a loop over a small float stack. No parser, no allocation, no
strings, no dynamic memory, no loops in the program itself — so execution time is bounded by
program length and termination is structural.

## Encoding

One-byte opcode, optional two-byte operand. Operands are little-endian.

| opcode | operand | stack effect | meaning |
|---|---|---|---|
| `PUSH_SIG` | u16 sel | → value | bus channel; `sel` is the existing `options_from:signals` convention (0 = None, else `SignalId + 1`), read through the existing `read_signal()` |
| `PUSH_CFG` | u24 offset, u8 type | → value | a config field — the sigil case, so a gate compares against a *setting* rather than a baked constant. See "Addressing config" for the packed and SELF variants |
| `PUSH_CONST` | i32 ×100 | → value | literal, the same fixed-point the current `value` field uses (0.01 resolution, no unaligned float load on the M7) |
| `GT GE LT LE EQ NE` | — | a b → bool | comparison |
| `AND OR` | — | a b → bool | logical |
| `NOT` | — | a → bool | logical negation |
| `ADD SUB MUL DIV` | — | a b → value | arithmetic |
| `MIN MAX` | — | a b → value | clamping helpers, cheap and often wanted |
| `ABS` | — | a → value | |
| `CLAMP` | — | a lo hi → value | |
| `SELECT` | — | c a b → value | ternary; `c ? a : b` |
| `BIT` | — | a n → bool | test bit n of a — status words compare as words today |
| `INTERP` | u16 table-id | x → value | look up a calibration curve/map already in the tune |
| `END` | — | — | terminate; the result is the stack top |

`rpm > 2500 AND (map > 50 OR tps > 80)` compiles to:

```
PUSH_SIG rpm   PUSH_CONST 250000   GT
PUSH_SIG map   PUSH_CONST   5000   GT
PUSH_SIG tps   PUSH_CONST   8000   GT
OR   AND   END
```

11 opcodes, **25 bytes**. For reference the four `precond` slots occupy 32 bytes per sensor today —
but the program is **not** constrained to fit them. The layout is free to change, so size the block
for what expressions actually need rather than squeezing into the space an encoding we are deleting
happened to use.

## Addressing config: bake the offset, no descriptor table

Much of the config is **bit-packed** — a sensor's six diagnostic checks share one byte, its
severities pack two bits each, `flags.invert` is one bit — and those are exactly the values a gate
wants to read. So config access must handle bit ranges natively, not just whole fields.

The client already knows every offset and bit range: that is what the meta is. So it **bakes them
into the instruction** rather than the firmware carrying a lookup table:

| opcode | operand | meaning |
|---|---|---|
| `PUSH_CFG` | u24 offset, u8 type | whole field: read per `type` (U08/U16/S16/U32/F32) at `offset` |
| `PUSH_BCFG` | u24 offset, u8 type, u8 bits | packed: `bits` = `lo` (5 bits) \| `width-1` (3 bits), so widths 1..8 |

(There is no `SELF` form. It existed only so one shared program could read "whichever sensor is
evaluating"; with per-sensor programs — decided — the compiler knows the element index and bakes the
absolute offset, so the VM needs no base pointer and no relative addressing.)

The config image is 105,085 bytes, so an offset needs 17 bits — u24 covers it with room, and keeps
the instruction byte-aligned. `PUSH_BCFG` is 6 bytes, `PUSH_CFG` 5.

This is better than a generated descriptor table in three ways, and worse in one:

- **No table.** ~780 entries × 12 bytes of flash saved, and no second artefact to keep in step with
  the layout.
- **No scale in the VM.** The descriptor version carried a `float scale` so gates could compare in
  engineering units. Baking offsets removes it: the compiler converts the *constant* into the
  field's raw domain instead, so `clt > 90` becomes a comparison against raw counts with no runtime
  multiply. Fewer opcodes, no float scaling on the M7, and the arithmetic stays exact.
- **No relative addressing at all.** A per-sensor program is compiled for one element, so the
  compiler bakes the absolute offset of `sensors.sensor[i].whatever`. The VM never needs a base
  pointer, and "my own operating window" is just another baked offset.
- **Weaker validation (the cost).** With a table, the firmware could reject any operand that was not
  a real field. With baked offsets it can only check that `offset + size` is inside the config image
  — a wrong-but-in-range offset reads a neighbouring field rather than being rejected. Note it cannot
  check ALIGNMENT either: the config is `#pragma pack(1)`, so a perfectly correct offset is routinely
  unaligned, and every config read therefore goes through `memcpy` (an unaligned `VLDR` would fault
  the M7). That is the client's job to get right, and the same trust the tune image
  already relies on everywhere else.

### Where the offsets are safe, and where they are not

Baked offsets are safe in the config **image** — every byte there is layout-bound already, which is
what `layout_hash` is for.

They would not be safe in the **tune**, which is name-addressed on purpose: `TuneFile` writes
`{"scalars": {"<field name>": value}}` so each value can be looked up by name and re-encoded at
whatever offset the current layout gives it. A blob of baked offsets dropped into that would be
carried across a layout change verbatim and would then address different fields — a silent wrong
answer rather than an error.

That is not a reason to avoid baking offsets; it is a reason to keep them out of the tune. The tune
holds the **source text**, the image holds the **bytecode**, and writing the image always compiles
from source, so its offsets are correct by construction.

The corollary is worth stating plainly even though no migration is planned: program bytes must never
be copied from one layout's image into another's. Recompile, or report it as unmapped. Never
transplant.

**A tune staged on SD for the next boot is already covered.** `main.cpp` accepts a stored image only
when its size AND its `layout_hash` match the running firmware exactly — the comment there records
the bug that earned the check ("a same-size layout change left 6 sensors with garbage
enabled/interface bytes"). So a pending image written for the old layout is rejected wholesale on
power-up and the firmware keeps its compiled defaults; a program's baked offsets cannot be applied
against a layout they were not compiled for. Nothing extra is needed for expressions.

The residual gap is reporting, not correctness: a rejected image looks to the user like their tune
quietly reverted to defaults. Worth a notice from the studio on connect ("the ECU is running
defaults; the stored tune was built for layout X"), and it applies to every field, not just gates.

### Sizing the program block

With the layout free, pick the size from the work rather than from history:

- a two-clause gate with a constant each side: ~25 bytes
- the same reading a packed config flag: ~31 bytes
- three clauses with arithmetic (`map > baro + 20 AND rpm > 2500 AND clt > 60`): ~45 bytes

**64 bytes per program** covers everything realistic with room to spare, at 122 sensors = 7.8 KB of
config — up 3.9 KB on today's 32-byte block. Cheap.

Two shrinking opcodes are still worth having, because the common gate is a comparison against a
small number: `PUSH_CONST8`/`PUSH_CONST16` (saves 2–3 bytes each) and `PUSH_ZERO`/`PUSH_ONE` for
flag tests. They cost four opcode values and nothing else.

If per-sensor blocks feel wasteful — most sensors will carry no program at all — the alternative is
a **shared pool**: N programs in one region, each sensor holding a u8 index. That is open decision 7,
and it is now purely a usability question ("edit one gate, every sensor using it follows") rather
than a storage one.

## Cost

**Flash is not the constraint, and should not be used as one.** A richer instruction set costs a few
hundred bytes of switch arms in a kernel that is already compact, against plenty of free image. Any
opcode that is a *pure function of now* — the arithmetic above, `CLAMP`, `SELECT`, `BIT`, curve
lookup, more comparison and conversion helpers — should be added freely on the grounds that someone
might want it. That is the cheap kind of power and there is no reason to ration it.

The two real bills are elsewhere.

**Every opcode is maintained in three places plus a UI.** Compiler, VM, decompiler — and the studio's
editor, help text and tests. Adding `CLAMP` is genuinely free; adding a construct with its own syntax
and its own editor affordance is not, and the cost lands on the studio, not the M7. This argues for a
generous set of *values and functions* and a stingy set of *forms*.

**State is a different currency: it buys a contract, not code.** See "Stateful operators" below.

~25 dispatches through a switch, one bus read per `PUSH_SIG`. With the M7's I-cache and ART enabled
that is comfortably sub-microsecond. Only sensors carrying a program pay anything: an empty program
is one byte (`END`) and the caller skips it. Worst case today (122 sensors, 200 Hz, all with
programs) is ~24k evaluations/second — an order of magnitude below the trigger decoder's budget.

Bounded by construction: no loops, no calls, no recursion. Length is capped by the block size, so
the worst-case execution time is a constant you can state.

## Semantics

**Fixed-point in, float on the stack.** Constants are stored ×100 and converted on push, so tune
bytes stay integer and the M7 never does an unaligned float load. Enum-valued channels compare as
their integer value, as they do today.

**Invalid or stale operands.** Today's rule is fail-to-false: a None, out-of-range or stale signal
makes its test false, so a dead sensor cannot satisfy a gate. Two ways to carry that through a
stack machine:

- **Strict (recommended).** Track validity alongside the stack; any invalid operand makes the whole
  expression `false`. Identical to today's behaviour, and impossible to be surprised by.
- **NaN propagation.** Push NaN for an invalid read and let IEEE comparisons fall false. Elegant for
  comparisons, but `NOT(stale)` evaluates *true*, which is a trap for exactly the person writing a
  safety gate.

## Validation

The M7 must never execute unvalidated bytecode. Validation happens twice:

- **At burn (studio):** opcode range, operand range, stack depth never negative and never over the
  limit, exactly one value left at `END`, program terminated within the block.
- **At load (firmware):** the same structural checks, cheaply, once per config load — not per
  evaluation. A tune can arrive from an SD card or a partial write, so the firmware cannot assume
  the studio wrote it.

**A program that fails validation raises a config DTC** (the existing P16xx block, beside "input not
assigned") **and falls back to always-armed.** A corrupt expression must not silently switch
diagnostics off — same principle as a check with no DTC code: detection that reports nothing is
worse than no detection at all.

## Round-tripping: the ECU is still self-describing

RPN → infix is mechanical, so the studio can **decompile bytecode back to readable source**. An ECU
read cold, with no project file, still shows `rpm > 2500 AND (map > 50 OR tps > 80)` — provided the
studio holds the meta for that image's `layout_hash`, which the library is keyed by.

The tune keeps the source as well (see "The tune stores SOURCE"), so the two could in principle
disagree if something edited one without the other. The rule is that **source is authoritative and
bytecode is derived**: a burn always recompiles from source, and a decompile is used only when there
is no source to be had.

Cosmetic detail: decompilation normalises spacing and may add parentheses the user did not type.
Round-tripping *meaning* is guaranteed; round-tripping *characters* is not.

## Where else it pays

DTC gates are the first customer, not the reason. The same evaluator retires several one-off
encodings and is expected to carry output and arming logic:

- `engine_protection.dtc_condition` — in the schema, in the config struct, **read by nothing**
  today, and exactly this shape.
- Enable/visibility conditions, which the studio already evaluates client-side with the same
  grammar.
- "Arm this module when…", instead of each module growing its own slot format.
- Output control of the fuel pump, thermo fans, and anything else whose rule is a condition over
  channels the bus already carries.

One grammar, one editor, one validator, one decompiler.

### What that expansion demands of the design

Three constraints follow, and they are cheap now and expensive later:

**The program is a field type, not a sensor feature.** "Per sensor" (decided) generalises to **per
owning site**: a program belongs to the field that owns it — this sensor's gate, this fan's on-rule,
this module's arm condition. The reasoning against a shared pool is unchanged and applies at every
site: a pool couples things that have no reason to be coupled, and needs a UI to answer "what else
does editing this change?".

**The VM stays pure — no side effects, no state.** It reads config and signals, and returns a value.
It never writes an output, sets a flag, or remembers the previous evaluation. A program that could
poke an output would need its execution ORDER specified against every module that also writes that
output, and the whole thing stops being reviewable. The caller owns the action: the VM says "this
condition is true", the fan driver decides what to do about it.

**Hysteresis and timers belong to the site, not the grammar.** A fan on a bare `coolant > 95`
chatters at the threshold. The fix is an on-threshold and an off-threshold at the site, plus a
minimum on-time — the same shape as `diag_delay_ms`, which the sensor gate already owns rather than
encoding in its condition. Putting "has been true for 5 s" into the expression language means every
program instance needs its own timer state, evaluated on a guaranteed cadence, and the pure stateless
VM is gone. That is the one choice that changes the VM's architecture rather than extending it, so it
gets its own section below rather than being waved off.

### Hysteresis needs no state at all

Worth stating before the stateful-operator question, because it removes most of the demand for one.

A pure VM cannot remember last frame — but hysteresis does not need it to. It needs the CURRENT
OUTPUT as an INPUT. A site that publishes its own state to the bus makes the rule expressible with
no stateful opcode:

```
clt > 95 or (fan_on and clt > 90)
    off -> on   above 95
    on  -> off  below 90
```

Read it as "come on above 95, and once on, stay on until 90". The memory lives in the bus, where it
is inspectable and logged, rather than inside an expression where it is neither. Verified on the VM
(`test_expr_compiler.cpp`): the same 92 °C gives a different answer depending on the state channel,
which is exactly the chatter a single threshold cannot avoid.

The requirement this places on a site is small and worth having anyway: **publish your output as a
channel**. Anything that wants hysteresis has to be observable first.

What this does NOT give you is a **minimum on-time** — that is time, not state, and `age()` cannot
supply it (`age` measures the last WRITE, and a module writing its state every frame reads as age
zero). A minimum on-time is either a site knob, like `diag_delay_ms`, or the stateful phase below.

### Stateful operators: affordable, but they are not free the way opcodes are

`HELD(cond, 5s)`, edge detect, latch, previous-value, an integrator — these are the ones worth
thinking about, because the named use cases want them. Minimum fan on-time IS a timer.

Their cost is not code size and not even RAM (a few bytes per program instance, statically sized —
affordable). It is three obligations that a pure VM does not have:

1. **A cadence contract.** `HELD(x, 5s)` is only true if the program is evaluated on a known,
   guaranteed interval. Evaluate it conditionally, or at whatever rate a caller happens to run, and
   the "5 s" is a lie that looks like it works on the bench. Any program carrying state must be bound
   to a fixed-rate slot, with the tick declared, and the VM given the elapsed time rather than
   reading a clock itself.
2. **Defined reset points.** Boot, config load/burn, module disarm, DTC clear. A latch that survives
   a burn — or one that silently doesn't — is a support call. Each stateful op needs its reset
   semantics written down, not discovered.
3. **Loss of "same inputs, same answer".** A gate that depends on history cannot be reasoned about
   from a snapshot, which is exactly what makes a misbehaving one hard to diagnose in the field.

None of that is prohibitive, and the conclusion is **not** "never". It is: state is a deliberate
second phase with a scheduler contract attached, not an opcode someone adds on a quiet afternoon.
Ship the pure VM with the generous function set first, because it is unblocked and useful now.

**Even with state available, prefer a site knob when the site has one.** A fan's minimum on-time as a
tune field is visible in the dialog, settable by someone who has never seen an expression, and
obvious in a screenshot. The same 5 seconds buried in a program is invisible unless you open and read
it. That is a discoverability argument, not a capability one, and it survives the VM getting as
powerful as you like.

Two consequences worth planning for. Sites multiply, so re-check the budget: the cost section's ~24k
evaluations/second assumed sensors alone, and output rules are a rounding error beside that — but a
program per output evaluated in the fast loop is not, and outputs should evaluate at their own rate.
And a boolean site coerces the result (`!= 0`) while a future numeric site can take the top of stack
as it stands, so the stack machine needs no change to serve both.

## Build order

**No migration, no backwards compatibility — decided.** Existing four-slot preconditions are not
carried over, existing tunes are not preserved, and the layout may change as often as it needs to.
That removes the compat translator, the staged rollout and the parity-with-`cond::eval` test, all of
which existed only to protect things we are not protecting.

What remains is ordinary build order:

1. **VM + compiler + decompiler** — **BUILT.**
   - `firmware/Signal/ExprIsa.h` — the instruction set + validator, shared by both halves so there
     is one opcode table, not two that can drift.
   - `firmware/Signal/Expr.h` — the executor. `tests/test_expr_vm.cpp`, 81 assertions, clean under
     ASan/UBSan, mutation-checked (11 deliberate defects, each caught).
   - `apps/studio-jf/src/model/ExprCompiler.{h,cpp}` — compiler + decompiler.
     `tests/test_expr_compiler.cpp` compiles every case and then RUNS it on the firmware's own
     `expr::exec`, so the two halves are tested against each other rather than each against itself.
2. **Repoint the block** — **DONE.** `precond` and `cond::eval` are deleted, not translated. The
   sensor carries `precond_expr`, a `type: expression` field (64 bytes). Sensors.cpp validates every
   program once per config change and runs it through `expr::eval_bool`. A broken program fails
   ARMED and raises its own per-sensor code (slot 6 of the per-sensor block), in `docs/dtc-codes.md`
   like every other code. Validated live on jaytek_v1 by `tools/bench_precond_expr.py`.
3. **Adopt elsewhere** — still to do. `engine_protection.dtc_condition` first, since it is unwired
   today and cannot regress. Fuel-pump and thermo-fan rules are the next customers (see "Where else
   it pays"); each needs its site to supply hysteresis and a minimum on-time, not the grammar.

### The tune still keeps the source

One thing survives from the migration discussion, on its own merit rather than for compatibility:
the tune stores the expression as **source text** and the image stores bytecode. Not for migration —
for editing. It is what lets the studio show you what you wrote without holding the exact meta, keeps
the authoritative form human-readable, and means a layout change can only ever cost a recompile
rather than an unrecoverable blob. A few dozen bytes of JSON on a host.

If you would rather the tune carried bytecode alone, the decompiler covers reading it back whenever
the matching meta is in the library — that is open decision 8.

## Status

Built and validated on hardware (2026-08-11): the VM, the shared ISA, the studio compiler and
decompiler, the `expression` schema type, the sensor precondition, the dictionary-drop editor, and
source-in-tune storage. What remains is adopting it at the other sites, and the stateful-operator
phase if it is ever wanted.

## Open decisions

Reviewed 2026-08-10 and 08-11: 7 and 8 are settled below. The recommendations on the rest stand as the working
answers unless someone says otherwise — they are recorded here so a later reader can see they were
chosen, not defaulted into.

1. **Strict validity or NaN propagation?** RESOLVED — strict, and tracked PER STACK SLOT rather than
   as one whole-program flag. Whole-program strictness looked right on paper but breaks the useful
   case: with MAP dead, `map > 50 OR tps > 80` must still arm on throttle alone, as it does today.
   So an untrustworthy value is marked and travels — arithmetic and comparison propagate it, AND/OR
   coerce it to false and produce a clean result (so OR can rescue a live clause), and **NOT
   propagates**, deliberately asymmetric with AND/OR, because a coercing NOT would make
   `NOT(dead > 5)` evaluate TRUE — the one trap that catches the person writing a safety gate.
2. **On a program that fails validation: always-armed + config DTC, or never-armed?** Recommend
   always-armed — a broken gate should not disable detection silently.
3. **Stack depth limit.** RESOLVED — 16 (`expr::STACK_MAX`), 64 bytes of frame, enforced by
   `validate()` and independently by `exec()`.
4. **How hard should load-time validation be?** (Built: range + structure, checked once per config
   change, never per frame. The layout-checksum idea below is still open.) With baked offsets the firmware can check range and
   alignment but cannot tell a wrong field from a right one. Accept that, or have the compiler emit
   a checksum of the layout it compiled against so a mismatched program is rejected wholesale?
5. **Functions?** RESOLVED — built: `MIN`, `MAX`, `ABS`, `CLAMP`, `SELECT` (ternary), `BIT` (test a
   bit of a status word), `INTERP` (look up a calibration curve already in the tune, via a hook the
   caller supplies so the kernel stays free of table-engine dependencies) and `AGE` (channel
   staleness in ms, from a `now_ms` passed IN, so it stays a pure function of its inputs). More can
   be added freely — see "Cost".
6. **Program size.** RESOLVED — 64 bytes (`expr::PROGRAM_MAX`). Measured rather than guessed: the
   realistic four-clause gate in the test suite (`map > baro + 20 AND rpm > 2500 AND clt > 60 AND
   <packed config flag>`) compiles to **36 bytes**, so the block has comfortable headroom.
7. **Per-sensor programs, or a shared pool?** RESOLVED — per owning site (per sensor, per fan, per
   module). A pool would couple sensors
   that have no reason to be coupled, needs a management UI to answer "what else does this change?",
   and buys nothing now that storage is not tight. It also removes `PUSH_BCFG_SELF` from the
   instruction set entirely (see Encoding).
8. **Does the tune keep source text, or bytecode alone?** RESOLVED — source, always. The studio's
   copy of a tune is written by the studio, so it always has the text; the only way to meet a tune
   without it is to download the image from an ECU this install has never seen, and that path
   decompiles against the matched layout to produce the text anyway. The stored source therefore has
   exactly one job left: recompiling against a NEW layout when a tune migrates.
