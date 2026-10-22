# Driving the studio from a script or an AI

:material-circle:{ .level-intermediate } Intermediate → :material-circle:{ .level-advanced } Advanced

> **In one sentence:** the studio has an AI bus that a script or an AI agent uses to read and write every
> setting and table and to watch the live channels, by name and in the units the studio shows — with the
> studio's window open, or with no screen at all (`studio --headless`).

## Overview

:material-circle:{ .level-intermediate } Intermediate

Everything you can see in the studio is also reachable without looking at it. A program talks to the
studio through the **AI bus**, a block of shared memory the studio serves while it runs. The program
sends a command ("what is the VE table?", "set the cylinder count to 6") and gets the answer back as
JSON. A small tool that ships with the studio, **jf-busctl**, sends the commands, so a shell script or
an AI agent needs nothing else installed.

There are two ways to run the studio for this:

| | Window | Headless |
|---|---|---|
| Start it with | `JF_AI_BUS=1 studio` | `studio --headless …` |
| Needs a screen | yes | **no** — no display, no GPU |
| The tune and the live data | whatever the window has open | the ECU or tune you name when starting it |
| Pages and widgets too | yes (`jf-busctl dump`, `act`) | no |

Both answer the same commands, so a script written against one works against the other.

The commands work on the studio's model of the tune, not on what is drawn. They reach every setting the
ECU's definition describes, including ones no page shows, and a command never depends on which page is
open or how a layout is arranged.
<!-- src: apps/studio-jf/src/app/AiCommands.h; apps/studio-jf/src/app/Headless.h; apps/studio-jf/main.cpp (--headless, JF_AI_BUS onAction) -->

## Concepts

:material-circle:{ .level-intermediate } Intermediate

### 1 · Paths

Every setting and table has a **path**, the same one the tune file uses: `engine.cylinder_count`,
`fuel_calculator.ve_table`, `sensors.sensor[clt].interface`. A setting that belongs to one sensor,
output or other numbered entry names it in brackets by its id (`clt`, `iat`), or by its number where it
has no id. Live channels have **names**: `rpm`, `map`, `clt`.

You rarely type a path from memory. `settings <filter>`, `tables <filter>` and `channels <filter>` list
everything whose path or label contains the filter, with its label beside it.

### 2 · Values and units

Values are in the units the studio shows — % for VE, °C, kPa, ms — never the ECU's raw storage. The
reply says which units in `"units"`. A setting with a list of choices is read back by its label and its
id, and written with either. A pin is written by its name (`AV3`). A channel selector is written by the
channel's name.

### 3 · Where a write goes

A write changes the tune the studio has open. When an ECU is connected it goes to the ECU straight
away, as an edit on a page would, into the ECU's working memory. **It is not permanent until you burn
it** (`burn`), exactly as in the window (chapter 36). The studio's own rules still apply:

- **Locked** in the window (the switch in the toolbar) refuses every write.
- A setting that cannot change while the engine turns is refused while it turns.
- A value outside its range is refused, and nothing is written.
- `table-set` checks the whole grid's size before writing any of it.

Every refusal answers `"ok": false` with the reason in `"error"`.
<!-- src: apps/studio-jf/src/app/AiCommands.cpp (writeSetting: readOnly, lockedWhileRunning, range; table-set shape check); apps/studio-jf/src/model/Cache.h (setConfigValue requests an ECU write) -->

## Procedure

:material-circle:{ .level-intermediate } Intermediate

### 1 · Find jf-busctl

| System | The tool |
|---|---|
| Linux, AppImage | `./jayecu-studio-<version>-x86_64.AppImage busctl …` (the AppImage runs it for you) |
| Windows | `"C:\Program Files\jayecu Studio\jf-busctl.exe" …` |
| Built from source | `apps/studio-jf/build/jf-busctl` |

In the examples below, `jf-busctl` stands for whichever of these you have. Run `studio --help` for the
studio's own options.

### 2 · Start the studio

**With no screen**, open an ECU from the studio's library (the ECUs **File ▸ Open ECU…** lists) by its
ECU id or by the name of its car:

```
studio --headless --ecu Hilux
studio --headless --ecu 004500353656500e20353641 --tune track
```

**Connected to the ECU**, with no screen:

```
studio --headless --connect                    # the only ECU plugged in
studio --headless --connect --port /dev/ttyACM0
studio --headless --connect --ecu 004500353656500e20353641   # that one of several
```

**A definition's defaults**, with no ECU at all: `studio --headless --meta jaytek_v1-0.4.10.meta`.

The headless studio prints one line of JSON when it is ready, and another when a connection completes:

```
{"event":"ready","bus":"/jframework_ai_bus","studio":"0.3.17","connecting":true}
{"event":"connected","port":"/dev/ttyACM0","board":"jaytek_v1","firmware":"0.4.10","ecu_id":"0045…"}
```

When several ECUs answer `--connect` and you did not say which, it lists them in an `"error"` line and
stops. It stops on Ctrl+C, on `SIGTERM`, or on `jf-busctl call quit`.

**With the window**, start the studio with the bus on: `JF_AI_BUS=1 studio` (on Windows, set
`JF_AI_BUS=1` in the environment first).
<!-- src: apps/studio-jf/src/app/Headless.cpp (--ecu by uid or car, --tune, --connect with scan, ready/connected lines, quit); apps/studio-jf/main.cpp (kUsage) -->

### 3 · Ask it something

```
jf-busctl call help
jf-busctl call status
```

`status` says whether an ECU is connected, which ECU and firmware, which tune is open, and how recently
telemetry arrived.

### 4 · The commands

