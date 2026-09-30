#!/usr/bin/env python3
"""
JayECU Board Codegen
Reads a definition/boards/<board>.board.yaml and writes a C++ constants header.

Usage:
  python3 codegen/board_codegen.py <input.board.yaml> <output_header.h>

Example:
  python3 codegen/board_codegen.py definition/boards/jaytek_v1.board.yaml \
      generated/boards/jaytek_v1_board.h
"""

import re
import sys
import yaml
from pathlib import Path


def write_if_changed(path: Path, text: str) -> None:
    """Only when the text changed: an identical header rewritten still recompiles everything including it."""
    if path.exists() and path.read_text() == text:
        return
    path.write_text(text)

BANNER = "// AUTO-GENERATED — do not edit. Run codegen/board_codegen.py to regenerate.\n"


def upper_snake(s: str) -> str:
    return s.upper().replace("-", "_").replace(" ", "_")


def _port_bit(gpio: str):
    """'PE2' -> ('GPIOE', 2)."""
    m = re.match(r"^P([A-K])(\d{1,2})$", gpio)
    if not m:
        raise ValueError(f"board schema: pin gpio '{gpio}' is not a Pxn name")
    return f"GPIO{m.group(1)}", int(m.group(2))


def pins_with(board: dict, cap: str):
    """Signals of every pin whose `caps` set includes `cap`, in pins[] declaration
    order. This is how role pools derive: a pin shows up under every role it can do
    (e.g. a DIG pin is both TRIGGER_INPUT and DIGITAL_INPUT), and the ordered list
    is the firmware index for that role."""
    return [p["signal"] for p in board["pins"] if cap in p.get("caps", [])]


