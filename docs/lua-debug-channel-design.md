# Debug channel + Lua error tracking — design

Status: **design of record** (firmware half by Claude; studio half pending Gemini). Branch
`lua-debug-channel`.

## Problem

The comms protocol (binary msEnvelope frames; single-char commands `Q/A/r/w/b/E`; CRC32) has **no
debug/log channel** firmware→host. The Lua `ScriptEngine` *catches* every compile and runtime error
(`lua_tostring`, whose message embeds the line as `[string "..."]:LINE: msg`) but **stores nothing**
— it `fprintf(stderr, …)` and discards. So there is no error tracking today. We're building the
transport + storage now, shaped to serve a future **context-sensitive Lua editor** in studio.

Today's surface: `ScriptEngine` keeps `state` (0 ok / 1 disabled / 2 load-error / 3 runtime-error)
and `error_count` (u8) as members, neither published. The Lua API (`rpm`, `signalRead/Write`,
`evalTable`, `setFuelAdd`, `canSend`, `tickMs`, `ecu_print`, `onTick/onStart/onStop/onCanRx`) lives
in one C table (`ECU_API`) — the natural source for autocomplete if we export it.

## Architecture — three firmware layers + the studio client

### Layer A — cheap live status in telemetry  (low-risk, unblocks the studio indicator)
Add three signals to the catalog (`definition/ecu.schema.yaml` `signals:`), which codegen turns into
telemetry channels packed from the SignalBus (same path as `lua_gauge_1..8`):
- `lua_state` (u8, enum ok/disabled/load_error/runtime_error)
- `lua_error_count` (u8)
- `lua_error_line` (u16) — parsed from the Lua error message
`ScriptEngine` publishes them to the bus each update. Studio already polls `A`, so it gets
"script OK / error on line N" with **no new command** and near-zero latency. 4 bytes on the frame.

### Layer B — generic debug-record channel  (the rich path)
A new `D` command + a firmware **ring buffer** of debug records. Not Lua-only — `source` generalises
to trigger/sensor/system, so this is the project's debug pipe.

- **Decisions (locked):** ring depth **16**, message max **96 bytes** (~1.7 KB RAM). Studio **pulls**
  (no firmware push): it drains `D` when telemetry's `lua_error_count` bumps, plus a slow ~1 Hz
  background poll. `seq` lets studio detect dropped records.
- **Record:** `{ seq:u32, tick_ms:u32, source:u8, severity:u8, line:u16, msg_len:u16, msg:n }`.
  `source` {0 system, 1 lua_compile, 2 lua_runtime, 3 trigger, 4 sensor, 5 can};
  `severity` {0 debug, 1 info, 2 warn, 3 error}.
- **Wire:** request `[len]['D' + after_seq:u32][crc]` → reply flag `0x00` + `[count:u16]` then
  `count` records with `seq > after_seq` (capped to the frame block size). `after_seq = 0` = all held.
- **Firmware:** a `DebugLog` ring (single-producer from the engine task). `ScriptEngine` pushes a
  record on compile/runtime error (capturing `lua_tostring`, parsing the line, trimming the
  `[string..]:N:` prefix into `line` + clean `msg`); `ecu_print()` pushes an info record. Producers
  elsewhere (trigger/sensor) can push later. Oldest dropped when full; `seq` is monotonic.

### Layer C — export the Lua API into the meta  (autocomplete source of truth)
Codegen emits the `ECU_API` surface (function name + signature + one-line doc) into
`tuneit-meta.json` (a `lua_api` section). The editor's context-sensitive completion reads it, so it
**always matches the firmware** — never a hand-maintained, drifting list.

### Studio client  (Gemini — to be designed)
- **Editor widget:** syntax highlight + context-sensitive autocomplete fed by Layer C; inline error
  markers (gutter + squiggle) placed from `line`. (Widget choice — QScintilla vs hand-rolled
  QPlainTextEdit + QSyntaxHighlighter — Gemini's call; Claude leans QScintilla.)
- **Debug consumption:** an error/console panel; an `EcuLink` consumer of `D` (pull when telemetry
  `lua_error_count` bumps + slow poll); a status chip from `lua_state`.
- **Feedback loop:** edit → write `lua_source` (config write) → firmware live-reloads
  (`g_config_generation`) → `lua_state`+`lua_error_line` in telemetry (instant) + full message via
  `D` → editor highlights the line and shows the message. Near-real-time error feedback.

## The loop (why this shape)
Telemetry carries the *cheap, always-on* "is there an error and where"; the `D` channel carries the
*rich, on-demand* message + history without bloating the high-rate telemetry path. Layer C keeps
autocomplete honest. Each layer is independently useful and shippable.

## Sequencing
1. **Layer A — DONE** (`a1787e2`): `lua_state`/`lua_error_count`/`lua_error_line` telemetry +
   `ScriptEngine` publish + line capture. 38 host tests.
2. **Layer C — DONE** (`5b43200`): `lua_api` meta export + `validate_lua_api` (schema ↔ `ECU_API`).
3. **Layer B — DONE** (`16a5b86`): `DebugLog` 16×128 ring + `D` command + `ScriptEngine`/`ecu_print`
   producers + `ts_bench.get_debug`. Firmware cross-links clean.
4. **Studio — Gemini** (`lua-debug-studio`): editor (autocomplete from Layer C), Diagnostics dock
   (status chip from Layer A; ECU Console + Lua Errors from Layer B `D`), feedback loop.

The firmware half is complete; the `D` wire format above is the locked seam for the studio consumer.

## Notes / open
- Layer A changes the telemetry layout → `layout_hash` changes → studio meta regen + reflash
  (expected for any firmware field add).
- `D` records cap to the negotiated block size per reply; multi-record drain over several `D` calls.
- Bench: `tools/ts_bench.py` gains `get_debug(after_seq)` to exercise `D` headless.