| Command | What it does |
|---|---|
| `status` | link, ECU, firmware, tune, definition, telemetry age |
| `settings [filter]` | every setting: path, label, kind, units, value, range |
| `get <path>` | one setting in full: value, units, range, its options, its help text |
| `set <path> <value>` | write it, in the units shown; a choice by label or id |
| `tables [filter]` | every table, with its size — a sensor's calibration curve too, by the sensor's id (`sensors.sensor[map].cal`) |
| `table <path>` | the table: each axis's bins and channel, and the cells |
| `table-set <path> <json>` | write every cell: `[[row 0…],[row 1…]]` (a 3-D table: a list of those) |
| `cell <path> <col> <row> <value>` | write one cell (counting from 0) |
| `axis-set <path> x\|y\|z <json>` | write an axis's breakpoints, `[b0, b1, …]`, ascending, as many as it has live |
| `channels [filter]` | the live channels: name, label, units, rate |
| `live [name …]` | their values now; with no names, every channel |
| `burn` | make the ECU keep what it has (flash) |
| `save` | save the tune into the ECU's folder in the studio's library |
| `page <navigation path>` | show a page (window only), e.g. `Configuration/Engine Configuration` |
| `key on\|off\|auto` | force the ECU's key: `off` stops the engine (no fuel, no spark); `auto` hands it back |
| `connect` / `disconnect` | open or close the link, as the toolbar button does (the tune is saved first) |
| `autotune status\|start\|stop\|reset\|apply` | Auto Tune without its tab open: run it, pause it, start a new session, apply the proposal |
| `autotune continuous on\|off` | write to the map as it drives |
| `autotune grid [session\|change\|proposed\|base\|weight]` | the grid as the panel shows it: Session % by default, rows of columns |
| `cardlogs list\|import new\|import <name>…\|status` | the ECU's SD logs once the card is mounted; an import runs in the background, and `status` reports its progress |
| `dtc list\|refresh\|clear` | the trouble codes, as the DTC dock shows them, with freeze frames |
| `record start\|stop\|status` | the studio's own PC-side datalog |
| `quit` | stop (headless only) |

A table's cells are **rows of columns**: `cells[2][5]` is row 2, column 5. Row 0 is the first Y bin, and
column 0 is the first X bin, as the axes list them.

`jf-busctl` exits 0 when a command succeeded and 4 when it was refused (the reply says why), so a
script can test it the ordinary way. An action of `-` is read from standard input, for a table too
large for a command line.
`help` lists every command this studio answers, including the ones the window adds.
<!-- src: apps/studio-jf/src/app/AiCommands.cpp (kHelp, handle, Host::verbs); apps/studio-jf/main.cpp (aiHost.verbs); JFramework tools/jf-busctl.cpp (exit codes, '-' from stdin) -->

### 5 · With the window: the pages themselves

The window also publishes every visible control. `jf-busctl dump` lists them (id, kind, caption,
value; its first line says how many, and `dropped=` how many did not fit, normally 0), and `jf-busctl act <id or caption> click` presses one, `type:12.5` types into it, `key:Return`
presses a key, and `dialog:ok` answers the dialog in front. This drives the studio the way a person
does, which is what testing a page needs. For reading and writing the tune, the commands above are
quicker and do not depend on the layout.
<!-- src: JFramework include/j/core/JAiBus.h (click, type:, key:, dialog:ok); JFramework docs/architecture/02_AI_CONTROL_BUS.md -->

## Examples

:material-circle:{ .level-advanced } Advanced

!!! example "Read the VE table and one cell of it"
    ```
    jf-busctl call 'table fuel_calculator.ve_table' > ve.json
    python3 -c "import json; t=json.load(open('ve.json')); print(t['y']['bins'][4], t['x']['bins'][9], t['cells'][4][9])"
    ```
    prints the load bin, the RPM bin and the VE at row 4, column 9.

!!! example "Watch the engine warm up"
    ```
    while true; do jf-busctl call 'live clt rpm afr'; sleep 1; done
    ```

!!! example "Write a whole table from a file"
    ```
    { printf 'table-set fuel_calculator.ve_table '; cat new-ve.json; } | jf-busctl call -
    jf-busctl call burn
    ```
    `new-ve.json` holds the cells only, rows of columns, the same size as the table.

!!! example "From Python"
    ```python
    import json, subprocess
    def studio(cmd):
        out = subprocess.run(["jf-busctl", "call", cmd], capture_output=True, text=True).stdout
        return json.loads(out)
    print(studio("get engine.cylinder_count")["value"])
    ```

## Pitfalls

:material-circle:{ .level-intermediate } Intermediate

- **One studio per session has the bus.** A second studio started with the bus replaces the first's.
- **Writes are in RAM until `burn`.** Exactly as on a page: unplug the ECU without burning and the
  change is gone.
- **Headless `save` writes the tune into the ECU's folder**, under the tune name it opened (or the one
  given with `--tune`), replacing that file. Connected headless, the tune it holds is the one read from
  the ECU.
- **A headless connect needs the ECU's definition.** It is found in the studio's meta library or its
  firmware kits. An ECU whose firmware this studio has never seen is refused with that message: open it
  once in the window (which can fetch the definition from the ECU's SD card or the release), or pass
  `--meta <file>`.
- **The headless studio speaks only to jayecu firmware.** A TunerStudio-protocol ECU is connected from
  the window.
- **`jf-busctl` says "no bus"** when no studio is running with the bus on, and "stale bus" when the
  studio that made it has crashed.
<!-- src: apps/studio-jf/src/app/Headless.cpp (save under st.tune; loadMeta message; native link only); JFramework tools/jf-busctl.cpp (no bus, stale bus) -->

## Related

- Chapter 36, Burning a tune — what `burn` does.
- Chapter 48, Tunes, layouts and backups — the library `--ecu` opens from, and where `save` writes.
- Chapter 49, Customising the studio — the pages and controls `dump` and `act` see.