def emit_header(board: dict, out_path: Path) -> None:
    name     = board["board"]
    guard    = f"JAYECU_GENERATED_{upper_snake(name)}_BOARD_H"
    knock    = pins_with(board, "KNOCK_INPUT")
    leds     = pins_with(board, "STATUS_LED")
    pg       = pins_with(board, "POWER_GOOD")
    led_roles = {sg: i for i, sg in enumerate(leds)}
    _missing = [r for r in ("RUNNING", "WARNING", "ERROR", "COMMS") if r not in led_roles]
    if _missing:
        raise ValueError(
            f"board schema: STATUS_LED pins must cover every platform LED role; missing {_missing}. "
            f"Declared: {leds}. (A board with fewer lamps points two roles at one pin.)")
    vref_mv  = int(board["adc_vref_mv"])
    vref_v   = vref_mv / 1000.0

    # Analog front-end full-scale (MCP6004 gain backed out). Defaults to vref_mv
    # for boards with no front-end (direct ADC connection).
    av_fs_mv = int(board.get("analog_av_fullscale_mv", vref_mv))
    at_fs_mv = int(board.get("analog_at_fullscale_mv", vref_mv))

    # ADC resolution is per-board — NOT every board is a 12-bit / 4095-count part.
    # `adc_bits` (default 12) drives the top count = 2^bits - 1.
    adc_bits     = int(board.get("adc_bits", 12))
    adc_full_cnt = (1 << adc_bits) - 1

    # Counts derive from pin caps (no hand-maintained scalars, no pools).
    res = board["resources"]
    ls_count  = len(pins_with(board, "LOWSIDE_OUT"))
    ign_count = len(pins_with(board, "IGNITION_OUT"))
    hs_count  = len(pins_with(board, "HIGHSIDE_OUT"))
    hbridge_count = len(res["hbridge"])
    av_count  = len(pins_with(board, "ANALOG_VOLTAGE"))
    at_count  = len(pins_with(board, "ANALOG_TEMP"))
    din_count = len(pins_with(board, "DIGITAL_INPUT"))

    # Battery = the ANALOG_VOLTAGE pin carrying a `divider` (a fixed input); its
    # index into the analog-voltage list + the divider feed the platform layer.
    by_sig = {p["signal"]: p for p in board["pins"]}
    batt_index, batt_divider = 0, 1.0
    for i, s in enumerate(pins_with(board, "ANALOG_VOLTAGE")):
        if "divider" in by_sig.get(s, {}):
            batt_index, batt_divider = i, float(by_sig[s]["divider"])
            break

    lines = [
        BANNER,
        f"#pragma once",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        "#include <cstdint>",
        "",
        "// ---------------------------------------------------------------------------",
        f"// Board: {name}",
        f"// MCU:   {board.get('mcu', 'unknown')}",
        f"// {board.get('description', '')}",
        "// ---------------------------------------------------------------------------",
        "",
        f'static constexpr const char* BOARD_NAME            = "{name}";',
        "",
        "// WHICH BOARD generated/ currently holds. generated/ is single-board state -- codegen",
        "// overwrites it for whatever BOARD it was last run with -- so a board's own sources check",
        "// this and refuse to compile against another board's constants. Without it the failure is",
        "// a pile of redefinition errors, or worse a silent build against the wrong pin counts.",
        f"#define JAYECU_BOARD_IS_{upper_snake(name)} 1",
        "",
        "// Clock",
        f"static constexpr uint32_t    BOARD_HSE_HZ           = {board['hse_hz']}u;",
        f"static constexpr uint32_t    BOARD_LSE_HZ           = {board['lse_hz']}u;",
        "",
        "// ADC reference",
        f"static constexpr uint32_t    BOARD_ADC_VREF_MV      = {vref_mv}u;",
        f"static constexpr float       BOARD_ADC_VREF_V       = {vref_v:.3f}f;",
        f"static constexpr uint8_t     BOARD_ADC_BITS         = {adc_bits}u;",
        f"static constexpr uint16_t    BOARD_ADC_FULL_SCALE   = {adc_full_cnt}u;",
        "",
        "// Analog front-end full-scale at the connector (MCP6004 gain removed)",
        f"static constexpr uint32_t    BOARD_AV_FULLSCALE_MV  = {av_fs_mv}u;",
        f"static constexpr uint32_t    BOARD_AT_FULLSCALE_MV  = {at_fs_mv}u;",
        "",
        "// Output counts (derived from resources.output_pool / resources.hbridge)",
        f"static constexpr uint8_t     BOARD_LOW_SIDE_COUNT   = {ls_count}u;",
        f"static constexpr uint8_t     BOARD_IGNITION_COUNT   = {ign_count}u;",
        f"static constexpr uint8_t     BOARD_HIGH_SIDE_COUNT  = {hs_count}u;",
        "",
        "// Hardware cylinder ceiling = the ignition-output count (every cylinder needs a spark channel).",
        "// The firmware's MAX_CYLINDERS and codegen's per-cylinder sensor generation both key off this,",
        "// so the cylinder limit is a HARDWARE fact (from the board's IGN pins), not a hardcoded constant.",
        f"static constexpr uint8_t     BOARD_MAX_CYLINDERS    = {ign_count}u;",
        f"static constexpr uint8_t     BOARD_HBRIDGE_COUNT        = {hbridge_count}u;",
        "",
        "// User-configurable input counts (derived from resources.analog_pool / capture_pool)",
        f"static constexpr uint8_t     BOARD_ANALOG_V_COUNT   = {av_count}u;",
        f"static constexpr uint8_t     BOARD_ANALOG_T_COUNT   = {at_count}u;",
        f"static constexpr uint8_t     BOARD_DIGITAL_IN_COUNT = {din_count}u;",
        "",
        "// Status-LED pool INDEX for each platform role, taken from the pin's signal NAME. The",
        "// platform LED API is four roles; which pin carries which is the board's business, and so",
        "// is the declaration ORDER (jaytek declares RUNNING first, proteus ERROR first). Calling",
        "// board_set_led(0) for 'running' hardcoded one board's order and would have lit the wrong",
        "// lamp on the next one.",
        *[f"static constexpr uint8_t     BOARD_LED_{r:<8} = {led_roles[r]}u;"
          for r in ("RUNNING", "WARNING", "ERROR", "COMMS")],
        f"static constexpr uint8_t     BOARD_LED_COUNT    = {len(leds)}u;",
        f"static constexpr uint8_t     BOARD_PG_COUNT     = {len(pg)}u;",
        f"static constexpr uint8_t     BOARD_KNOCK_COUNT  = {len(knock)}u;",
        "",
        "// The ANALOG POOL LAYOUT — the one contract the firmware and the studio must agree on.",
        "// platform_read_ain_raw(pin) takes a single pool index: [0 .. AT_BASE) are the voltage",
        "// inputs, [AT_BASE ..] the temperature inputs. It is the AV count by construction, but it",
        "// is named because it is a CONTRACT, not an accident: codegen builds the studio's pin",
        "// dropdowns and the raw telemetry against the same derivation. It was a literal 16 in four",
        "// places, which agreed only for a board with 16 voltage inputs.",
        f"static constexpr uint8_t     BOARD_ANALOG_T_BASE    = {av_count}u;",
        "",
        "// Fixed input: battery voltage (AV12, not user-assignable)",
        f"static constexpr uint8_t     BOARD_BATTERY_AIN_INDEX = {batt_index}u;",
        f"static constexpr float       BOARD_BATTERY_DIVIDER   = {batt_divider:.4f}f;",
        "",
        f"#endif // {guard}",
        "",
    ]

    out_path.parent.mkdir(parents=True, exist_ok=True)
    write_if_changed(out_path, "\n".join(lines))
    print(f"  wrote {out_path}")


def emit_board_shim(board: dict, out_path: Path) -> None:
    """Emit generated/boards/board.h — the BOARD-NEUTRAL include.

    Shared firmware headers (Pipeline/Stages.h, Scheduler/SchedulerTypes.h) and the SoC tier
    (Platform/stm32f7xx/) need the board's derived constants, but must not name a board: a
    literal `#include "jaytek_v1_board.h"` in the MCU tier is the board tier reaching upward,
    and it silently pins the whole firmware to one board. They include this instead, and the
    active board is whatever codegen last ran for — i.e. the BOARD make variable, one decision
    in one place. See docs/modular-platform-architecture.md, "Layer -1 — the SoC seam".
    """
    name = board["board"]
    lines = [
        "// AUTO-GENERATED — do not edit. Run codegen/board_codegen.py to regenerate.",
        "",
        "#pragma once",
        "#ifndef JAYECU_GENERATED_BOARD_H",
        "#define JAYECU_GENERATED_BOARD_H",
        "",
        "// The board this firmware is being built for. Selected by the BOARD make/CMake",
        "// variable; nothing in the firmware may include a <board>_board.h directly.",
        f'#include "{name}_board.h"',
        "",
        "#endif // JAYECU_GENERATED_BOARD_H",
        "",
    ]
    out_path.parent.mkdir(parents=True, exist_ok=True)
    write_if_changed(out_path, "\n".join(lines))
    print(f"  wrote {out_path}")


def emit_pins_header(board: dict, out_path: Path) -> None:
    """Emit the firmware pin/port/ADC map from `pins:`/`resources:`.

    The firmware keeps the driver logic (how to toggle a GPIO, run the ADC); the
    *map* (which signal -> which port/bit/channel) lives only here, derived from
    the board schema. board_hal + the board profile include this header instead
    of hand-written pin tables, so the schema is the single source of truth.
    """
    name  = board["board"]
    guard = f"JAYECU_GENERATED_{upper_snake(name)}_PINS_H"
    res   = board["resources"]
    by_sig = {p["signal"]: p for p in board["pins"]}

    def need(sig):
        if sig not in by_sig:
            raise ValueError(f"board schema: signal '{sig}' referenced but not in pins:")
        return by_sig[sig]

    def gpio_rows(sigs):
        rows = []
        for s in sigs:
            port, bit = _port_bit(need(s)["gpio"])
            rows.append(f"    {{{port}, GPIO_PIN_{bit}, {bit}u}},  // {s}")
        return rows

    def adc_rows(sigs):
        rows = []
        for s in sigs:
            p = need(s)
            unit = int(re.sub(r"\D", "", str(p["adc"]["unit"])))   # ADC1 -> 1
            rows.append(f"    {{{unit}u, ADC_CHANNEL_{int(p['adc']['channel'])}}},  // {s}")
        return rows

    def chan_rows(sigs, cls):
        rows = []
        for s in sigs:
            port, bit = _port_bit(need(s)["gpio"])
            rows.append(f"    {cls}{{{port}, {bit}u}},  // {s}")
        return rows

    # Role pools derive from pin caps (declaration order = firmware index).
    ign = pins_with(board, "IGNITION_OUT")
    ls  = pins_with(board, "LOWSIDE_OUT")
    hs  = pins_with(board, "HIGHSIDE_OUT")
    cap = pins_with(board, "TRIGGER_INPUT")              # EXTI capture pool
    compare = ign + ls                                   # firmware compare pool order
    av  = pins_with(board, "ANALOG_VOLTAGE")
    at  = pins_with(board, "ANALOG_TEMP")
    knock = pins_with(board, "KNOCK_INPUT")
    dig   = pins_with(board, "DIGITAL_INPUT")
    leds  = pins_with(board, "STATUS_LED")
    pg    = pins_with(board, "POWER_GOOD")
    hbridge   = res["hbridge"]

    def cap_flag(sig):
        return ("PinCapabilityFlags::EXTI_CAPABLE" if "exti" in need(sig)
                else "PinCapabilityFlags::TIMER_CAPTURE")

    L = [BANNER.rstrip("\n"), "#pragma once",
         f"#ifndef {guard}", f"#define {guard}", "",
         "#include <cstdint>",
         '#include "stm32f7xx_hal.h"',
         "",
         "// ---------------------------------------------------------------------------",
         f"// Board pin/port/ADC map for '{name}', generated from definition/boards/{name}.board.yaml.",
         "// The schema owns WHAT-connects-where; firmware keeps only the driver HOW.",
         "// ---------------------------------------------------------------------------",
         "",
         "struct BoardGpioPin { GPIO_TypeDef* port; uint16_t pin; uint8_t bit; };",
         "struct BoardAdcChan { uint8_t adc_unit; uint32_t channel; };  // unit 1 or 3",
         "// A capture-capable input: the GPIO plus the EXTI line the schema DECLARES for it. The line",
         "// is carried rather than computed because `first_bit + index` only holds for a pool that is",
         "// contiguous on one port -- true of PD8-PD15, false of the next board.",
         "struct BoardExtiPin { GPIO_TypeDef* port; uint16_t pin; uint8_t bit; uint8_t line; };",
         ""]

    def block(decl, rows):
        # An EMPTY pool is a legitimate board fact -- proteus has no power-good sense pins at all --
        # but `T arr[0] = {}` is ill-formed ISO C++ (GCC allows it as an extension, and only
        # -Wpedantic says so). Emit a single zeroed row instead and leave the matching BOARD_*_COUNT
        # at 0, so every bounds check rejects every index and nothing can reach the placeholder.
        if not rows:
            decl = decl.replace("[0]", "[1]")
            rows = ["    {},  // (pool is empty on this board; the COUNT is 0 and gates every access)"]
        return [f"[[maybe_unused]] static const {decl} = {{", *rows, "};", ""]

    L += block(f"BoardGpioPin BOARD_IGN_PINS[{len(ign)}]", gpio_rows(ign))
    L += block(f"BoardGpioPin BOARD_LS_PINS[{len(ls)}]",   gpio_rows(ls))
    L += block(f"BoardGpioPin BOARD_HS_PINS[{len(hs)}]",   gpio_rows(hs))
    L += block(f"BoardGpioPin BOARD_LED_PINS[{len(leds)}]", gpio_rows(leds))
    L += block(f"BoardGpioPin BOARD_PG_PINS[{len(pg)}]",   gpio_rows(pg))
    L += block(f"BoardGpioPin BOARD_HBRIDGE_DIS_PINS[{len(hbridge)}]", gpio_rows([e["dis"] for e in hbridge]))
    L += block(f"BoardGpioPin BOARD_HBRIDGE_DIR_PINS[{len(hbridge)}]", gpio_rows([e["dir"] for e in hbridge]))
    L += block(f"BoardGpioPin BOARD_HBRIDGE_PWM_PINS[{len(hbridge)}]", gpio_rows([e["pwm"] for e in hbridge]))
    L += block(f"BoardAdcChan BOARD_AV_CHANS[{len(av)}]", adc_rows(av))
    L += block(f"BoardAdcChan BOARD_AT_CHANS[{len(at)}]", adc_rows(at))
    L += block(f"BoardAdcChan BOARD_KNOCK_CHANS[{len(knock)}]", adc_rows(knock))

    # The digital-input pool, carrying each pin's DECLARED EXTI line.
    def exti_rows(sigs):
        rows = []
        for sg in sigs:
            pin_def = need(sg)
            port, bit = _port_bit(pin_def["gpio"])
            if "exti" not in pin_def:
                raise ValueError(f"board schema: {sg} has DIGITAL_INPUT but no exti: {{line}}")
            line = int(pin_def["exti"]["line"])
            # On STM32 an EXTI line is not a free choice: line N is driven by bit N of whichever
            # PORT the SYSCFG_EXTICR mux selects, so the line ALWAYS equals the pin's bit number.
            # A schema that declares otherwise produces a pin whose interrupt simply never fires,
            # and nothing downstream can tell that from a quiet sensor. Catch it at generate time.
            if line != bit:
                raise ValueError(
                    f"board schema: {sg} is {pin_def['gpio']} (bit {bit}) but declares "
                    f"exti line {line}. On STM32 the EXTI line IS the pin bit number; "
                    f"the port is what SYSCFG_EXTICR selects.")
            rows.append(f"    {{{port}, GPIO_PIN_{bit}, {bit}u, {line}u}},  // {sg}")
        return rows
    L += block(f"BoardExtiPin BOARD_DIG_PINS[{len(dig)}]", exti_rows(dig))


    L += [f"static constexpr uint8_t BOARD_IGN_COMPARE_COUNT = {len(ign)}u;", ""]

    # Concrete capture/compare channel objects + resource tables for the board
    # profile. Guarded: the includer must pull in Stm32Capture.h / Stm32OutputCompare.h
    # / BoardProfile.h first (board_hal includes this header WITHOUT the macro and
    # only takes the data arrays above).
    L += ["#ifdef BOARD_PINS_EMIT_CHANNELS",
          f"static Stm32CaptureChannel s_board_caps[{len(cap)}] = {{",
          *chan_rows(cap, ""), "};", "",
          f"static Stm32CompareChannel s_board_cmps[{len(compare)}] = {{",
          *chan_rows(compare, ""), "};", ""]
    cres = [f"    {{&s_board_caps[{i}], {cap_flag(s)}, \"{s}\"}}," for i, s in enumerate(cap)]
    L += [f"static const CaptureResource s_board_capture_resources[{len(cap)}] = {{",
          *cres, "};", ""]
    ores = [f"    {{&s_board_cmps[{i}], PinCapabilityFlags::TIMER_COMPARE, \"{s}\"}},"
            for i, s in enumerate(compare)]
    L += [f"static const CompareResource s_board_compare_resources[{len(compare)}] = {{",
          *ores, "};",
          "#endif // BOARD_PINS_EMIT_CHANNELS", ""]

    L += [f"#endif // {guard}", ""]

    write_if_changed(out_path, "\n".join(L))
    print(f"  wrote {out_path}")


def main() -> None:
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)

    in_path  = Path(sys.argv[1])
    out_path = Path(sys.argv[2])

    if not in_path.exists():
        print(f"Error: {in_path} not found", file=sys.stderr)
        sys.exit(1)

    board = yaml.safe_load(in_path.read_text())
    emit_header(board, out_path)
    # Pins header lives next to the constants header: <board>_pins.h
    emit_pins_header(board, out_path.parent / f"{board['board']}_pins.h")
    # ...and the board-neutral shim the shared/SoC headers include instead of either.
    emit_board_shim(board, out_path.parent / "board.h")


if __name__ == "__main__":
    main()
