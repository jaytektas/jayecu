#!/usr/bin/env python3
"""
JayECU Codegen
Reads schema/ecu.schema.yaml (incl. its `signals:` catalog) and writes all generated artifacts.

Outputs:
  generated/signal_ids.h                  — SignalId enum + SIGNAL_NAMES[]
  generated/modules/<module>_config.h
  generated/ecu_telemetry.h
  generated/ecu_config.h
  generated/schema_meta.h
  generated/default_config.cpp
  shared/ecu.ini
  shared/ecu.json
  shared/tuneit-meta.json                 — TuneIt (Java) resolved Data Dictionary
"""

import yaml
import json
import struct
import base64
import sys
import re
import math
import zlib
import hashlib
from pathlib import Path


def write_if_changed(path, text):
    """Write a generated file only when its text changed. Rewriting an identical header still bumps its
    timestamp, and every object that includes it recompiles — so each firmware build was a full one
    (~70 objects) even when nothing in the schema had moved."""
    path = Path(path)
    if path.exists() and path.read_text() == text:
        return
    path.write_text(text)

# The on-flash config identity is a CONTENT HASH of the byte-layout contract, not a
# hand-bumped integer — see compute_layout_hash(). It is field 0 of EcuConfig (and of
# the default-tune image) and the firmware's boot gate accepts a stored tune only if its
# stored hash equals the running firmware's JAYECU_LAYOUT_HASH. Because it is derived
# from the layout itself it can never be forgotten on a schema change, and (being a pure
# function of offsets/sizes/types, not git/build metadata) it is stable across rebuilds
# that don't move a config byte — so the firmware still accepts its own saved tune after
# a no-op rebuild. The same value is the meta-match key in shared/tuneit-meta.json.
REPO_ROOT    = Path(__file__).resolve().parent.parent
SCHEMA_FILE  = REPO_ROOT / "definition" / "ecu.schema.yaml"
GEN_DIR      = REPO_ROOT / "generated"
SHARED_DIR   = REPO_ROOT / "shared"
# The editor-authored UI layout (dialogs + menu) lives in the schema's top-level
# `layout:` key, edited in the studio. If absent, codegen emits no [UiDialogs]/[Menu];
# it never auto-synthesizes per-module dialogs.

BANNER = "// AUTO-GENERATED — do not edit. Run codegen/codegen.py to regenerate.\n"

# The integer wire types a telemetry channel may use, and the span each can hold. One definition —
# the range guard and the sensor-signal resolver both index it, so "what fits" is never two answers.
WIRE_MIN = {"uint8": 0.0, "int8": -128.0, "uint16": 0.0, "int16": -32768.0,
            "uint32": 0.0, "int32": -2147483648.0}
WIRE_MAX = {"uint8": 255.0, "int8": 127.0, "uint16": 65535.0, "int16": 32767.0,
            "uint32": 4294967295.0, "int32": 2147483647.0}
WIRE_ORDER = ("uint8", "int8", "uint16", "int16", "uint32", "int32")   # narrowest first

def narrowest_wire(mn: float, mx: float, scale: float) -> str:
    """The smallest wire type that holds mn..mx as counts of `scale`. The wire is not a free
    choice: given a range and a precision, anything narrower truncates and anything wider is
    dead bandwidth — so it is computed, not declared."""
    lo, hi = mn / scale, mx / scale
    for t in WIRE_ORDER:
        if lo >= WIRE_MIN[t] and hi <= WIRE_MAX[t]:
            return t
    raise SystemExit(f"codegen: no wire type holds {mn}..{mx} at scale {scale}")

NO_TYPE = "generic"   # `type: generic` on a sensor row = it has NO type at build time.
                      # Not a type itself: there is no `generic` entry in sensor_types, because a
                      # type is units+precision+domain and this has none of the three. The tune
                      # supplies a real type at runtime; until it does the input is unconfigured.

def assignable_types(schema):
    """The types a runtime-typed input may be given. An unconfigured input is read as an analog
    voltage through a cal curve, so it can be any type built that same way — derived from each
    type's own declaration, never a hand-kept list."""
    return [t for t in (schema.get("sensor_types") or []) if t.get("raw") == "volts"]

def untyped_channel_wire(schema):
    """(units, scale, min, max) for a channel whose sensor has no compile-time type.

    Not a type — a WIRE-SIZING rule. The telemetry wire is fixed at build time while the type is
    chosen in the tune, so the wire must hold whatever type is later assigned: widest range, finest
    scale, across every assignable type. Until a type is assigned the input is simply unconfigured
    and publishes nothing at all, so these numbers describe capacity, not a reading."""
    pool = assignable_types(schema)
    return ("",
            10.0 ** -max(int(t.get("decimals", 2)) for t in pool),
            min(float(t["min"]) for t in pool),
            max(float(t["max"]) for t in pool))

def sensor_diagnostics(schema, sen):
    """Which checks this sensor can raise a code for. A runtime-typed input does not know its type
    at build time but its P-codes are allocated then, so it gets the union over every type it could
    be assigned — otherwise configuring one as a pressure sensor would find its codes missing."""
    types = {t["id"]: t for t in (schema.get("sensor_types") or [])}
    if sen.get("type") != NO_TYPE:
        return (types.get(sen.get("type"), {}) or {}).get("diagnostics") or []
    seen = []
    for t in assignable_types(schema):
        for c in (t.get("diagnostics") or []):
            if c not in seen:
                seen.append(c)
    return seen

def resolve_sensor_signals(schema):
    """Fill every sensor-backed signal's units/scale/min/max/telem_type from its sensor's TYPE.

    The sensor type owns units, precision and range, so a signal a sensor produces declares none
    of them. Before this they were hand-copied onto each signal: 88 of the 93 were byte-identical
    to the type they came from, and `battery` had already drifted (type 0..32 V, signal 0..30 V)
    with nothing comparing the two. Declaring any of them on a sensor-backed signal is now an
    ERROR, so a channel's range can never have a second answer.

    provides[0] is the sensor's own value, typed by the sensor. provides[1+k] is the k-th
    secondary output its type declares, which names its own type in `role`.
    """
    types = {t["id"]: t for t in schema.get("sensor_types", []) or []}
    by_id = {s["id"]: s for s in schema["signals"]}
    OWNED = ("units", "scale", "min", "max", "telem_type")
    rt_wire = untyped_channel_wire(schema)
    bad = []
    for sen in schema.get("sensors", []) or []:
        untyped = sen.get("type") == NO_TYPE
        st = None if untyped else types.get(sen.get("type"))
        if not untyped and not st:
            bad.append(f"sensor '{sen['id']}' has unknown type '{sen.get('type')}'")
            continue
        outs = (st or {}).get("outputs") or []
        for k, ch in enumerate(sen.get("provides") or [sen["id"]]):
            sig = by_id.get(ch)
            if sig is None:
                continue                       # the catalog-reference check above already reported it
            if untyped:
                ct = None                      # no type until the tune assigns one
            elif k == 0:
                ct = st                        # the sensor's own value
            elif k - 1 < len(outs):
                ct = types.get((outs[k - 1] or {}).get("role"))   # a secondary output, typed by `role`
            else:
                ct = None
            if ct is None and not untyped:
                bad.append(f"sensor '{sen['id']}' provides '{ch}' with no sensor_type behind it")
                continue
            declared = [f for f in OWNED if f in sig]
            if declared:
                owner = "its type is chosen in the tune" if untyped else f"its sensor is type '{ct['id']}'"
                bad.append(f"signal '{ch}' declares {', '.join(declared)} — {owner}, which owns "
                           f"those. A narrower DISPLAY range goes in gauge: {{min, max}}.")
                continue
            if untyped:                                       # capacity for any assignable type
                units, scale, lo, hi = rt_wire
            else:
                units = ct.get("units", "")
                scale = 10.0 ** -int(ct.get("decimals", 2))
                lo, hi = float(ct["min"]), float(ct["max"])   # the type's DOMAIN
            sig["units"] = units
            sig["scale"] = scale
            # The wire must carry the whole domain: it is sized from the type, never from a gauge's
            # display range, so a channel can always transport any value its type permits.
            sig["telem_type"] = narrowest_wire(lo, hi, scale)
            # min/max as published to the client is the DISPLAY range: the domain unless a gauge
            # deliberately narrows it (a MAP gauge reads 0..400 kPa though pressure spans 0..3000).
            g = sig.get("gauge") or {}
            sig["min"] = float(g.get("min", lo))
            sig["max"] = float(g.get("max", hi))
    if bad:
        sys.exit("ERROR: sensor-backed signals (the sensor TYPE owns units/scale/min/max/wire):\n  "
                 + "\n  ".join(bad))

# Tuning-data block size: the largest table/write DATA chunk accepted in one transfer. Single
# source of truth — flows to schema_meta.h (JAYECU_BLOCK_SIZE, the firmware's inbound assembly
# buffer) and the meta protocol block, so they can never drift apart.
BLOCK_SIZE = 1024

# -----------------------------------------------------------------------
# Wire protocol — THE single source of truth. Emitted into BOTH the firmware
# (generated/protocol.h: command-code constants CommsManager switches on) AND the
# app-facing meta (tuneit-meta.json "protocol"), so the firmware and the studio's
# comms client can never disagree about the wire. Only the commands we actually
# need.
# -----------------------------------------------------------------------
PROTOCOL = {
    "framing": {
        # Omnidyno sync-framed wire format, both directions:
        #   [0xAA 0x55][typeId:1][rsv:1][length:u16 LE][timestamp:u64 LE][sequence:u16 LE][payload][crc16:2 LE]
        # length = TOTAL frame bytes (16-byte header + payload + 2-byte CRC). crc16 = CRC16-CCITT
        # (poly 0x1021, init 0xFFFF) over header+payload. typeId carries the command / response code.
        "sync":         [0xAA, 0x55],
        "header_bytes": 16,
        "length":       {"bytes": 2, "endian": "little", "counts": "total"},
        "crc":          {"algo": "crc16-ccitt", "poly": 0x1021, "init": 0xFFFF, "bytes": 2, "endian": "little"},
        "request":      "typeId header + payload",
        "response":     "typeId header + payload",
        "field_endian": "little",      # multi-byte fields inside config/telemetry payloads
        "block_size":   BLOCK_SIZE,    # max config read/write data chunk per frame
    },
    "commands": {
        "identity": {
            "code": "Q", "reply": "ascii",
            "desc": "Identity: 'jayecu <board> <version> <build> <layout_hash>'; "
                    "the last token is the exact meta-match key.",
        },
        "telemetry": {
            "code": "A", "reply": "telemetry_frame",
            "desc": "The whole packed telemetry frame (decode via the telemetry descriptor).",
        },
        "config_read": {
            "code": "r", "request": "u8 selector=0, u32 offset, u16 size",
            "reply": "raw config bytes [offset, offset+size)",
            "desc": "Read config — flat 32-bit absolute offset, no pages.",
        },
        "config_write": {
            "code": "w", "request": "u8 selector=0, u32 offset, u16 size, bytes data",
            "reply": "ack",
            "desc": "Write config — flat 32-bit absolute offset, no pages.",
        },
        "burn": {
            "code": "b", "reply": "ack",
            "desc": "Persist the RAM config to flash.",
        },
        "cli": {
            "code": "E", "request": "ascii command line", "reply": "ascii output",
            "desc": "Text console escape hatch for the long tail (RTC, debug, SD, …).",
        },
        "debug": {
            "code": "D", "request": "u32 after_seq",
            "reply": "u16 count, then records {u32 seq, u32 tick_ms, u8 source, u8 severity, "
                     "u16 line, u16 msg_len, bytes msg}",
            "desc": "Pull debug/log records with seq > after_seq, oldest first (Lua errors, "
                    "ecu_print, …). after_seq=0 returns all held; the count is capped to the block size.",
        },
        "dtc_read": {
            "code": "G", "reply": "the DtcManager image: [u32 magic][u16 ver][u16 boot_id]"
                                  "[u16 n], then n DtcRecord structs (Dtc.h packed layout)",
            "desc": "Read the full DTC table (active + stored). Same serialized image the SD "
                    "persistence uses; the studio parses DtcRecord[] to list every code.",
        },
    },
    # Field-type tags used by config/telemetry fields. Codes match the studio's decoder
    # (omnidyno DataType: U8=0 S8=1 U16=2 S16=3 U32=4 S32=5 F32=6 ASCII=8).
    "field_types": {
        "U08":   {"code": 0, "size": 1, "signed": False, "float": False},
        "S08":   {"code": 1, "size": 1, "signed": True,  "float": False},
        "U16":   {"code": 2, "size": 2, "signed": False, "float": False},
        "S16":   {"code": 3, "size": 2, "signed": True,  "float": False},
        "U32":   {"code": 4, "size": 4, "signed": False, "float": False},
        "S32":   {"code": 5, "size": 4, "signed": True,  "float": False},
        "F32":   {"code": 6, "size": 4, "signed": True,  "float": True},
        "ASCII": {"code": 8, "size": 0, "signed": False, "float": False},
        "EXPR":  {"code": 9, "size": 0, "signed": False, "float": False},
    },
}

# Stable C macro name per command key.
_CMD_MACRO = {
    "identity": "IDENTITY", "telemetry": "TELEMETRY", "config_read": "CONFIG_READ",
    "config_write": "CONFIG_WRITE", "burn": "BURN", "cli": "CLI", "debug": "DEBUG",
    "dtc_read": "DTC_READ",
}


def gen_protocol_h() -> str:
    """generated/protocol.h — the firmware-side command-code constants from PROTOCOL.
    CommsManager switches on these; the same command set is exported in tuneit-meta.json."""
    lines = [
        BANNER, "#pragma once",
        "#ifndef JAYECU_GENERATED_PROTOCOL_H",
        "#define JAYECU_GENERATED_PROTOCOL_H",
        "",
        "// Wire-protocol command codes — single source of truth (codegen PROTOCOL).",
    ]
    for key, c in PROTOCOL["commands"].items():
        lines.append(f"#define JAYECU_CMD_{_CMD_MACRO[key]:<13} '{c['code']}'")
    lines += [
        "",
        "#define JAYECU_CFG_SELECTOR  0u   // config selector byte for 'r'/'w' (flat 32-bit offset)",
        "",
        "#endif // JAYECU_GENERATED_PROTOCOL_H",
        "",
    ]
    return "\n".join(lines)

# -----------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------

def module_snake(mod_name: str) -> str:
    """MyModule -> my_module"""
    s = re.sub(r'([A-Z]+)([A-Z][a-z])', r'\1_\2', mod_name)
    s = re.sub(r'([a-z\d])([A-Z])', r'\1_\2', s)
    return s.lower()

def array_name_pascal(array_name: str) -> str:
    """digital_inputs -> DigitalInputs"""
    return ''.join(p.capitalize() for p in array_name.split('_'))

# Default byte length of a `type: expression` field. MUST equal expr::PROGRAM_MAX in
# firmware/Signal/Expr.h — the host test `expr_studio_roundtrip` asserts they agree, so a
# change here that is not mirrored there fails the suite rather than silently truncating
# every stored program.
EXPR_PROGRAM_MAX = 64

def type_info(prim_types: dict, type_name: str) -> dict:
    if type_name not in prim_types:
        raise ValueError(f"Unknown type '{type_name}' — add it to primitive_types")
    return prim_types[type_name]

def field_size(prim_types: dict, f: dict) -> int:
    # A string is a char[length] ASCII blob — size is per-field, not a fixed primitive.
    # An expression is the same shape: a fixed-length BYTECODE blob for the VM
    # (firmware/Signal/Expr.h). Its length defaults to expr::PROGRAM_MAX.
    if f.get("type") in ("string", "expression"):
        return int(f.get("length", EXPR_PROGRAM_MAX))
    # enum/bool are TS `bits` constants stored in one byte.
    if f.get("type") in ("enum", "bool"):
        return 1
    return type_info(prim_types, f["type"])["size"]

def table_size(prim_types: dict, t: dict) -> int:
    ti = type_info(prim_types, t["type"])
    # Resizable tables reserve the MAX allocation (annotate_resizable sets
    # _alloc_elems); the live row/col count is a runtime scalar.
    if "_alloc_elems" in t:
        return ti["size"] * t["_alloc_elems"]
    if "rows" in t:
        return ti["size"] * t["rows"] * t["cols"]
    return ti["size"] * t["size"]

def array_element_size(prim_types: dict, arr: dict) -> int:
    # field_size handles enum/bool (1 byte) and string (char[len]); plain types fall
    # through to the primitive size. Lets config_array elements be enum dropdowns.
    return sum(field_size(prim_types, f) for f in arr["element"])

def config_array_total_size(prim_types: dict, arr: dict) -> int:
    return array_element_size(prim_types, arr) * arr["count"]

# Every `options_from: signals` field anywhere in a module's config — scalars, arrays, and the ELEMENT
# fields of a nested array (the precond selector sits two levels down: config.arrays[].element[].element[]).
# These hold a +1-encoded SignalId (0 = None), so their width decides how much of the catalog a user can pick;
# a walk that only looked one level deep silently found nothing, which is exactly how a guard becomes decor.
def _iter_signal_selector_fields(node, path=""):
    if isinstance(node, dict):
        if node.get("options_from") == "signals":
            yield path + (node.get("name") or "?"), node
        for k, v in node.items():
            yield from _iter_signal_selector_fields(v, path)
    elif isinstance(node, list):
        for v in node:
            yield from _iter_signal_selector_fields(v, path)

def normalize_module_config(schema: dict) -> None:
    """Accept the nested module-config form `config: {scalars, tables, arrays}` by
    flattening it to the internal flat keys (config / tables / config_arrays) that every
    generator already reads. A flat `config: [...]` list is left untouched. Idempotent —
    safe to call from both main() and gen_ini(). Tables/arrays are config; this lets the
    schema model that (config owns them) without changing any generator or the layout."""
    for mod in (schema.get("modules") or {}).values():
        c = mod.get("config")
        if isinstance(c, dict):
            mod["config"]        = c.get("scalars", []) or []
            mod["tables"]        = c.get("tables", []) or []
            mod["config_arrays"] = c.get("arrays", []) or []

def expand_inline_axes(modules: dict) -> None:
    """Table-owned axes. A table may declare each axis inline as a dict
    `x_axis: {signal, max, values, [units, scale, type, optional]}` instead of referencing a
    separately-declared, shareable axis. Expand each inline axis into a synthesized axis 'table'
    named `<table>_<ax>_axis` (owned by exactly this table — never shared) and set the legacy keys the
    rest of the pipeline already reads: the `<ax>_axis` name ref, `<ax>_channel` (channel == signal),
    and `<ax>_optional`. The table's live dimension is DERIVED from len(values) — so the axis bin count
    always equals the table size — while `max` drives the reserved/resizable allocation. Synthesized
    axes are tagged `_synth_axis`/`_axis_owned_by` so meta/studio show them as part of the table, not a
    separate config object. Run after normalize_module_config, before auto_table_channels. Legacy
    name-ref tables (string `x_axis`) pass through untouched."""
    DIM = {"x": "cols", "y": "rows", "z": "depth"}
    for mod in modules.values():
        synth = []
        for t in mod.get("tables", []):
            for ax in ("x", "y", "z"):
                spec = t.get(f"{ax}_axis")
                if not isinstance(spec, dict):
                    continue                                  # legacy name ref (or absent) — leave it
                vals = spec["values"]
                axis_name = f"{t['name']}_{ax}_axis"
                synth.append({
                    "name": axis_name,
                    "type": spec.get("type", "float"),        # float so the engine reads real-unit bins
                    "size": len(vals),
                    "max_size": spec["max"],
                    "default_values": list(vals),
                    "label": spec.get("label", f"{t.get('label', t['name'])} {ax.upper()} Axis"),
                    "units": spec.get("units", ""),
                    "scale": spec.get("scale", 1.0),
                    "_synth_axis": True,
                    "_axis_owned_by": t["name"],
                })
                t[f"{ax}_axis"]    = axis_name                # the ref the pipeline resolves
                if spec.get("external"):
                    t[f"{ax}_external"] = True                # no bus channel — the firmware supplies the
                else:                                         # coordinate at eval (instance idx / internal state)
                    t[f"{ax}_channel"] = spec["signal"]       # configurable signal driving the axis
                if spec.get("optional"):
                    t[f"{ax}_optional"] = True
                t[DIM[ax]] = len(vals)                        # axis size == table dimension
        if synth:
            mod.setdefault("tables", []).extend(synth)        # axes follow their table (schema order)


# -----------------------------------------------------------------------
# Element-local primitives — a table is a table, an array is an array, no matter where declared
#
# A config_array `element:` may declare a `type: table` field (a 1-D lookup: a resizable axis of
# breakpoints + a value cell per breakpoint + a live-count) or a `type: array` field (a fixed
# repeated sub-struct). This pass DESUGARS each into the flat scalar fields that the offset /
# serializer / layout-hash / shadow machinery already walks — so none of that changes — while
# recording an ordered emit plan (`_element_layout`) plus meta descriptors
# (`_element_tables` / `_element_arrays`). The C struct + C++ initializer emitters consult the plan
# to emit REAL arrays / nested structs (byte-identical to the flat run), and the meta emitter
# presents ONE clean table/array per declaration. So the schema declares the structure once and it
# falls out everywhere — no hand-enumerated cells, no downstream re-collapsing helpers.
# -----------------------------------------------------------------------

def _expand_element_table_std(f: dict, flat: list, plan: list, tables: list) -> None:
    """A STANDARD (multi-axis) table in a config_array element — the unified table form. Lays the SAME
    storage a module table has (typed cell grid + per-axis breakpoint array + live <ax>_axis_n + optional
    channel src / enable) into the element struct, so the config_array replication makes it per-instance
    for FREE (etb[i].ff_table). A per-element `<array>_<name>_desc(const <Elem>Config*)` is emitted by
    gen_table_descs_h. Declared with inline `x_axis` (+ optional `y_axis`/`z_axis`) + `cell_type`/`scale`."""
    name      = f["name"]
    cell_type = f.get("cell_type", "int16")
    label     = f.get("label", name)
    axes      = [(ax, f[f"{ax}_axis"]) for ax in ("x", "y", "z") if isinstance(f.get(f"{ax}_axis"), dict)]
    alloc     = 1
    for _, spec in axes:
        alloc *= int(spec["max"])
    axis_cfg = {}
    for ax, spec in axes:
        amax  = int(spec["max"])
        vals  = list(spec.get("values") or [])
        atype = spec.get("type", "float")
        alabel = spec.get("label", f"{label} {ax.upper()} Axis")
        members = []
        for k in range(amax):
            d = vals[k] if k < len(vals) else (vals[-1] if vals else 0)
            af = {"name": f"{name}_{ax}_axis_{k}", "type": atype, "default": d, "label": f"{alabel} {k}"}
            # Breakpoints are stored raw in the axis's own scalar type; the axis scale converts them to
            # engineering units. It has to ride on the field or the studio edits raw counts, and on the
            # descriptor (below) or the resolver compares an engineering-unit coordinate against raw.
            if float(spec.get("scale", 1.0)) != 1.0:
                af["scale"] = float(spec["scale"])
                if spec.get("digits") is not None:
                    af["digits"] = spec["digits"]
            if spec.get("units"):
                af["units"] = spec["units"]
            flat.append(af); members.append(af)
        plan.append({"kind": "prim_array", "handle": f"{name}_{ax}_axis", "ctype": atype,
                     "len": amax, "members": members})
        # A synthesized companion gets synthesized HELP. These fields exist only because the schema
        # declared a table, so there is nowhere for an author to write help for them — and a field
        # without help is a control that answers nothing when hovered, which is most of a table's
        # settings. Derived from the parent axis's label, so it stays correct as tables come and go.
        amin = int(spec.get("min", 2))                         # min live bins (schema-driven; default 2)
        nf = {"name": f"{name}_{ax}_axis_n", "type": "uint8", "default": max(amin, len(vals)),
              "min": amin, "max": amax, "label": f"{alabel} Bins",
              "help": f"How many breakpoints of '{alabel}' are LIVE, out of {amax} allocated. "
                      f"Shrinking the axis does not delete the values past the end — they stay in "
                      f"the tune and come back if it is grown again. Fewer, well-placed breakpoints "
                      f"beat many evenly-spaced ones: resolution is only worth spending where the "
                      f"surface actually bends."}
        flat.append(nf); plan.append({"kind": "scalar", "field": nf})
        cfg = {"n": nf["name"], "type": atype, "max": amax, "min": amin,
               "scale": float(spec.get("scale", 1.0))}
        if spec.get("signal") is not None:                     # bus-channel axis: a selector scalar
            sf = {"name": f"{name}_{ax}_src", "type": "int16", "default": spec["signal"],
                  "min": 0, "max": 254, "label": f"{alabel} Channel",
                  "help": f"WHICH SIGNAL '{alabel}' is looked up on. The axis carries the "
                          f"breakpoints; this says what is compared against them, so changing it "
                          f"re-points the table at a different quantity without moving a single "
                          f"cell — and the breakpoints, which were chosen for the old signal's "
                          f"range and units, almost certainly need redrawing when it changes."}
            flat.append(sf); plan.append({"kind": "scalar", "field": sf}); cfg["src"] = sf["name"]
        if spec.get("optional"):                               # optional axis: an enable toggle
            ef = {"name": f"{name}_{ax}_en", "type": "uint8", "default": 1 if spec.get("en_default") else 0,
                  "min": 0, "max": 1, "label": f"{alabel} Enabled",
                  "help": f"Use '{alabel}' as a second dimension. Off, the table is a one-dimensional "
                          f"curve on its X axis alone and this axis is ignored entirely — which is "
                          f"the right default for most tables, since a second dimension nobody "
                          f"calibrates is a row of copies that still has to be kept in step."}
            flat.append(ef); plan.append({"kind": "scalar", "field": ef}); cfg["en"] = ef["name"]
        axis_cfg[ax] = cfg
    cells = []
    # `cells:` seeds the DEFAULT TUNE with a starting curve. Most tables have no sensible default and
    # stay zero, but one that a fresh board must be able to drive on — the throttle's bias curve — needs
    # to arrive already populated, or the first move is made with no feed-forward at all.
    cell_defaults = f.get("cells") or []
    for k in range(alloc):
        cf = {"name": f"{name}_{k}", "type": cell_type,
              "default": cell_defaults[k] if k < len(cell_defaults) else 0,
              "label": f"{label} {k}"}
        flat.append(cf); cells.append(cf)
    plan.append({"kind": "prim_array", "handle": name, "ctype": cell_type, "len": alloc, "members": cells})
    tables.append({"name": name, "std": True, "cell_type": cell_type, "scale": f.get("scale", 1.0),
                   "label": label, "display": f.get("display", "grid"), "axis_cfg": axis_cfg,
                   "axis_specs": {ax: spec for ax, spec in axes}})


def _expand_element_table(f: dict, flat: list, plan: list, tables: list) -> None:
    """One `type: table` element-field. With inline `x_axis` -> the STANDARD multi-axis form
    (_expand_element_table_std). Legacy 1-D form (`axis`+`value`+`points`) -> count scalar + axis array +
    value array (a sensor cal). Both resolve through the one tbl::interp; the legacy form keeps its
    storage (cal_raw U16 axis + cal_val I16 cells) which IS already a 1-axis table."""
    if isinstance(f.get("x_axis"), dict):
        return _expand_element_table_std(f, flat, plan, tables)
    name  = f["name"]
    pts   = int(f.get("points") or f.get("cols"))
    axis  = f["axis"]
    value = f["value"]
    n_name = f.get("n", f"{name}_n")
    pairs  = list(f.get("default") or [])
    n_def  = max(2, min(pts, len(pairs) or 2))

    def pad(seq, k):
        if k < len(seq):
            return seq[k]
        return seq[-1] if seq else 0

    raws = [int(p[0]) for p in pairs]
    vals = [int(p[1]) for p in pairs]

    n_field = {"name": n_name, "type": "uint8", "label": f"{f.get('label', name)} Points",
               "default": n_def, "min": 2, "max": pts,
               "help": f"How many points of '{f.get('label', name)}' are LIVE, out of {pts} allocated. "
                       f"A sensor's own type seeds this at its natural count — a two-point linear "
                       f"sensor stores exactly two — so more points are only worth adding where the "
                       f"real curve bends. The raw axis must stay ascending across the live points."}
    flat.append(n_field)
    plan.append({"kind": "scalar", "field": n_field})

    ax_members = []
    for k in range(pts):
        af = {"name": f"{axis['name']}_{k}", "type": axis["type"],
              "label": f"{axis.get('label', 'Raw')} {k}", "default": pad(raws, k),
              "min": axis.get("min", 0), "max": axis.get("max", 0)}
        if axis.get("units"):
            af["units"] = axis["units"]
        flat.append(af)
        ax_members.append(af)
    plan.append({"kind": "prim_array", "handle": axis["name"], "ctype": axis["type"],
                 "len": pts, "members": ax_members})

    val_members = []
    for k in range(pts):
        vf = {"name": f"{value['name']}_{k}", "type": value["type"],
              "label": f"{value.get('label', 'Value')} {k}", "default": pad(vals, k)}
        if value.get("type_scaled"):
            vf["type_scaled"] = True
        flat.append(vf)
        val_members.append(vf)
    plan.append({"kind": "prim_array", "handle": value["name"], "ctype": value["type"],
                 "len": pts, "members": val_members})

    tables.append({
        "name": name, "display": f.get("display", "grid"), "points": pts,
        "label": f.get("label", name), "n_field": n_name,
        "axis":  {"field": axis["name"],  "type": axis["type"],
                  "units": axis.get("units", ""), "label": axis.get("label", "Raw"),
                  "scale": axis.get("scale", 1.0)},
        "value": {"field": value["name"], "type": value["type"],
                  "label": value.get("label", "Value"), "scale": value.get("scale", 1.0),
                  "type_scaled": bool(value.get("type_scaled")),
                  "min": value.get("min", 0), "max": value.get("max", 0)},
    })


def _expand_element_array(f: dict, flat: list, plan: list, arrays: list) -> None:
    """One `type: array` element-field (a fixed repeated sub-struct, e.g. precond[4]) -> the flat
    `<name>_<i>_<sub>` fields (struct order: element-major then subfield) + an emit plan for a real
    nested `<Struct> <name>[count]` array + a meta array descriptor. Per-subfield `default` may be a
    per-element list (one value per array index)."""
    name  = f["name"]
    count = int(f["count"])
    sub   = f["element"]
    struct_name = array_name_pascal(name) + "Sub"

    members_by_idx = []
    for i in range(count):
        row = []
        for sf in sub:
            ff = dict(sf)
            ff["name"] = f"{name}_{i}_{sf['name']}"
            d = sf.get("default", 0)
            ff["default"] = (d[i] if i < len(d) else (d[-1] if d else 0)) if isinstance(d, list) else d
            flat.append(ff)
            row.append(ff)
        members_by_idx.append(row)
    plan.append({"kind": "struct_array", "handle": name, "struct_name": struct_name,
                 "len": count, "subfields": sub, "members_by_idx": members_by_idx})

    arrays.append({
        "name": name, "count": count, "struct_name": struct_name,
        # A SUBFIELD IS COPIED, so anything the resolve pass added to the schema field has to be copied
        # WITH it or it is lost here. `control` is the one that matters: it is what makes a field a
        # signal picker rather than a number box, and cand[].sig — a field whose whole job is to name a
        # channel — reached the studio without it, as an empty dropdown.
        "subfields": [{"name": sf["name"], "type": sf.get("type"),
                       "label": sf.get("label", sf["name"]), "units": sf.get("units", ""),
                    "help": sf.get("help", ""),          # hover tooltip, same as any other field
                       "scale": sf.get("scale", 1.0), "min": sf.get("min", 0), "max": sf.get("max", 0),
                       **({"control": sf["control"]} if sf.get("control") else {}),
                       **({"default": sf["default"]} if sf.get("default") is not None else {}),
                       **({"options": sf["options"]} if sf.get("options") else {}),
                       **({"options_from": sf["options_from"]} if sf.get("options_from") else {})}
                      for sf in sub],
    })


def expand_element_primitives(modules: dict) -> None:
    """Desugar every config_array element's `type: table` / `type: array` fields (see above). Mutates
    `modules`; run after resolve_array_counts, before synthesize_sensor_defaults (which fills the
    per-sensor cal defaults on the flat fields this produces). Idempotent: an element already carrying
    `_element_layout` is skipped."""
    for mod in modules.values():
        for arr in (mod.get("config_arrays") or []):
            elem = arr.get("element")
            if not elem or arr.get("_element_layout") is not None:
                continue
            flat, plan, tables, arrays = [], [], [], []
            for f in elem:
                ft = f.get("type")
                if ft == "table":
                    _expand_element_table(f, flat, plan, tables)
                elif ft == "array":
                    _expand_element_array(f, flat, plan, arrays)
                else:
                    flat.append(f)
                    plan.append({"kind": "scalar", "field": f})
            arr["element"] = flat
            arr["_element_layout"] = plan
            if tables:
                arr["_element_tables"] = tables
            if arrays:
                arr["_element_arrays"] = arrays


def resolve_channel_name_defaults(schema: dict) -> None:
    """A numeric config field may give its default as a channel NAME (e.g. `default: clt`)
    instead of a fragile raw index — resolve it to the channel's index in the signal
    catalog (== its SignalId value). So adding/removing a channel can never silently
    point a *_src default at the wrong signal. Errors on an unknown name. Idempotent
    (an already-int default is left alone). Run AFTER normalize_module_config()."""
    channels = schema["signals"]
    idx = {c["id"]: i for i, c in enumerate(channels)}
    prims = set(schema.get("primitive_types", {}))

    def _resolve(fields, where):
        for f in fields:
            # Recurse into config_array element fields (incl. nested `type: array` sub-elements),
            # so a *_src default given as a signal name resolves inside an array too.
            if isinstance(f.get("element"), list):
                _resolve(f["element"], where)
            name = f.get("name", "")
            d = f.get("default")
            is_prim = f.get("type") in prims
            sig_default = isinstance(d, str) and is_prim
            if sig_default:
                if d not in idx:
                    raise SystemExit(f"{where}.{f.get('name')}: default '{d}' is not a channel id")
                f["default"] = idx[d]
            # A per-element (per-array-index) default may also be a list of channel NAMES
            # (e.g. an etb[]'s tps_a_src: [tps, aux_2]). Resolve each name to its index the same
            # way; leave already-int entries alone (idempotent on the second resolve pass).
            list_sig_default = isinstance(d, list) and is_prim and any(isinstance(x, str) for x in d)
            if list_sig_default:
                resolved = []
                for x in d:
                    if isinstance(x, str):
                        if x not in idx:
                            raise SystemExit(f"{where}.{f.get('name')}: default '{x}' is not a channel id")
                        resolved.append(idx[x])
                    else:
                        resolved.append(x)
                f["default"] = resolved
            # A field that holds a SignalId (a signal-name default, or a *_src/*_sig selector) -> the
            # studio renders a signal PICKER, not a raw number box. control:signal -> meta kind "signal".
            # A raw-SignalId selector -> the studio renders a signal PICKER, and the field must span the
            # whole catalog (which can exceed 255 for per-cylinder wideband/EGT). `options_from:signals`
            # fields (the precond `signal`, +1-encoded 0=None) are a DIFFERENT scheme, layout-locked to
            # firmware (and uint16, so they span the catalog) — leave them. For the rest: widen a narrow
            # (uint8) selector to int16 with -1 = unset
            # (a valid id is >= 0; -1 casts to SignalId 0xFFFF = SIG_NONE). An already-wide (uint16) raw
            # selector keeps its width but moves its old "unset" 255 -> 0xFFFF (SIG_NONE), since id 255 is
            # now a real signal (the reserved-255 hole is gone).
            if is_prim and not f.get("options_from") and (
                    f.get("control") == "sensor"
                    or sig_default or list_sig_default or name.endswith("_src")
                    or name.endswith("_sig") or name in ("sig", "signal")):
                f.setdefault("control", "signal")
                if f.get("type") in ("uint8", "int8"):
                    f["type"] = "int16"
                    if f.get("default") == 255:  f["default"] = -1        # unset
                    if f.get("max") in (254, 255):  f["max"] = 0x7FFF
                    f["min"] = -1                                         # -1 = unassigned
                else:                                                     # already uint16/int16 raw selector
                    if f.get("default") == 255:  f["default"] = 0xFFFF    # unset = SIG_NONE
                    if f.get("max") in (254, 255):  f["max"] = 0xFFFF

    for mname, mod in (schema.get("modules") or {}).items():
        _resolve(mod.get("config", []), mname)
        for arr in mod.get("config_arrays", []):
            _resolve(arr.get("element", []), f"{mname}.{arr.get('name')}")

def resolve_array_counts(schema: dict, board: str | None = None) -> None:
    """Resolve every config_array's `count_from: <schema key>` into a literal `count`
    = len(schema[key]) (e.g. Sensors sizes itself from the catalog), and
    `count_from_board: output_pins` into the active board's output-capable pin count (so the
    Outputs array auto-tracks the hardware — no hardcode). Idempotent; called from both main() and
    gen_ini() so any entry point — full codegen run, studio, or a unit test calling gen_ini directly
    — sees a concrete count. count_from_board falls back to the schema's literal `count` if no board."""
    for mod in (schema.get("modules") or {}).values():
        for arr in (mod.get("config_arrays") or []):
            cf = arr.get("count_from")
            if cf is not None:
                arr["count"] = len(schema.get(cf, []))
            if arr.get("count_from_board") == "output_pins":
                n = _board_output_pin_count(board)
                if n is not None:
                    arr["count"] = n
                # ROW i IS PIN i, so each row is named by its pin and offers only the functions that pin
                # can do: a coil needs a compare channel (IGNITION_OUT), an injector a low-side driver.
                rows = _board_output_rows(board)
                if rows and "element_labels" not in arr:
                    arr["element_labels"] = [sig for sig, _caps in rows]
                    arr["_element_options"] = {"function": [_output_row_functions(caps) for _s, caps in rows]}

def array_union_slots(config_arrays: list) -> list:
    """Group CONSECUTIVE config_arrays that share a non-empty `union` key into one
    overlay slot. Mutually-exclusive per-strategy pattern buffers (only one strategy
    is ever live) tag the same `union` name → they share one byte region instead of
    each reserving its own. Returns a list of slots; each slot is a list of array
    dicts (length 1 = a standalone array)."""
    slots = []
    for arr in config_arrays:
        u = arr.get("union")
        if u and slots and slots[-1][0].get("union") == u:
            slots[-1].append(arr)
        else:
            slots.append([arr])
    return slots

def array_slot_size(prim_types: dict, slot: list) -> int:
    """Bytes a slot consumes: the LARGEST member for a union group (they overlay),
    the array's own size for a standalone."""
    return max(config_array_total_size(prim_types, a) for a in slot)

def struct_size(prim_types: dict, fields: list, tables: list,
                config_arrays: list = None) -> int:
    sz = (sum(field_size(prim_types, f) for f in fields) +
          sum(table_size(prim_types, t) for t in tables))
    if config_arrays:
        sz += sum(array_slot_size(prim_types, slot)
                  for slot in array_union_slots(config_arrays))
    return sz

def ini_type(prim_types: dict, type_name: str) -> str:
    return type_info(prim_types, type_name)["ini_type"]

def c_type(prim_types: dict, type_name: str) -> str:
    return type_info(prim_types, type_name)["c_type"]

def is_pad(f: dict) -> bool:
    return f.get("pad", False) or f["name"].startswith("_pad")


def _elem_default(f: dict, idx: int):
    """A config-array element field's default for element `idx`. `default` may be a per-element
    list (e.g. capture_index: default: [0, 1, 2, 3] → one explicit value per stream); a scalar
    default applies to every element."""
    d = f.get("default", 0)
    if isinstance(d, list):
        if idx is None:
            return d[0] if d else 0
        return d[idx] if idx < len(d) else (d[-1] if d else 0)
    return d

def _elem_init_value(f: dict, idx: int) -> str:
    """One element field's C++ initializer text.

    Strings and expression programs are ARRAYS, so a bare default is not an initializer for them:
    `.name = ` with an empty default emitted nothing at all and the whole default_config failed to
    compile. `{}` zero-fills, which is what an unset name or an empty program means; a non-empty
    string default becomes a real literal.
    """
    t = f.get("type")
    if t == "expression":
        return "{}"                                   # empty program = always armed
    if t == "string":
        d = _elem_default(f, idx)
        if not d:
            return "{}"
        esc = str(d).replace("\\", "\\\\").replace('"', '\\"')
        return f'"{esc}"'
    return str(_elem_default(f, idx))


def _elem_init_str(prim_types: dict, arr: dict, i: int) -> str:
    """C++ designated-initializer for config-array element `i`, driven by the emit plan so a
    `type: table`/`type: array` field initialises as a real array `{..}` / nested `{{..}}` —
    byte-matching both the regrouped struct and the flat binary serializer."""
    plan = arr.get("_element_layout")
    if not plan:
        return "{" + ", ".join(
            f".{f['name']} = {_elem_init_value(f, i)}" for f in arr["element"]) + "}"
    parts = []
    for item in plan:
        if item["kind"] == "scalar":
            f = item["field"]
            parts.append(f".{f['name']} = {_elem_init_value(f, i)}")
        elif item["kind"] == "prim_array":
            vals = ", ".join(str(_elem_default(m, i)) for m in item["members"])
            parts.append(f".{item['handle']} = {{{vals}}}")
        elif item["kind"] == "struct_array":
            rows = []
            for row in item["members_by_idx"]:
                cells = ", ".join(f".{sf['name']} = {_elem_default(row[p], i)}"
                                  for p, sf in enumerate(item["subfields"]))
                rows.append("{" + cells + "}")
            parts.append(f".{item['handle']} = {{{', '.join(rows)}}}")
    return "{" + ", ".join(parts) + "}"

def const_name(mod_name: str, raw: str) -> str:
    """Globally-unique INI/JSON constant name. Two modules can each declare a
    'map_src' or 'rpm_axis'; constants are keyed by name and must be unique,
    so every *config* constant is namespaced by its module.
    Telemetry channels are deliberately NOT prefixed — they must keep stable
    names that gauges and a table's x_channel/y_channel reference."""
    return f"{module_snake(mod_name)}_{raw}"

# A float-stored scalar's display precision. Its scale says nothing about how
# finely it can be set (an F32 at scale 1.0 holds 1.5 exactly), so the
# scale-derived rule below would show every one of them as a whole number.
# Three places is what the fields on F32 storage need: the ETB position gains
# were a uint16 x0.001 field before they became floats, so a thousandth is the
# precision they have always been set to.
FLOAT_DIGITS = 3

def scalar_digits(scale: float, dt: str = "") -> int:
    """Decimal places a scalar shows (and the spin-box step = 1 in the last
    digit). Derived from the display scale: 1.0 -> 0, 0.1 -> 1, 0.001 -> 3 —
    because a scaled INTEGER cannot express anything finer than its scale.
    F32 storage can, so it takes FLOAT_DIGITS as a floor: a Position Kp of 1.5
    displayed to the scale's 0 places read "2", and a nudge WROTE that back."""
    d = 0 if scale >= 1.0 else max(0, int(round(-math.log10(scale))))
    return max(d, FLOAT_DIGITS) if dt == "F32" else d

# -----------------------------------------------------------------------
# Resizable tables/axes — annotation pass
#
# An "axis" is a 1D table referenced as another table's x_axis/y_axis. If an
# axis declares max_size > size it becomes RESIZABLE: we reserve max_size of
# static storage and synthesize a tunable length scalar <axis>_n (default=size,
# min 2, max=max_size). A 2D table's cols = its x_axis's _n, rows = its y_axis's
# _n; a 1D value table's length = its x_axis's _n. So growing one axis re-grids
# every table that references it, and storage is always the compile-time MAX.
# This pass MUTATES `modules` in place; run it once before any generation.
# -----------------------------------------------------------------------

def _resolve_axis_table(ref: str, cur_mod: str, modules: dict):
    """Resolve an x_axis/y_axis ref to (owner_module, raw_name, table_dict).
    Ref is 'Module.axis' (schema key or its snake form) or a bare 'axis'
    (same module)."""
    if "." in ref:
        mod_part, raw = ref.split(".", 1)
        owner = mod_part if mod_part in modules else None
        if owner is None:
            owner = next((m for m in modules if module_snake(m) == mod_part), None)
    else:
        owner, raw = cur_mod, ref
    if owner is None or owner not in modules:
        raise ValueError(f"axis ref '{ref}' (from {cur_mod}): unknown module")
    for t in modules[owner].get("tables", []):
        if t["name"] == raw:
            return owner, raw, t
    raise ValueError(f"axis ref '{ref}' (from {cur_mod}): no table '{raw}' in '{owner}'")

_IDENT = re.compile(r'^[A-Za-z_][A-Za-z0-9_]*$')

def to_ident(name: str) -> str:
    """Coerce an arbitrary name to a valid C identifier: every character that is
    not a letter/digit/underscore becomes '_', and a leading digit gets an '_'
    prefix. 'fuel table rows' -> 'fuel_table_rows'. Kept byte-identical to the
    studio's cIdent() so its full-ecu.ini preview matches what we emit."""
    s = re.sub(r'[^A-Za-z0-9_]', '_', name or '')
    if s and s[0].isdigit():
        s = '_' + s
    return s

def sanitize_names(modules: dict) -> None:
    """Field/table/array names become C struct members and INI constant ids, so
    they MUST be C identifiers (letters/digits/_, no spaces). Rather than fail the
    build, coerce any invalid name to a valid one IN PLACE and warn — labels are
    left untouched (they may contain spaces). Mutates `modules`; run before any
    generation so every downstream use sees the sanitized name."""
    fixed = []
    for mod, md in modules.items():
        for sec in ("telemetry", "config", "tables"):
            for f in (md.get(sec) or []):
                nm = f.get("name", "")
                if nm and not _IDENT.match(nm):
                    f["name"] = to_ident(nm)
                    fixed.append(f"{mod}.{sec}: '{nm}' -> '{f['name']}'")
        for arr in (md.get("config_arrays") or []):
            nm = arr.get("name", "")
            if nm and not _IDENT.match(nm):
                arr["name"] = to_ident(nm)
                fixed.append(f"{mod}.config_arrays: '{nm}' -> '{arr['name']}'")
    if fixed:
        print("  WARNING: coerced invalid names to C identifiers (rename in the "
              "schema to silence):\n    " + "\n    ".join(fixed))

# tbl::CellType per schema cell type (TableEval.h)
_CELL_TYPE = {"uint8": "tbl::CELL_U8", "int8": "tbl::CELL_I8",
              "uint16": "tbl::CELL_U16", "int16": "tbl::CELL_I16", "float": "tbl::CELL_F32"}


def auto_table_channels(modules: dict) -> None:
    """'Every axis, every table, configurable.' For each VALUE table (one with an x_axis), synthesize
    the per-axis channel selectors <table>_<ax>_src (default = the table's *_channel) and, for any axis
    flagged `<ax>_optional: true`, an enable toggle <table>_<ax>_en (default off, or `<ax>_en_default: 1`
    to ship enabled — e.g. an optional X axis that should keep its 1D curve by default). These replace the
    hand-written _src/_en scalars — declare a table's axes once and the config falls out. Records the
    field names on the table (`_axis_cfg`) for the descriptor emitter. Run before
    resolve_channel_name_defaults (so the string `default:` resolves) and before annotate_resizable."""
    for mod_name, mod in modules.items():
        new_scalars = []
        for t in mod.get("tables", []):
            if not t.get("x_axis") or t.get("_align_pad"):
                continue
            label = t.get("label", t["name"])
            cfg = {}
            for ax in ("x", "y", "z"):
                if not t.get(f"{ax}_axis"):
                    continue
                if t.get(f"{ax}_external"):
                    cfg[ax] = {}                              # external axis: no channel selector; the
                else:                                         # firmware passes the coordinate at eval
                    src = f"{t['name']}_{ax}_src"
                    # Synthesized field, synthesized HELP. Nobody can author help for a scalar that
                    # only exists because a table was declared, and a field without help is a control
                    # that answers nothing on hover — which was most of every table's settings.
                    new_scalars.append({
                        "name": src, "type": "uint8", "scale": 1.0, "min": 0, "max": 254,
                        "label": f"{label} {ax.upper()} Channel", "units": "",
                        "help": f"WHICH SIGNAL the {ax.upper()} axis of '{label}' is looked up on. "
                                f"The axis holds the breakpoints; this says what is compared against "
                                f"them, so changing it re-points the table at a different quantity "
                                f"without moving a single cell — and the breakpoints, chosen for the "
                                f"old signal's range and units, will almost certainly need redrawing.",
                        "default": t.get(f"{ax}_channel", 0), "_auto_table_chan": True})
                    cfg[ax] = {"src": src}
                if t.get(f"{ax}_optional"):
                    en = f"{t['name']}_{ax}_en"
                    new_scalars.append({
                        "name": en, "type": "uint8", "scale": 1.0, "min": 0, "max": 1,
                        "label": f"{label} {ax.upper()} Axis Enabled", "units": "",
                        "help": f"Use the {ax.upper()} axis of '{label}' as a real dimension. Off, "
                                f"the table collapses to a curve on its remaining axes and this one "
                                f"is ignored entirely — the right default for most optional axes, "
                                f"since a dimension nobody calibrates is a set of duplicate rows that "
                                f"still has to be kept in step.",
                        "default": 1 if t.get(f"{ax}_en_default") else 0, "_auto_table_chan": True})
                    cfg[ax]["en"] = en
                elif t.get(f"_{ax}_en_field"):
                    cfg[ax]["en"] = t[f"_{ax}_en_field"]   # shared with the table this axis is borrowed from
            t["_axis_cfg"] = cfg
        if new_scalars:
            mod.setdefault("config", []).extend(new_scalars)


def gen_module_cadence_h(schema: dict) -> str:
    """generated/module_cadence.h — per-module scheduler cadence (Hz) from each module's `cadence_hz`
    attribute. These are FIRMWARE CONSTANTS, never config: cadence is a control-architecture invariant
    (a wrong rate breaks a control loop or a fail-safe), so it lives in the schema + a compile-time
    constant and NEVER enters EcuConfig / the tune. SystemComposer passes cadence::<Module> to
    add_participant; the scheduler runs the module every round(1000/hz) frames (phase-staggered) and
    derives its publish ttl from it. Omitted -> 1000 (every 1 kHz frame)."""
    L = [BANNER, "#pragma once", "#include <cstdint>", "",
         "// How often each module's OUTPUT must refresh — the control cadence, architect-owned (schema",
         "// module `cadence_hz`), NOT a tunable. The scheduler decimates + phase-staggers to this rate and",
         "// derives the fail-safe publish ttl from it. Per-cycle modules (Ignition/FuelCalculator) are not",
         "// listed here — they run per engine cycle, not at a fixed Hz.",
         "namespace cadence {"]
    for mname, mod in (schema.get("modules") or {}).items():
        hz = int((mod or {}).get("cadence_hz", 1000))
        L.append(f"    constexpr uint16_t {mname} = {hz}u;")
    L += ["}   // namespace cadence", ""]
    return "\n".join(L)


def gen_mlg_log_h(telem_fields: list, prim_types: dict) -> str:
    """generated/mlg_log.h — the MLG v2 field table for the SD datalogger.

    MegaLogViewer is the analyser, so the ECU writes the format it already reads rather than one of
    our own (spec: MLG_Binary_LogFormat_2.0, EFI Analytics). Each field carries its NAME, UNITS, SCALE and DIGITS in the
    file's own header, which is what makes a log decodable years later with no reference to the meta
    it was written against — the problem the old raw-struct log had.

    Only channels the catalog marks `datalog:` are logged. Every channel costs 89 bytes of header in
    EVERY file, so "all of them" is a decision with a price rather than a default — and it is the
    same set the studio's recorder offers as its standard.

    The table doubles as the GATHER list: `off`/`size` say where the value sits in the packed
    EcuTelemetry frame, so a record is read straight out of the frame the telemetry path already
    builds rather than from a second copy of every value."""
    # MLG scalar ordinals (mlg_types.h): U08 0, S08 1, U16 2, S16 3, U32 4, S32 5, S64 6, F32 7.
    MLG_TYPE = {"uint8": 0, "int8": 1, "uint16": 2, "int16": 3,
                "uint32": 4, "int32": 5, "float": 7}
    # EVERY channel, not just the logged ones. This is the descriptor CATALOG — the selection is a
    # tune field (Datalog.mask), because what a car logs is a tuning decision and not a build-time
    # one. `by_default` is the definition's opinion, used when the mask is all zero.
    rows, rec_len = [], 0
    for (name, typ, offset, f, cat) in telem_fields:
        code = MLG_TYPE.get(typ)
        if code is None:
            sys.exit(f"ERROR: telemetry channel '{name}' is type {typ}, which MLG has no ordinal for")
        sz = field_size(prim_types, f)
        rec_len += sz
        rows.append((name, f, cat, offset, sz, code))
    if not rows:
        sys.exit("ERROR: the telemetry frame has no channels — an SD log would have no columns")

    L = [BANNER, "#pragma once", "#include <cstdint>", "",
         "// MLG v2 (MegaLogViewer) field table for the SD datalogger — see gen_mlg_log_h.",
         "// A descriptor is 89 bytes in the FILE; `off`/`size` locate the value in the packed",
         "// EcuTelemetry frame, so a record is gathered from the frame telemetry already builds.",
         "struct MlgFieldDesc {",
         "    const char* name;       // <= 34 bytes in the file, NUL-padded",
         "    const char* units;      // <= 10",
         "    const char* category;   // <= 34; MLV groups its channel list by this",
         "    float       scale;      // MLV shows (raw + transform) * scale",
         "    float       transform;",
         "    uint16_t    off;        // byte offset in EcuTelemetry",
         "    uint8_t     size;       // 1 / 2 / 4",
         "    uint8_t     type;       // MLG scalar ordinal",
         "    int8_t      digits;     // decimal places",
         "    bool        by_default; // in the log when the tune's mask says nothing",
         "};",
         "",
         f"static constexpr uint16_t MLG_FIELD_COUNT = {len(rows)}u;",
         "// The LARGEST a record can be — every channel selected. The real length is a runtime sum",
         "// over the tune's mask; this is what a buffer has to be able to hold.",
         f"static constexpr uint16_t MLG_MAX_RECORD_LEN = {rec_len}u;",
         "static const MlgFieldDesc MLG_FIELDS[MLG_FIELD_COUNT] = {"]
    for (name, f, cat, offset, sz, code) in rows:
        scale  = float(f.get("scale", 1.0) or 1.0)
        digits = scalar_digits(scale)
        units  = (f.get("units") or "")[:10].replace('"', "")
        # A C++ float literal, always with a point: "%.9g" of 1.0 is "1", and "1f" does not compile.
        sc = f"{scale:.9g}"
        if "." not in sc and "e" not in sc and "E" not in sc:
            sc += ".0"
        L.append(f'    {{ "{name[:34]}", "{units}", "{(cat or "Uncategorised")[:34]}", '
                 f'{sc}f, 0.0f, {offset}u, {sz}u, {code}u, {digits}, '
                 f'{"true" if f.get("datalog") else "false"} }},')
    L += ["};", ""]
    return "\n".join(L)


def gen_table_descs_h(modules: dict) -> str:
    """generated/table_descs.h — one tbl::TableDesc builder per configurable table, built from the
    live module config (so it always points at the active buffer). The firmware calls
    tbl::table_eval(<table>_desc(cfg_), bus) for every lookup."""
    out = [BANNER, "#pragma once", '#include "../firmware/Engine/TableEval.h"',
           # A learned table's cells are a pointer into the RAM-backed learned region, so the descriptor
           # builder needs the accessor and the codegen-assigned offsets. Every other table's cells are a
           # config-struct member and need neither.
           '#include "../firmware/Platform/platform_hal.h"',
           '#include "learned_layout.h"']
    have = {m for m, mod in modules.items()
            if any(t.get("_axis_cfg") for t in mod.get("tables", []))
            or any(st.get("std") for arr in mod.get("config_arrays", [])
                   for st in (arr.get("_element_tables") or []))}
    for m in sorted(have):
        out.append(f'#include "modules/{module_snake(m)}_config.h"')
    out.append("")

    def axisdesc(mod_name, t, ax):
        if not t.get(f"{ax}_axis"):
            return "{}"
        am, an, at = _resolve_axis_table(t[f"{ax}_axis"], mod_name, modules)
        breaks = f"c->{an}"
        if at.get("_resizable"):
            n_live, n_fixed = f"&c->{an}_n", "0"
        else:
            n_live, n_fixed = "nullptr", f"(uint8_t){const_name(am, an).upper()}_ALLOC"
        c = t["_axis_cfg"][ax]
        en  = f"&c->{c['en']}"  if "en"  in c else "nullptr"
        src = f"&c->{c['src']}" if "src" in c else "nullptr"   # external axis: no selector
        btype = _CELL_TYPE.get(at.get("type", "float"), "tbl::CELL_F32")   # axis breakpoint scalar type
        # Breakpoints are stored raw; the axis scale converts them to engineering units, so the
        # resolver can compare a bus/module coordinate (always engineering units) against them.
        ascale = float(at.get("scale", 1.0))
        # ...and how wide the axis's STORAGE is. The cells are laid out at this stride whatever
        # <axis>_n currently says, so a resize is a change of search bound and nothing moves.
        alloc = f"{const_name(am, an).upper()}_ALLOC"
        return f"{{ {breaks}, {btype}, {n_live}, {n_fixed}, {src}, {en}, {ascale!r}f, (uint16_t){alloc} }}"

    for mod_name, mod in modules.items():
        cfg_type = f"{mod_name}Config"
        for t in mod.get("tables", []):
            if not t.get("_axis_cfg"):
                continue
            # A learned table's CELLS live in the RAM-backed learned region, its AXES in EcuConfig like
            # any other table's. That is the only difference, so it is the only thing that differs here:
            # the cell pointer is the learned base plus this block's codegen-assigned offset instead of a
            # config-struct member. Everything downstream then has the ordinary TableDesc it needs —
            # which is what lets a module call tbl::lookup() instead of hand-rolling
            # `static_cast<int>(v / SPAN * BINS)` against constants restating the axis.
            if t.get("_learned"):
                blk = t["_learned"].upper()
                # The cast follows the block's DECLARED cell type, not a fixed float — a learned block
                # sized in int16 is read as int16, and LEARNED_<BLK>_BYTES already states its extent.
                ctype = {"float": "float", "int16": "int16_t"}[t["type"]]
                cells = (f"reinterpret_cast<const {ctype}*>(platform_learned_block("
                         f"LEARNED_{blk}_OFFSET, LEARNED_{blk}_BYTES))")
            else:
                cells = f"c->{t['name']}"
            out += [
                f"inline tbl::TableDesc {t['name']}_desc(const {cfg_type}* c) {{",
                f"    return {{ {cells}, {_CELL_TYPE[t['type']]}, {float(t.get('scale', 1.0))!r}f,",
                f"        {axisdesc(mod_name, t, 'x')},",
                f"        {axisdesc(mod_name, t, 'y')},",
                f"        {axisdesc(mod_name, t, 'z')} }};",
                "}",
            ]
        # Per-element (standard) tables — a desc per ARRAY ELEMENT (the config_array IS the instance dim):
        #   <array>_<name>_desc(const <Elem>Config* e)  ->  e.g. etb_ff_table_desc(&cfg->etb[i])
        for arr in mod.get("config_arrays", []):
            elem_type = array_name_pascal(arr["name"]) + "Config"
            for st in (arr.get("_element_tables") or []):
                if not st.get("std"):
                    continue
                nm = st["name"]
                def eax(ax, st=st, nm=nm):
                    cfg = st["axis_cfg"].get(ax)
                    if not cfg:
                        return "{}"
                    bt  = _CELL_TYPE.get(cfg["type"], "tbl::CELL_F32")
                    src = f"&e->{cfg['src']}" if "src" in cfg else "nullptr"
                    en  = f"&e->{cfg['en']}"  if "en"  in cfg else "nullptr"
                    return (f"{{ e->{nm}_{ax}_axis, {bt}, &e->{cfg['n']}, 0, {src}, {en}, "
                            f"{float(cfg.get('scale', 1.0))!r}f }}")
                out += [
                    f"inline tbl::TableDesc {arr['name']}_{nm}_desc(const {elem_type}* e) {{",
                    f"    return {{ e->{nm}, {_CELL_TYPE[st['cell_type']]}, {float(st['scale'])!r}f,",
                    f"        {eax('x')},",
                    f"        {eax('y')},",
                    f"        {eax('z')} }};",
                    "}",
                ]
    out.append("")
    return "\n".join(out)


def expr_table_registry(modules: dict) -> list:
    """The tables an EXPRESSION may read, in id order.

    An id is the table's index in this list and it is stored inside compiled bytecode, so the order has
    to be one both sides derive the same way: schema order, module by module, whole-module tables only.
    Per-ELEMENT tables (a per-ETB feed-forward map, a per-slot duty map) are deliberately absent — an id
    would have to carry the element index too, and "which throttle body" is not a question the current
    operand can ask.

    A renumber cannot silently re-point a stored program: bytecode is compiled against the layout hash,
    and adding or removing a table changes the config layout, so old programs are rejected rather than
    reinterpreted.
    """
    out = []
    for mod_name, mod in modules.items():
        if mod_name == "System":
            continue
        ms = module_snake(mod_name)
        for t in mod.get("tables", []):
            if not t.get("_axis_cfg"):
                continue
            out.append({"key": const_name(mod_name, t["name"]),
                        "expr": f'{t["name"]}_desc(&g_config.{ms})',
                        "label": t.get("label", t["name"])})
    return out


def gen_table_registry_h(modules: dict) -> str:
    """generated/table_registry.h — id -> table, for the expression VM.

    Two questions, one registry: expr_table_value() reads the table at its OWN axes (OP_TABLE), and
    expr_table_at() reads it at an X the program supplies (OP_INTERP). That is the entire difference
    between a table and a curve in this firmware — everything interpolates through tbl::interp either
    way — so it would be perverse to give them separate registries.
    """
    reg = expr_table_registry(modules)
    out = [BANNER, "#pragma once", '#include "ecu_config.h"', '#include "table_descs.h"',
           '#include "../firmware/Signal/SignalBus.h"', "",
           f"static constexpr uint16_t EXPR_TABLE_COUNT = {len(reg)}u;", ""]
    out += ["// The descriptor for an id, or false if there is no such table.",
            "inline bool expr_table_desc(uint16_t id, tbl::TableDesc& out) {",
            "    switch (id) {"]
    for i, e in enumerate(reg):
        out.append(f'        case {i}: out = {e["expr"]}; return true;   // {e["key"]}')
    out += ["        default: return false;", "    }", "}", "",
            "// OP_TABLE: the table as the firmware reads it, at its own configured axes.",
            "inline bool expr_table_value(uint16_t id, float& out, void* user) {",
            "    tbl::TableDesc d;",
            "    if (!user || !expr_table_desc(id, d)) return false;",
            "    out = tbl::table_eval(d, *static_cast<SignalBus*>(user));",
            "    return true;",
            "}", "",
            "// OP_INTERP: the same table, read at the x the program pushed.",
            "inline bool expr_table_at(uint16_t id, float x, float& out, void* user) {",
            "    tbl::TableDesc d;",
            "    if (!user || !expr_table_desc(id, d)) return false;",
            "    out = tbl::table_eval_at(d, *static_cast<SignalBus*>(user), x);",
            "    return true;",
            "}", "",
            "// The names, in id order — the studio compiles a name to the id at this index.",
            "inline const char* expr_table_name(uint16_t id) {",
            "    switch (id) {"]
    for i, e in enumerate(reg):
        out.append(f'        case {i}: return "{e["key"]}";')
    out += ['        default: return "";', "    }", "}", ""]
    return "\n".join(out)


def gen_script_lookups_h(modules: dict) -> str:
    """generated/script_lookups.h — name->value lookups for the Lua read API.
      script_get_calibration(name, out) : read a config scalar by its TS/ini name (scale applied).
      script_eval_table(name, bus, out) : interpolate a configurable table by name at the live axes.
    Keys are the namespaced TS constant names (const_name), so Lua matches what the studio shows."""
    out = [BANNER, "#pragma once", "#include <cstring>",
           '#include "ecu_config.h"', '#include "table_descs.h"',
           '#include "../firmware/Signal/SignalBus.h"', "",
           "// getCalibration(name): read a tune scalar by its (namespaced) TS name. false if unknown.",
           "inline bool script_get_calibration(const char* n, float& out) {"]
    for mod_name, mod in modules.items():
        if mod_name == "System":
            continue
        ms = module_snake(mod_name)
        for f in mod.get("config", []):
            if f.get("type") in ("string", "expression"):
                continue   # not a numeric calibration Lua can read
            key = const_name(mod_name, f["name"])
            scale = float(f.get("scale", 1.0))
            out.append(f'    if (!strcmp(n, "{key}")) {{ out = g_config.{ms}.{f["name"]} * {scale!r}f; return true; }}')
    out += ["    return false;", "}", "",
            "// evalTable(name): interpolate a configurable table by its (namespaced) TS name. false if unknown.",
            "inline bool script_eval_table(const char* n, SignalBus& bus, float& out) {"]
    for mod_name, mod in modules.items():
        if mod_name == "System":
            continue
        ms = module_snake(mod_name)
        for t in mod.get("tables", []):
            if not t.get("_axis_cfg"):
                continue
            key = const_name(mod_name, t["name"])
            out.append(f'    if (!strcmp(n, "{key}")) {{ out = tbl::table_eval({t["name"]}_desc(&g_config.{ms}), bus); return true; }}')
    out += ["    return false;", "}", ""]
    return "\n".join(out)


def annotate_resizable(modules: dict, axis_max: int = 32) -> None:
    # Pass 1: mark which tables are used as axes.
    for mod_name, mod in modules.items():
        for t in mod.get("tables", []):
            if "rows" in t and t.get("x_axis") and t.get("y_axis"):
                _, _, xt = _resolve_axis_table(t["x_axis"], mod_name, modules)
                _, _, yt = _resolve_axis_table(t["y_axis"], mod_name, modules)
                xt["_is_axis"] = True
                yt["_is_axis"] = True
                if t.get("z_axis"):                       # 3D table — the Z/depth axis too
                    _, _, zt = _resolve_axis_table(t["z_axis"], mod_name, modules)
                    zt["_is_axis"] = True
            elif t.get("x_axis"):
                _, _, xt = _resolve_axis_table(t["x_axis"], mod_name, modules)
                xt["_is_axis"] = True

    # An axis inherits the project-wide maximum (table_defaults.axis_max) unless it pins its own
    # max_size — so table sizes are DEFINABLE in one place, not concrete per-axis. The live bin
    # count (<axis>_n) is still a runtime scalar (resizable up to this max).
    for mod in modules.values():
        for t in mod.get("tables", []):
            if t.get("_is_axis"):
                t.setdefault("max_size", axis_max)

    # Pass 2: flag resizable axes + synthesize their <axis>_n length scalars.
    # _n scalars are appended to the module's `config` list so they land BEFORE
    # the tables in both the struct and the INI (required for the expression).
    for mod_name, mod in modules.items():
        new_scalars = []
        for t in mod.get("tables", []):
            max_size = t.get("max_size", t.get("size", 0))
            # AN AXIS IS RESIZABLE FULL STOP, not only when it ships below its allocation. This used to
            # read `max_size > size`, so an axis whose default happened to equal its allocation was
            # FROZEN — and shrinking is the useful direction: some maps only need a 3x3, and nobody
            # should be made to fill an 8x8 because the default arrived full. There is no reason to
            # restrict any table from being resized; the allocation is the ceiling, not the grid. Growing past max_size still needs firmware, which is the one thing a user
            # cannot self-serve — so reserve generously and let them shape it.
            if t.get("_is_axis") and "size" in t and max_size >= t["size"]:
                t["_resizable"] = True
                _alabel = t.get("label", t["name"])
                new_scalars.append({
                    "name":   t["name"] + "_n",
                    "type":   "uint8",
                    "label":  _alabel + " bins",
                    # Synthesized field, synthesized HELP — see the axis-channel scalars above.
                    "help":   f"How many breakpoints of '{_alabel}' are LIVE, out of "
                              f"{t['max_size']} allocated. Shrinking the axis does not delete the "
                              f"values past the end — they stay in the tune and reappear if it is "
                              f"grown again. Every table sharing this axis is resized with it. "
                              f"Fewer, well-placed breakpoints beat many evenly-spaced ones: "
                              f"resolution is only worth spending where the surface actually bends.",
                    "scale":  1.0,
                    # Schema-driven floor (default 2) — but NEVER above the axis's own default size: an axis
                    # that ships collapsed to one bin is a legitimate state, and a floor of 2 put its own
                    # default outside its declared range (and above max on a 1-bin allocation).
                    "min":    min(int(t.get("min_size", 2)), int(t["size"])),
                    "max":    t["max_size"],
                    "default": t["size"],
                    "_dim_for": t["name"],      # marks a dimension scalar
                })
        if new_scalars:
            mod.setdefault("config", []).extend(new_scalars)

    # Pass 3: annotate storage + INI dims for every table.
    for mod_name, mod in modules.items():
        for t in mod.get("tables", []):
            _annotate_table_storage(t, mod_name, modules)

def _annotate_table_storage(t: dict, cur_mod: str, modules: dict) -> None:
    if "rows" in t:                                   # 2D / 3D table
        if t.get("x_axis") and t.get("y_axis"):
            xm, xr, xt = _resolve_axis_table(t["x_axis"], cur_mod, modules)
            ym, yr, yt = _resolve_axis_table(t["y_axis"], cur_mod, modules)
            x_max = xt.get("max_size", xt.get("size"))
            y_max = yt.get("max_size", yt.get("size"))
            # Z/depth (optional): a third axis. Storage = x_max*y_max*z_max; the firmware indexes at
            # LIVE strides cell[z*(cols*rows) + y*cols + x]. The TS editor stays 2D on the z=0 plane
            # (TS has no native 3rd axis); full-depth editing is the studio's job.
            z_max = z_depth = 1
            if t.get("z_axis"):
                _, _, zt = _resolve_axis_table(t["z_axis"], cur_mod, modules)
                z_max   = zt.get("max_size", zt.get("size"))
                z_depth = t.get("depth", zt.get("size"))
                t["_max_depth"] = z_max
            t["_max_cols"]   = x_max
            t["_max_rows"]   = y_max
            t["_alloc_elems"] = x_max * y_max * z_max
            t["_live_elems"]  = t["cols"] * t["rows"] * z_depth
            x_res, y_res = xt.get("_resizable"), yt.get("_resizable")
            if x_res or y_res:
                # Per-axis dims: a resizable axis references its live <axis>_n; a fixed axis (e.g. a
                # secondary axis reserved at its max while the primary stays resizable) is a literal —
                # its _n scalar isn't emitted, so {ref}-ing it would dangle in TS.
                xdim = "{%s}" % const_name(xm, xr + "_n") if x_res else str(t["cols"])
                ydim = "{%s}" % const_name(ym, yr + "_n") if y_res else str(t["rows"])
                t["_ini_dims"]       = f"[{xdim}x{ydim}]"   # base (z=0) plane for TS
                t["_resizable_table"] = True
            else:
                t["_ini_dims"]       = f"[{t['cols']}x{t['rows']}]"  # [cols x rows]
                t["_resizable_table"] = False
        else:                                         # 2D without paired axes
            t["_alloc_elems"]    = t["rows"] * t["cols"]
            t["_live_elems"]     = t["rows"] * t["cols"]
            t["_max_cols"]       = t["cols"]          # the allocation IS the grid: nothing resizes it
            t["_max_rows"]       = t["rows"]
            t["_ini_dims"]       = f"[{t['cols']}x{t['rows']}]"
            t["_resizable_table"] = False
    elif t.get("_resizable"):                          # this 1D table IS a resizable axis
        t["_alloc_elems"]    = t["max_size"]
        t["_live_elems"]     = t["size"]
        t["_ini_dims"]       = "[{%s}]" % const_name(cur_mod, t["name"] + "_n")
        t["_resizable_table"] = True
    elif t.get("x_axis"):                              # 1D value table — follows its x_axis
        xm, xr, xt = _resolve_axis_table(t["x_axis"], cur_mod, modules)
        if xt.get("_resizable"):
            t["_alloc_elems"]    = xt["max_size"]
            t["_live_elems"]     = t["size"]
            t["_max_cols"]       = xt["max_size"]     # the axis's allocation IS this table's width
            t["_ini_dims"]       = "[{%s}]" % const_name(xm, xr + "_n")
            t["_resizable_table"] = True
        else:
            t["_alloc_elems"]    = t["size"]
            t["_live_elems"]     = t["size"]
            t["_max_cols"]       = t["size"]          # fixed 1-D grid: allocation == size
            t["_ini_dims"]       = f"[{t['size']}]"
            t["_resizable_table"] = False
    else:                                              # bare/fixed 1D axis
        t["_alloc_elems"]    = t.get("max_size", t["size"])
        t["_live_elems"]     = t["size"]
        t["_ini_dims"]       = f"[{t['size']}]"
        t["_resizable_table"] = False


def align_float_config(prim_types: dict, modules: dict) -> None:
    """EcuConfig is byte-packed (#pragma pack(1)), but Cortex-M7 VLDR faults on an unaligned float load
    (unlike integer LDR/LDRH, which the hardware fixes up). So every FLOAT config member must start at a
    4-byte-aligned offset. This replays the config layout in collect_offsets' exact order and inserts
    synthetic uint8 padding wherever a float would land unaligned. Every downstream emitter walks the
    same lists in order, so the pads flow through the struct / serializer / .ini / json / meta
    consistently. Idempotent: drops its own pads and re-derives them.

    Four places need it, and only the second existed before a float first appeared inside a config array:

      1. before a float SCALAR in a module's config
      2. before a float TABLE (or a float axis)
      3. before an ARRAY SLOT whose element contains a float — the slot base itself must be aligned
      4. before a float MEMBER inside an element, AND after the last member so the element STRIDE is a
         multiple of 4. The stride matters as much as the base: element N+1 starts at base + N*stride,
         so an odd stride misaligns every element after the first. The etb[] element was 533 bytes.

    Must run AFTER annotate_resizable (tables sized, _n scalars added)."""
    pad_seq = 0

    def is_float(typ) -> bool:
        return typ in prim_types and prim_types[typ]["c_type"] == "float"

    def pad_scalars(k: int) -> list:
        """k one-byte scalar pads. Scalars have no size field — a k-byte pad is k uint8 fields."""
        nonlocal pad_seq
        out = []
        for _ in range(k):
            out.append({"name": f"_align_pad_{pad_seq}", "type": "uint8", "label": "(alignment padding)",
                        "units": "", "scale": 1.0, "min": 0, "max": 0, "default": 0,
                        "help": "Padding so the float that follows is 4-byte aligned.",
                        "_align_pad": True})
            pad_seq += 1
        return out

    def pad_table(k: int) -> dict:
        nonlocal pad_seq
        t = {"name": f"_align_pad_{pad_seq}", "type": "uint8", "size": k,
             "label": "(alignment padding)", "_align_pad": True,
             "_alloc_elems": k, "_live_elems": k, "_ini_dims": f"[{k}]", "_resizable_table": False}
        pad_seq += 1
        return t

    off = 4  # after layout_hash (uint32, field 0)
    for mod_name, mod in modules.items():
        if mod_name == "System":
            continue

        # 1 — module scalars
        new_cfg = []
        for f in mod.get("config", []):
            if f.get("_align_pad"):                    # a pad from a previous run — drop, re-derive
                continue
            if is_float(f.get("type")) and (off % 4):
                k = 4 - (off % 4)
                new_cfg.extend(pad_scalars(k)); off += k
            new_cfg.append(f)
            off += field_size(prim_types, f)
        mod["config"] = new_cfg

        # 2 — tables
        new_tables = []
        for t in mod.get("tables", []):
            if t.get("_align_pad"):
                continue
            if t.get("_learned"):                      # no config bytes: nothing to align or advance
                new_tables.append(t)
                continue
            if is_float(t["type"]) and (off % 4):
                k = 4 - (off % 4)
                new_tables.append(pad_table(k)); off += k
            new_tables.append(t)
            off += table_size(prim_types, t)

        # 3 + 4 — array slots, their bases, and their element strides
        for slot in array_union_slots(mod.get("config_arrays", [])):
            for arr in slot:                           # drop previous element pads first, both lists
                arr["element"] = [f for f in arr["element"] if not f.get("_align_pad")]
                if arr.get("_element_layout") is not None:
                    arr["_element_layout"] = [it for it in arr["_element_layout"]
                                              if not (it.get("kind") == "scalar"
                                                      and it["field"].get("_align_pad"))]
            slot_needs = any(is_float(f.get("type")) for arr in slot for f in arr["element"])
            if slot_needs and (off % 4):               # the slot BASE must be aligned
                k = 4 - (off % 4)
                new_tables.append(pad_table(k)); off += k
            for arr in slot:
                if not any(is_float(f.get("type")) for f in arr["element"]):
                    continue
                # The element is described TWICE: `element` is the flat byte order and
                # `_element_layout` is the emit plan the struct is generated from (a table field
                # desugars into one plan item covering several flat fields). They must stay in step —
                # padding only `element` moves every offset in the meta while leaving the C struct
                # unchanged, which is a silent layout divergence the size assertions then catch.
                new_elem, eoff, before, by_name = [], 0, {}, {}
                for f in arr["element"]:
                    if is_float(f.get("type")) and (eoff % 4):
                        pads = pad_scalars(4 - (eoff % 4))
                        before[id(f)] = pads
                        # …AND BY NAME. An element TABLE desugars into several flat fields (its float
                        # axes among them), and the plan below carries the table as ONE item whose
                        # `field` object is not the flat field the pad was recorded against. Keyed by
                        # name too, the plan can find the pad that belongs in front of it — without
                        # which the meta grew three bytes the C struct did not, and the element's own
                        # size assertion is what caught it.
                        by_name[f.get("name")] = pads
                        new_elem.extend(pads); eoff += len(pads)
                    new_elem.append(f)
                    eoff += field_size(prim_types, f)
                tail = pad_scalars(4 - (eoff % 4)) if (eoff % 4) else []   # STRIDE: element N+1 too
                new_elem.extend(tail)
                arr["element"] = new_elem
                plan = arr.get("_element_layout")
                if plan is not None:
                    new_plan = []
                    for item in plan:
                        pads = before.get(id(item.get("field")), []) if item.get("field") else []
                        if not pads:
                            # A non-scalar plan item groups MANY flat fields — an axis expands to one
                            # scalar per breakpoint ("duty_table_x_axis_0", "_1" …) — and the pad was
                            # recorded against the FIRST of them. Look it up by that member's name:
                            # matching on the handle alone finds nothing, which is how three bytes
                            # ended up in the meta and not in the C struct.
                            first = None
                            if item.get("members"):          # prim_array
                                first = item["members"][0].get("name")
                            elif item.get("members_by_idx"): # struct_array
                                row = item["members_by_idx"][0]
                                first = row[0].get("name") if isinstance(row, list) and row else None
                            if first and first in by_name: pads = by_name.pop(first)
                        for pad in pads:
                            new_plan.append({"kind": "scalar", "field": pad})
                        new_plan.append(item)
                    for pad in tail:
                        new_plan.append({"kind": "scalar", "field": pad})
                    arr["_element_layout"] = new_plan
            off += array_slot_size(prim_types, slot)

        mod["tables"] = new_tables


def assert_config_alignment(prim_types: dict, modules: dict) -> None:
    """Build-time guarantee: every float config member sits at a 4-byte-aligned offset (so the M7
    never VLDR-faults on it). Raises if align_float_config missed a case — fail loud at codegen, not
    as a hard fault on the bench."""
    _, scalars, tables, arrays, _, _ = collect_offsets(prim_types, modules)
    def is_float(typ):    # enum/bool/string aren't primitives (and never float) — skip safely
        return typ in prim_types and prim_types[typ]["c_type"] == "float"
    bad = []
    for _const, typ, off, t, mod in tables:
        if is_float(typ) and off % 4:
            bad.append(f"{mod}.{t['name']} (table) @ {off}  off%4={off % 4}")
    for _const, typ, off, f, mod in scalars:
        if is_float(typ) and off % 4:
            bad.append(f"{mod}.{f['name']} (scalar) @ {off}  off%4={off % 4}")
    for entry in arrays:
        typ, off, f, mod = entry[1], entry[2], entry[3], entry[4]
        if is_float(typ) and off % 4:
            bad.append(f"{mod}.{f['name']} (array elem) @ {off}  off%4={off % 4}")
    if bad:
        raise SystemExit("Unaligned float config member(s) — Cortex-M7 VLDR will hard-fault:\n  "
                         + "\n  ".join(bad)
                         + "\n(align_float_config should have padded these; check the layout pass.)")

# -----------------------------------------------------------------------
# Signal IDs — generated/signal_ids.h
# -----------------------------------------------------------------------

# Non-channel SignalId/SignalType members that gen_signal_ids_h emits alongside the per-signal
# SIG_<id> constants (sentinels + the bus-type tag enum). They are always defined.
_SIG_SENTINELS = {"SIG_NONE", "SIG_COUNT", "SIG_T_FLOAT", "SIG_T_U32", "SIG_T_I32"}
_SIG_TOKEN  = re.compile(r"\bSIG_[A-Z][A-Z0-9_]*")
_C_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
_FW_SRC_SUFFIXES = (".h", ".hpp", ".cpp", ".cc", ".inc")


def validate_trigger_wheels(schema: dict) -> None:
    """A gap wheel's FIRST gap index must be 0 — the reference is the first tooth after SYNC.

    This is specific to the GAP primitive, because the gap is the only event a missing-tooth wheel
    has that the decoder can identify: GapMatcher establishes position there and nowhere else, so the
    tooth ending the gap is the reference. A non-zero first index does not move where sync happens,
    it only renames that tooth and drags the angle origin backwards onto one that cannot be
    identified at all. The wheel then carries a hidden `index * tooth_angle` offset on top of
    trigger_offset_btdc, which already exists to state the wheel-to-engine relationship — two knobs
    for one quantity, one of them invisible.

    The other primitives sync differently and are not constrained here: a SEQUENCE stream's ratio
    pattern names every event so it can sync anywhere, and a WIDTH stream syncs on its reference
    pulse and takes the authored target angle.

    A 2JZ 36-2 shipped as gaps:[6], putting its origin 80 crank degrees before the tooth the decoder
    actually syncs on. Numbering a wheel any other way is unusual: the post-gap tooth is the
    conventional origin precisely because it is the only one that can be identified.

    Later indices are unconstrained — on a multi-gap wheel (36-2-2-2 is [0, 1, 14]) they are the
    pattern, and the decoder needs them to place itself once it knows which gap it matched.
    """
    bad = []
    for w in schema.get("trigger_wheels", []) or []:
        for i, st in enumerate(w.get("streams", []) or []):
            if st.get("kind") != "gap":
                continue
            gaps = st.get("gaps") or []
            if gaps and gaps[0] != 0:
                bad.append(f"{w.get('name', '?')} stream[{i}]: gaps {gaps} — first index must be 0")
    if bad:
        sys.exit("ERROR: trigger wheel gap index —\n  " + "\n  ".join(bad) +
                 "\n  The reference is the first tooth after SYNC; on a gap wheel sync is the gap,"
                 "\n  so the first gap index is 0. Express the wheel-to-engine relationship in"
                 "\n  trigger_offset_btdc instead.")


def validate_lua_api(schema: dict) -> None:
    """Keep schema `lua_api.functions` in lock-step with firmware ECU_API (the autocomplete source of
    truth) — fail the build if either side adds/removes/renames a function the other lacks. The studio
    editor binds to the schema's signatures/docs; this guard stops them silently going stale."""
    documented = {f["name"] for f in (schema.get("lua_api") or {}).get("functions", [])}
    src = (REPO_ROOT / "firmware" / "Scripting" / "ScriptEngine.cpp").read_text(errors="replace")
    m = re.search(r"ECU_API\[\]\s*=\s*\{(.*?)\}\s*;", src, re.DOTALL)
    if not m:
        sys.exit("ERROR: could not find the ECU_API table in ScriptEngine.cpp for lua_api validation.")
    registered = set(re.findall(r'\{\s*"([A-Za-z_]\w*)"\s*,', m.group(1)))
    missing = sorted(registered - documented)   # in firmware, undocumented
    stale   = sorted(documented - registered)   # documented, not in firmware
    if missing or stale:
        parts = []
        if missing:
            parts.append("in firmware ECU_API but undocumented in lua_api.functions: " + ", ".join(missing))
        if stale:
            parts.append("in lua_api.functions but not in firmware ECU_API: " + ", ".join(stale))
        sys.exit("ERROR: lua_api drift —\n  " + "\n  ".join(parts) +
                 "\n  Fix: keep definition/ecu.schema.yaml lua_api.functions == firmware ECU_API.")


# Cell scalar types a learned block may declare, and their widths. float is the default and what every
# block was before; int16 exists so a trim can be shaped like the table it corrects without the region
# doubling past the RAM that is actually free.
_LEARNED_WIDTH = {"float": 4, "int16": 2}


def learned_layout(schema: dict):
    """Pack schema `learned.blocks` into FIXED byte offsets within the RAM-backed learned region — the exact
    parallel of the EcuConfig layout. Blocks are laid out in declaration order (APPEND-ONLY: a new block
    goes at the end so existing offsets, hence persisted learned data, never move). Each block is just its
    cells (row-major, in the block's declared scalar type) — validity is the WHOLE-region SD totem header
    (magic+crc32+layout_hash), so no per-block magic. Returns (blocks_with_offset_size_cells, region_used, region_cap). This single
    computation feeds BOTH generated/learned_layout.h (firmware) and the meta (studio), so the firmware and
    studio can never disagree on where a learned table lives."""
    learned = schema.get("learned") or {}
    cap = int(learned.get("region_cap", 65536))
    out, off = [], 0
    for b in learned.get("blocks", []):
        rows, cols = int(b.get("rows", 1)), int(b.get("cols", 1))
        cells = rows * cols * int(b.get("depth", 1))
        # A BLOCK DECLARES ITS CELL WIDTH. Everything here was f32 because the first learned tables were
        # small enough that it never mattered. A trim shaped like the map it corrects is not: at the VE
        # table's allocation an f32 overall LTFT alone is 16 KB, against a RAM region with about 10 KB
        # spare. int16 at 0.01 % holds +-327 %, which is six times the widest authority the config allows,
        # so the resolution is free and the halving is not.
        size  = cells * _LEARNED_WIDTH[b.get("type", "float")]
        nb = dict(b)
        nb["offset"], nb["size"], nb["cells"] = off, size, cells
        out.append(nb)
        off += (size + 3) & ~3                               # keep 4-byte alignment for the next block
    if off > cap:
        sys.exit(f"ERROR: learned region exceeds the append ceiling — {off} B used > {cap} B cap. "
                 "Reduce a block's rows*cols or raise learned.region_cap.")
    return out, off, cap


def gen_learned_layout_h(schema: dict) -> str:
    """generated/learned_layout.h — fixed offsets/dims for the RAM-backed learned region.
    The firmware overlays these on platform_learned_base() (a plain RAM buffer sized to REGION_USED,
    persisted to SD totem files); the studio reaches the same bytes via the config r/w block protocol
    at (learned.page_base + OFFSET)."""
    blocks, used, cap = learned_layout(schema)
    page_base = int((schema.get("learned") or {}).get("page_base", "0x40000000"), 16)
    L = [BANNER, "#pragma once", "#include <cstdint>", "",
         "// RAM-backed learned region (LTFT/LTT) — a flat, codegen-assigned layout, the exact parallel of",
         "// EcuConfig. APPEND-ONLY: new blocks go at the END so existing offsets (hence the persisted learned",
         "// data) never move across a firmware update. At runtime the firmware overlays these on a plain RAM",
         "// buffer (platform_learned_base(), sized to REGION_USED); the buffer is persisted to rotating SD",
         "// totem files (LearnedStore) and reloaded at boot. The studio reads/writes the same live RAM bytes",
         "// via the config r/w block protocol at (learned.page_base + OFFSET). Each block is CELLS cells of",
         "// its declared scalar type (BYTES total)",
         "// (cell = row*COLS + col) starting at OFFSET — no per-block magic; the whole region's integrity is",
         "// the totem header (magic + crc32 + layout_hash), just like a config bank.", "",
         "// Comms device base: the config r/w protocol routes an absolute offset >= LEARNED_PAGE_BASE to this",
         "// region (offset - LEARNED_PAGE_BASE within the RAM buffer); the studio meta uses the same base.",
         f"static constexpr uint32_t LEARNED_PAGE_BASE  = 0x{page_base:08X}u;",
         f"static constexpr uint32_t LEARNED_REGION_CAP = {cap}u;   // append ceiling (sanity bound), NOT the buffer size"]
    for b in blocks:
        u = b["id"].upper()
        L += ["",
              f"static constexpr uint32_t LEARNED_{u}_OFFSET = {b['offset']}u;   // {b.get('label','')}",
              f"static constexpr uint32_t LEARNED_{u}_ROWS   = {int(b.get('rows', 1))}u;",
              f"static constexpr uint32_t LEARNED_{u}_COLS   = {int(b.get('cols', 1))}u;",
              f"static constexpr uint32_t LEARNED_{u}_CELLS  = {b['cells']}u;",
              f"static constexpr uint32_t LEARNED_{u}_BYTES  = {b['size']}u;   "
              f"// {b.get('type', 'float')} cells"]
    L += ["",
          "// The RAM buffer + every SD totem payload is exactly this many bytes.",
          f"static constexpr uint32_t LEARNED_REGION_USED = {used}u;",
          'static_assert(LEARNED_REGION_USED <= LEARNED_REGION_CAP, "learned region exceeds the append ceiling");',
          ""]
    return "\n".join(L)


# `EGT_SIGNALS[k]` / `STFT_CYL_SIGNALS[c]` — the generated per-family id arrays the firmware indexes.
_SIG_FAMILY_TOKEN = re.compile(r"\b([A-Z][A-Z0-9_]*)_SIGNALS\s*\[")

def validate_firmware_signal_refs(signals: list, well_known: dict, sensors: list | None = None) -> None:
    """Pre-build guard enforcing the firmware↔schema signal contract — firmware usage is the
    authority (the "ownership inversion"):
      (1) every `SIG_<X>` token in hand-written firmware resolves to a generated SignalId — a
          rename/removal is caught HERE (precise file:line) instead of as a post-flash C++ error;
      (2) every signal the firmware owns (referenced raw as SIG_<id>, or bound to a wk::<role>) is
          declared `owner: firmware` in the schema — so the studio locks its id and it can't be
          renamed out from under the C++;
      (3) a soft warning for `owner: firmware` signals nothing references (a stale lock).
    Comments are stripped so doc text like `// SIG_FOO_* channels` never trips it."""
    by_id   = {s["id"]: s for s in signals}
    defined = {f"SIG_{i.upper()}" for i in by_id} | _SIG_SENTINELS
    fw_dir = REPO_ROOT / "firmware"
    bad, used_ids, fam_ids = [], set(), set()
    for path in sorted(fw_dir.rglob("*")):
        if path.suffix not in _FW_SRC_SUFFIXES:
            continue
        src = _C_BLOCK_COMMENT.sub(lambda m: "\n" * m.group(0).count("\n"),
                                   path.read_text(errors="replace"))
        for lineno, line in enumerate(src.splitlines(), 1):
            code = line.split("//", 1)[0]
            for m in _SIG_TOKEN.finditer(code):
                sym = m.group(0)
                if sym not in defined:
                    bad.append((path.relative_to(REPO_ROOT), lineno, sym))
                elif sym not in _SIG_SENTINELS:
                    used_ids.add(sym[4:].lower())
            # The firmware reaches a whole per-cylinder family through its GENERATED array --
            # `bus.set(STFT_CYL_SIGNALS[c], ...)`, `bus.get(EGT_SIGNALS[k], ...)` -- and never names
            # SIG_STFT_CYL_1. A token scan therefore saw the family's FIRST member (the schema template,
            # the only one not marked per_cyl_gen) as an unreferenced stale lock, every build, for
            # signals the firmware demonstrably publishes.
            for fam in _SIG_FAMILY_TOKEN.finditer(code):
                fam_ids.add(fam.group(1).lower())
    if bad:
        msg = "\n  ".join(
            f"{p}:{ln}: '{sym}' — no signal id '{sym[4:].lower()}' in the schema (renamed/removed?)"
            for p, ln, sym in bad)
        sys.exit("ERROR: firmware references undefined SignalId(s):\n  " + msg +
                 "\n  Fix: restore the id, or route the reference through a wk::<role> in "
                 "well_known_signals:.")

    # (2) ownership: firmware-owned = referenced raw OR bound to a wk:: role.
    owned = used_ids | set((well_known or {}).values())
    undeclared = sorted(i for i in owned if i in by_id and by_id[i].get("owner") != "firmware")
    if undeclared:
        sys.exit("ERROR: signals referenced by firmware are not declared `owner: firmware`:\n  "
                 + "\n  ".join(undeclared) +
                 "\n  Fix: add `owner: firmware` to each (the studio then locks the id).")
    # Members of any family the firmware reached through its generated <FAMILY>_SIGNALS array count as
    # referenced: `EGT_SIGNALS` covers egt_1..egt_N.
    for fam in fam_ids:
        owned |= {i for i in by_id if i == fam or i.startswith(fam + "_")}

    # A SENSOR'S OWN CHANNEL IS PUBLISHED BY GENERATED CODE, and this scan only reads firmware/ — so a
    # sensor nothing names by SIG_<id> looked stale while its pipeline published it every frame. tps_2 is
    # the case in point: the ETB reads throttle track B through tps_b_src, a CONFIGURABLE source that
    # merely DEFAULTS to it, so no line of firmware ever spells the id. A sensor id (and the secondary
    # outputs hanging off it) counts as referenced.
    for sen in (sensors or []):
        sid = sen.get("id")
        if sid:
            owned |= {i for i in by_id if i == sid or i.startswith(sid + "_")}

    # (3) stale locks: declared firmware-owned but nothing in firmware uses it. Per-cylinder-generated
    # signals (lambda_2..N / egt_2..N) share the template's owner:firmware but aren't referenced yet —
    # sensor-produced, not stale — so skip them. `bus_producer: false` signals are skipped too: the comms
    # layer writes them STRAIGHT into the telemetry frame (config_gen is its own write counter, command
    # state is that layer's status word), so they have no bus producer by design and never will.
    stale = sorted(s["id"] for s in signals
                   if s.get("owner") == "firmware" and s["id"] not in owned
                   and not s.get("per_cyl_gen") and s.get("bus_producer", True))
    if stale:
        print("  WARNING: owner: firmware signals with no firmware reference (stale lock?): "
              + ", ".join(stale))


# -----------------------------------------------------------------------
# Signal-ID lock — definition/signal_ids.<board>.lock
# -----------------------------------------------------------------------

# THE LOCK IS PER BOARD, because a large part of the catalog is board-DERIVED: there is one
# hw_av<n> per analog pin, four hw_dig<n>_* per digital pin, and one out_<n> per output row.
# A board with 11 analog inputs where another has 15 does not merely renumber those — it has
# FEWER OF THEM, which the lock's remove-is-an-error rule correctly refuses. Sharing one file
# between boards would mean either a permanent hard error or deleting one board's ids to build
# another, and deleting them is exactly the silent selector re-pointing the lock exists to stop.
#
# Per board, the guarantee the lock actually makes is restored and made stronger: an id is
# stable for the board whose tunes store it, and no board can perturb another's. It costs
# nothing, because a tune is already board-specific (the output-row count alone changes the
# config layout, so the layout_hash differs anyway).
def signal_lock_path(board: str | None) -> Path:
    return REPO_ROOT / "definition" / f"signal_ids.{board or 'default'}.lock"

def apply_signal_id_lock(signals: list, board: str | None = None) -> list:
    """Pin every signal's SignalId so adding one cannot renumber the others.

    A SignalId is the channel's INDEX in this catalog, and a tune stores selector fields
    (kind:"signal" — tps_a_src, demand_sig, every table's *_x_src) as that raw number. So
    inserting a signal in the middle of the schema silently re-points every selector above it
    at its neighbour. layout_hash cannot catch it: the hash covers the config BYTE layout, and
    not one byte moves — the bytes keep their meaning while the ids underneath them change.

    That is not hypothetical. Adding `app_state` next to `pedal_demand` shifted 179 ids by one,
    and a tune written before it bound half-bridge A's enable to etb_en_2 instead of etb_en_1
    afterwards. The bridge simply never enabled: no error, no DTC, no failed validation — a
    throttle that would not move, which reads exactly like dead hardware.

    The lock is the fix, and it fixes the CAUSE rather than detecting the symptom: a checked-in
    name -> id map that codegen honours and only ever appends to. Declare a signal wherever it
    reads best in the schema; its id comes from here. New signals take the next free id, so
    existing tunes stay valid across the addition.

    REMOVING a signal is the one thing that cannot be absorbed silently, because it would close
    the gap and shift everything after it. That is a hard error telling you to retire the id
    explicitly — deleting the lock line is a deliberate act that invalidates stored tunes, not
    something to discover on a bench at midnight.
    """
    SIGNAL_LOCK = signal_lock_path(board)
    names = [s["id"] for s in signals]
    dupes = sorted({n for n in names if names.count(n) > 1})
    if dupes:
        sys.exit(f"ERROR: duplicate signal id(s) in the catalog: {', '.join(dupes)}")

    lock: dict = {}
    if SIGNAL_LOCK.exists():
        lock = {k: int(v) for k, v in json.loads(SIGNAL_LOCK.read_text())["ids"].items()}

    live = set(names)
    retired = [n for n in lock if n not in live]
    if retired:
        sys.exit(
            "ERROR: signal(s) locked to an id but no longer in the catalog: "
            + ", ".join(sorted(retired))
            + "\n       Removing a signal shifts every id above it, which silently re-points every"
            + "\n       stored tune's selectors. If that is really intended, delete the entr" +
            ("ies" if len(retired) > 1 else "y") + " from"
            + f"\n       {SIGNAL_LOCK.relative_to(REPO_ROOT)} in the same commit and re-run — every tune"
            + "\n       written before that point must then be re-checked.")

    next_id = (max(lock.values()) + 1) if lock else 0
    added = []
    for n in names:                       # schema order, so a batch of new signals stays readable
        if n not in lock:
            lock[n] = next_id
            added.append((n, next_id))
            next_id += 1

    ids = sorted(lock.values())
    if ids != list(range(len(ids))):      # a hole would break index == id everywhere downstream
        sys.exit(f"ERROR: signal ids are not contiguous after locking: {SIGNAL_LOCK}")

    by_name = {s["id"]: s for s in signals}
    ordered = [by_name[n] for n, _ in sorted(lock.items(), key=lambda kv: kv[1])]

    SIGNAL_LOCK.parent.mkdir(exist_ok=True)
    SIGNAL_LOCK.write_text(json.dumps(
        {"_comment": "SignalId lock — index into the signal catalog, stored raw in every tune's "
                     "kind:'signal' selector. Codegen APPENDS only; ids never move. See "
                     "apply_signal_id_lock() in codegen/codegen.py.",
         "ids": {n: i for n, i in sorted(lock.items(), key=lambda kv: kv[1])}},
        indent=2) + "\n")
    if added:
        print(f"  signal ids: {len(added)} new -> " +
              ", ".join(f"{n}={i}" for n, i in added[:6]) + (" ..." if len(added) > 6 else ""))
    return ordered


def gen_signal_ids_h(signals: list) -> str:
    guard = "JAYECU_GENERATED_SIGNAL_IDS_H"
    type_map = {"float": "SIG_T_FLOAT", "uint32": "SIG_T_U32", "int32": "SIG_T_I32"}
    # SignalId is uint16 and config selectors are uint16 too (unset = SIG_NONE = 0xFFFF), so ids are
    # CONTIGUOUS 0..N-1 — no reserved hole. (Previously id 255 was reserved so uint8 selectors could
    # store 255 = unset; widening the selectors to uint16 retired that constraint and lets the catalog
    # grow past 255 for per-cylinder wideband/EGT signals.)
    def sid(i):
        return i

    enum_lines = [f"    SIG_{sig['id'].upper()} = {sid(i)}," for i, sig in enumerate(signals)]
    bound = len(signals)   # contiguous ids -> array size == signal count
    # Per-id tables, sized to `bound` and indexed by the ACTUAL SignalId, with a dummy at any reserved
    # hole (255) so index == id everywhere (id_by_name / SIGNAL_TYPES[s] rely on that).
    names = ['""'] * bound
    types = ["SIG_T_FLOAT"] * bound
    isenum = ["0"] * bound
    for i, sig in enumerate(signals):
        names[sid(i)]  = f'"{sig["id"]}"'
        types[sid(i)]  = type_map.get(sig.get("bus_type", "float"), "SIG_T_FLOAT")
        isenum[sid(i)] = "1" if sig.get("enum") else "0"
    name_lines    = [f"    {v}," for v in names]
    type_lines    = [f"    {v}," for v in types]
    is_enum_lines = [f"    {v}," for v in isenum]

    # Per-cylinder signal groups (wideband/EGT replicated to _1.._N) — a SignalId[] per group so a
    # consumer can iterate every cylinder's sensor (e.g. EGT protection over the hottest). Order = id order.
    groups: dict = {}
    for s in signals:
        # Per-SLOT groups ride the same array (out_1..out_N, one per output slot) — the firmware
        # iterates them exactly the way it iterates the per-cylinder ones, so there is no reason for a
        # second mechanism.
        g = s.get("per_cyl_group") or s.get("per_slot_group")
        if g:
            groups.setdefault(g, []).append(s["id"])
    group_lines = []
    for g in sorted(groups):
        ids = groups[g]
        gu = g.upper()
        group_lines += [
            "",
            f"// {g}: one per element ({len(ids)}) — iterate the group.",
            f"static constexpr uint16_t {gu}_SIGNAL_COUNT = {len(ids)};",
            f"static constexpr SignalId {gu}_SIGNALS[{gu}_SIGNAL_COUNT] = {{ "
            + ", ".join(f"SIG_{i.upper()}" for i in ids) + " };",
        ]

    count = bound   # SIG_COUNT is the slot-array bound (spans the reserved hole), NOT len(signals)
    lines = [
        BANNER,
        "#pragma once",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        "#include <cstdint>",
        "",
        "// ---------------------------------------------------------------------------",
        "// SignalId — index into SignalBus::slots[].",
        "//",
        "// Values are stable within a firmware build; do not reorder without",
        "// rebuilding firmware and reflashing all config pages.",
        "//",
        "// SIG_NONE (0xFFFF) is the unassigned sentinel. Config signal selectors are int16 with -1 = unset;",
        "// a valid id is >= 0, and -1 cast to SignalId is 0xFFFF = SIG_NONE (bus.get returns the fallback).",
        "// ---------------------------------------------------------------------------",
        "",
        "enum SignalId : uint16_t {",
    ] + enum_lines + [
        f"    SIG_COUNT = {count},",
        "    SIG_NONE  = 0xFFFF,   // unassigned sentinel; int16 config selectors store -1 (casts to this).",
        "};",
        "",
        f"static constexpr uint16_t SIGNAL_COUNT = {count};",
        "",
        "// Human-readable names — index matches SignalId value.",
        "// Used by Lua signalRead()/signalWrite() and debug logging.",
        f"static const char* const SIGNAL_NAMES[{count}] = {{",
    ] + name_lines + [
        "};",
        "",
        "// Per-channel bus cell type (a formal declaration — the mutable slot carries no tag).",
        "// Type-erased readers (Lua / by-name) and the telemetry packer use this to interpret",
        "// each cell; compile-time typed accessors don't need it. float is the analog default.",
        "enum SignalType : uint8_t { SIG_T_FLOAT = 0, SIG_T_U32 = 1, SIG_T_I32 = 2 };",
        "",
        f"static const uint8_t SIGNAL_TYPES[{count}] = {{",
    ] + type_lines + [
        "};",
        "",
        "// 1 for signals that carry a closed value-set (schema `enum:`). Preconditions on an enum",
        "// signal compare against the RAW enum code (the value's low byte), not the x100 threshold",
        "// scale — the TS value picker stores a clean enum index. See Condition.h test_eval.",
        f"static const uint8_t SIGNAL_IS_ENUM[{count}] = {{",
    ] + is_enum_lines + [
        "};",
    ] + group_lines + [
        "",
        f"#endif // {guard}",
        "",
    ]
    return "\n".join(lines)


def gen_signal_enums_h(schema: dict) -> str:
    """generated/signal_enums.h — firmware `enum class`es generated from schema enums listed in
    `enum_cpp:`. The firmware INCLUDES and uses these (they replace the old hand-rolled definitions),
    so a published value is by construction the schema's value — the same value-set that drives the
    studio's precondition pickers. One source, no drift."""
    guard = "JAYECU_GENERATED_SIGNAL_ENUMS_H"
    cpp_map = schema.get("enum_cpp") or {}
    lines = [
        BANNER, "#pragma once", f"#ifndef {guard}", f"#define {guard}", "",
        "#include <cstdint>", "",
        "// Schema-sourced state enums (schema: enums + enum_cpp). DO NOT redefine these in firmware;",
        "// include this header and use the generated type so values can never diverge from the TS",
        "// dropdown labels.", "",
    ]
    for eid, cpp in cpp_map.items():
        pairs = enum_pairs(schema, eid)
        if not pairs:
            raise SystemExit(f"enum_cpp.{eid}: no such enum in schema['enums']")
        lines.append(f"enum class {cpp} : uint8_t {{")
        lines += [f"    {pid.upper()} = {i}," for i, (pid, _lbl) in enumerate(pairs)]
        lines += ["};", ""]
    lines += [f"#endif // {guard}", ""]
    return "\n".join(lines)


def gen_well_known_signals_h(well_known: dict, signals: list) -> str:
    """generated/well_known_signals.h — stable semantic ROLES → the SignalId currently bound to
    each, from the schema's `well_known_signals:` map. Firmware references the role (`wk::map`),
    NEVER the renameable `SIG_<id>`; renaming a signal just rebinds the role here, so no .cpp ever
    has to change. THIS is how a signal rename stays a pure-schema operation."""
    guard = "JAYECU_GENERATED_WELL_KNOWN_SIGNALS_H"
    ids = {s["id"] for s in signals}
    lines = [
        BANNER, "#pragma once", f"#ifndef {guard}", f"#define {guard}", "",
        '#include "signal_ids.h"', "",
        "// Semantic role -> currently-bound signal (schema: well_known_signals). The role name is",
        "// STABLE firmware vocabulary; the binding follows the schema, so renaming a signal needs",
        "// zero firmware edits — codegen just emits a new SIG_* on the right of the constexpr.",
        "namespace wk {",
    ]
    for role, sigid in (well_known or {}).items():
        if sigid not in ids:
            raise SystemExit(f"well_known_signals.{role}: '{sigid}' is not a signal id")
        lines.append(f"    constexpr SignalId {role} = SIG_{sigid.upper()};")
    lines += ["}  // namespace wk", "", f"#endif // {guard}", ""]
    return "\n".join(lines)


def gen_well_known_telem_h(well_known: dict, telem_members: set) -> str:
    """generated/well_known_telem.h — `wkt::<role>(t)` accessors onto the EcuTelemetry struct,
    mapping each stable role to the member currently named after its bound signal. Same idea as
    wk:: but for the telemetry struct, so OBD/comms read `wkt::clt(t)` not the renameable `t.clt`.
    Separate header so the bus-only consumers of wk:: don't pull in the telemetry struct."""
    guard = "JAYECU_GENERATED_WELL_KNOWN_TELEM_H"
    lines = [
        BANNER, "#pragma once", f"#ifndef {guard}", f"#define {guard}", "",
        '#include "ecu_telemetry.h"', "",
        "// Stable role -> the EcuTelemetry member named after its currently-bound signal.",
        "namespace wkt {",
    ]
    for role, sigid in (well_known or {}).items():
        if sigid not in telem_members:
            continue                                  # this role's signal isn't a telemetry channel
        lines.append(f"    inline auto&       {role}(EcuTelemetry& t)       {{ return t.{sigid}; }}")
        lines.append(f"    inline const auto& {role}(const EcuTelemetry& t) {{ return t.{sigid}; }}")
    lines += ["}  // namespace wkt", "", f"#endif // {guard}", ""]
    return "\n".join(lines)

# -----------------------------------------------------------------------
# Sensors — channel derivation + Tier-2 catalog descriptor
# (see docs/sensors-design.md). The channel namespace (old `signals:`) is
# DERIVED from the sensor catalog so it is never hand-maintained.
# -----------------------------------------------------------------------

# ---- schema enums: the single source of truth for closed value-sets --------
def enum_pairs(schema: dict, name: str) -> list:
    """[(id, label)] for schema['enums'][name]; tolerates bare-string entries.

    An id written `false`/`true` in the yaml (bool_active does) is a YAML BOOLEAN, and str()ing one
    gives Python's "False"/"True" — a capitalised id nobody wrote, shipped to the studio to match on.
    Normalise it back to the word in the definition."""
    def ident(v):
        return ("true" if v else "false") if isinstance(v, bool) else v
    out = []
    for e in (schema.get("enums") or {}).get(name, []):
        if isinstance(e, dict):
            out.append((ident(e["id"]), e.get("label", ident(e["id"]))))
        else:
            out.append((str(e), str(e)))
    return out

def enum_ids(schema: dict, name: str) -> list:
    return [i for i, _ in enum_pairs(schema, name)]


def sensor_offered_interfaces(schema: dict, sen: dict) -> list:
    """Which interfaces this sensor MAY be set to, in enum order.

    Two different questions were being answered by one list. A sensor's `interfaces:` says what it is
    NATURALLY read as — it picks the default selector and the ADC front-end — and read as a permission
    list it refuses things the ECU can plainly do: a coolant-flow switch declares [digital,
    analog_voltage] and there are CAN coolant switches. The firmware's CAN path binds a device signal
    to the sensor's CHANNEL without ever asking what kind of sensor it is, and analog / engine-sync /
    frequency / pulse / SENT all decode a raw scalar through the sensor's own cal curve, so which of
    those a given car uses is a fact about the WIRING.

    So: declared, widened by every interface the schema marks `universal`. `locked:` still collapses it
    to one — there the firmware ignores the selector outright and a menu would be a control that does
    nothing. The two non-universal interfaces are the two that cannot be built generically: `on_board`
    (three acquire functions exist and no more) and `digital` (the pin level IS the value, with no
    decode, so a temperature read that way publishes 0 or 1 degrees).
    """
    order = enum_ids(schema, "sensor_interface")
    if sen.get("locked"):
        return [sen["locked"]]
    universal = {e["id"] for e in (schema.get("enums") or {}).get("sensor_interface", [])
                 if isinstance(e, dict) and e.get("universal")}
    allowed = set(sen.get("interfaces") or []) | universal
    return [i for i in order if i in allowed]

_TABLE_OPTS: dict = {}


def table_selector_meta(schema: dict) -> dict:
    """{options, option_ids} for a `control: table` field — every table an id can name.

    ONE registry, two customers. The expression VM already had to turn a table id into a descriptor
    (OP_TABLE / OP_INTERP), and "which table does this output read" is the same question asked by a
    config field instead of by bytecode. So the selector's options ARE the registry, in id order: the
    stored number is the id, the label is what the picker shows, and the id a program compiles against
    and the id a slot stores cannot drift apart because there is only one list.
    """
    key = id(schema)
    if key not in _TABLE_OPTS:
        reg = expr_table_registry(schema["modules"])
        _TABLE_OPTS[key] = {"options": [e["label"] for e in reg],
                            "option_ids": [e["key"] for e in reg]}
    return _TABLE_OPTS[key]


def field_enum_meta(schema: dict, f: dict) -> dict:
    """The app-facing enum presentation for an enum-like config field — {kind, options, enum} or {}.
    Resolves `options_from` (an `enums:` set, e.g. `sensor_interface`; tolerates a stray plural; or a
    top-level catalog list like `signals:`), an inline `type: enum` + `options`, or an `enum:` ref. So
    an enum field works the same whether it's a top-level scalar or a struct-array element."""
    ftype = f.get("type")
    if f.get("control") == "signal":                       # signal-source selector -> dynamic signal picker
        return {"kind": "signal"}
    # A TABLE selector: the number is a table id, so the picker lists tables and the tune stores the
    # one it names by id — never a bare number nobody can read back.
    if f.get("control") == "table":
        # "table_ref", not "table": a path that IS a table already answers "table" all over the studio
        # (that is what a grid editor binds to), and a SELECTOR is the opposite thing — a scalar whose
        # number names one. Two meanings on one word is how a picker comes to be drawn as a spin box.
        return {"kind": "table_ref", **table_selector_meta(schema)}
    # A selector that must name a channel SOMETHING ACQUIRES. Same picker, filtered to sensor-backed
    # channels: pointing a calibrating input at an arbitrary bus signal is how `pedalcal` would come to
    # rewrite an unrelated sensor's calibration (see App::write_cal — it resolves by primary_channel).
    if f.get("control") == "sensor":
        return {"kind": "sensor"}
    if ftype == "bool":                                    # a 0/1 flag -> a toggle (not a number box)
        return {"kind": "bool"}
    if ftype == "enum" and f.get("options"):
        return {"kind": "enum", "options": list(f["options"]), **({"enum": f["enum"]} if f.get("enum") else {})}
    # AN INTEGER THAT NAMES ITS VALUES IS A CHOICE. A plain uint8 with three or more named options
    # (a stage's fuel-pressure mode) left the meta with no option list at all, so the studio drew its
    # dropdown empty. Declaring it `type: enum` instead would fix the meta and move the layout hash —
    # the type is part of it — orphaning every stored tune for a presentation fix. Two options stay as
    # they are: those are the on/off flags, drawn as switches.
    if ftype in ("uint8", "int8", "uint16", "int16") and len(f.get("options") or []) > 2:
        return {"kind": "enum", "options": list(f["options"])}
    of = f.get("options_from")
    if of:
        name = of
        pairs = enum_pairs(schema, name)
        if not pairs and of.endswith("s"):                 # tolerate a plural ref (sensor_interfaces)
            name = of[:-1]; pairs = enum_pairs(schema, name)
        if not pairs and isinstance(schema.get(of), list):  # a top-level catalog list (signals, …)
            pairs = [(c.get("id", str(i)), c.get("label") or c.get("id") or str(i))
                     for i, c in enumerate(schema[of])]
        if pairs:
            # The IDS travel with the labels. A label is for reading; anything that has to RECOGNISE an
            # option (the studio asking whether a sensor's interface is analog before deciding what its
            # cal axis counts in) must match on the id, or a rename in the definition silently changes
            # behaviour downstream. They cost a few bytes and they are already in hand.
            return {"kind": "enum", "options": [lbl for _id, lbl in pairs],
                    "option_ids": [str(_id) for _id, _lbl in pairs], "enum": name}
    if f.get("enum"):
        pairs = enum_pairs(schema, f["enum"])
        return {"kind": "enum", "options": [lbl for _id, lbl in pairs],
                "option_ids": [str(_id) for _id, _lbl in pairs], "enum": f["enum"]}
    return {}

# A `pickers` field's option lists are board pins, and WHICH list applies depends on a sibling enum
# (the sensor `interface`). Map each picker's `options:` name to the board capabilities that supply it,
# each with the FIRMWARE POOL BASE for that capability's index. A base is either a literal or the NAME
# of the capability whose pool length supplies it: the analog temperature pins start where the voltage
# pins end, so AT's base is len(ANALOG_VOLTAGE) — which was written as a literal 16 here and in two
# other places, and is only 16 on a board with 16 voltage inputs. The firmware reads the same number as
# BOARD_ANALOG_T_BASE, so a dropdown offering "AT1" and platform_read_ain_raw() cannot disagree about
# which pin that is. Each option carries its real pool index, so non-assignable pins can be dropped from
# the list without shifting the rest (no "INVALID" placeholder).
PICKER_CAPS = {
    "sensor_source":         [("ANALOG_VOLTAGE", 0), ("ANALOG_TEMP", "ANALOG_VOLTAGE")],
    "sensor_source_digital": [("DIGITAL_INPUT", 0)],
    # The trigger capture pool, base 0 — option index IS the firmware pool index, so the board yaml's
    # TRIGGER_INPUT order and s_board_capture_resources[] are the same list and nothing maps between.
    "trigger_input":         [("TRIGGER_INPUT", 0)],
    # THE USER OUTPUT POOL, base 0. Its order is the board's own pins_with("TACH_OUTPUT") —
    # IGN(12), LS(22), HS(8) — the same order as the output rows (outputs.output[i] IS pin i). PWM on
    # any of them is SOFTWARE (SoftPwm on TIM4), so a generic PWM output is a KIND, not a pin property.
    # The hbridge PWM pins are deliberately absent — board-locked hardware nobody may claim.
    "output_pin":            [("TACH_OUTPUT", 0)],
    # The FIRING pools. EventScheduler claims a cylinder's coil through PinArbiter::Class::IGN and its
    # injector through ::LS, both 0-based into the board's own ordered list — so the option index IS the
    # channel number the schema field stores, exactly as for trigger_input.
    "ignition_out":          [("IGNITION_OUT", 0)],
    "lowside_out":           [("LOWSIDE_OUT", 0)],
}

def field_bits_meta(f: dict) -> dict:
    """Per-BIT-GROUP descriptors for a packed field, so each group is addressable in its own right.

    A `bits:` entry is a named group at [lsb .. lsb+width-1] with its own label and option list — one bit
    for a flag ("Detect Short-to-GND": Off/On), two for a severity ("None/Level 1/Level 2/Level 3"). Without this
    the app only ever saw the containing BYTE, so dragging a sensor's Diagnostics out of the dictionary
    gave you a spin box over a bit mask: typing a number wrote all six checks at once. Emitting the groups
    lets each one bind and render as what it is — a checkbox for a 1-bit flag, a list for a wider group.

    kind is derived, not declared: width 1 -> bool, anything wider with names -> enum.
    """
    bits = f.get("bits") or []
    if not bits:
        return {}
    out = {}
    for b in bits:
        lsb   = int(b.get("lsb", 0))
        width = int(b.get("width", 1))
        opts  = b.get("options") or []
        out[b["name"]] = {
            "bitLo": lsb,
            "bitHi": lsb + width - 1,
            "label": b.get("label", b["name"]),
            "help":  b.get("help", ""),
            "kind":  "bool" if width == 1 else "enum",
            "min": 0,
            "max": (1 << width) - 1,
            **({"options": opts} if opts else {}),
        }
    return {"bits": out}

def field_pickers_meta(f: dict, board) -> dict:
    """For a `pickers` field, emit {picker: {by, sets:[{ifaces, options:[{value,label}]}]}} resolved
    from the board — the app shows the board-pin list matching the sensor's current interface, each
    option keyed by its firmware pool index. `by` names the sibling field whose value selects the set."""
    pk = f.get("pickers")
    if not pk or not board:
        return {}
    caps = _board_pins_by_cap(board)
    sets = []
    for p in pk:
        opts = []
        # An explicit entry ahead of the pool: a channel field stores a SENTINEL for "not assigned"
        # (255), which no board pin can supply. Without it the list could not express the state the
        # field's own default is in, and a cylinder with no coil had to be typed as a number.
        for extra in p.get("extra", []):
            # pin:false — this option names NO hardware. A field sitting on it holds nothing, so it must
            # not be reported as claiming anything: every cylinder with no coil sits on "None", and the app
            # greys a pin that another element holds.
            opts.append({"value": int(extra["value"]), "label": extra["label"], "pin": False})
        for cap, base_spec in PICKER_CAPS.get(p.get("options"), []):
            base = base_spec if isinstance(base_spec, int) else len(caps.get(base_spec, []))
            fixed = _board_fixed_pins(board).get(cap, {})
            for j, label in enumerate(caps.get(cap, [])):
                if label != "INVALID":
                    opts.append({"value": base + j, "label": label})
                elif j in fixed:
                    # A FIXED pin: real hardware, permanently spoken for. It carries its name so
                    # anything DISPLAYING the field can say where the thing is, and `fixed` so nothing
                    # OFFERS it — see _board_fixed_pins().
                    opts.append({"value": base + j, "label": fixed[j], "fixed": True})
        # SHARED: several elements of this array may name the SAME pin through this field, and that is
        # the wiring rather than a clash. A distributor points every cylinder at one coil; wasted spark
        # points each companion pair at one; batch injection fires a bank off one driver. The firmware
        # claims a shared channel once and says nothing (EventScheduler.cpp:135), so the app must not
        # grey it out — a picker that offers each pin once cannot express a distributor at all.
        sets.append({"ifaces": p.get("ifaces", []), "options": opts,
                     **({"shared": True} if p.get("shared") else {})})
    # `by` names the SIBLING field that selects which set applies — a sensor's pin list depends on its
    # interface. A field with a single ungated set has no such sibling, and inventing one meant the app
    # had to evaluate a path that does not exist and hope it read back as a matching gate value. Empty
    # `by` says so outright: one list, always.
    gated = any(s["ifaces"] for s in sets)
    return {"picker": {"by": "interface" if gated else "", "sets": sets}}

def _board_output_pin_count(board: str | None) -> int | None:
    """Number of USER-assignable output pins = IGNITION_OUT + LOWSIDE_OUT + HIGHSIDE_OUT pins.
    jaytek_v1 = 12 + 22 + 8 = 42. The H-bridge PWM pins (PD6/PD3) are deliberately EXCLUDED: they are
    board-LOCKED hbridge hardware owned by the HBridgeControl actuator (DMA-PWM drives them directly),
    just like the DIR/DIS pins — none of the 6 hbridge pins is a user output. None if no board yaml is
    found (caller keeps the schema's literal `count` fallback)."""
    import yaml
    boards = Path(__file__).resolve().parent.parent / "definition" / "boards"
    p = boards / f"{board}.board.yaml"
    if not p.is_file():
        avail = sorted(boards.glob("*.board.yaml"))
        if not avail:
            return None
        p = avail[0]
    data = yaml.safe_load(p.read_text()) or {}
    pins = data.get("pins", [])
    n = lambda cap: sum(1 for pp in pins if cap in (pp.get("caps") or []))
    return n("IGNITION_OUT") + n("LOWSIDE_OUT") + n("HIGHSIDE_OUT")


def _board_output_rows(board: str | None) -> list:
    """The output rows in order — IGNITION_OUT, then LOWSIDE_OUT, then HIGHSIDE_OUT pins, each class in
    board-yaml order — as [(signal name, caps)]. The same order _board_output_pin_count counts, the
    firmware's output pool binds and the `output_pin` picker lists. [] if no board yaml is found."""
    import yaml
    boards = Path(__file__).resolve().parent.parent / "definition" / "boards"
    p = boards / f"{board}.board.yaml"
    if not p.is_file():
        avail = sorted(boards.glob("*.board.yaml"))
        if not avail:
            return []
        p = avail[0]
    pins = (yaml.safe_load(p.read_text()) or {}).get("pins", [])
    rows = []
    for cap in ("IGNITION_OUT", "LOWSIDE_OUT", "HIGHSIDE_OUT"):
        rows += [(pp["signal"], list(pp.get("caps") or [])) for pp in pins if cap in (pp.get("caps") or [])]
    return rows


def _output_row_functions(caps: list) -> list:
    """Indices into outputs.output[].function (None, Ignition, Injector, Generic) a pin can take."""
    out = [0]
    if "IGNITION_OUT" in caps: out.append(1)
    if "LOWSIDE_OUT" in caps:  out.append(2)
    out.append(3)
    return out


def _board_max_cylinders(board: str | None) -> int:
    """Hardware cylinder ceiling = the board's IGNITION_OUT pin count (every cylinder needs a spark
    channel) — the SAME value board_codegen emits as BOARD_MAX_CYLINDERS and the firmware uses for
    MAX_CYLINDERS. Drives the generated per-cylinder wideband/EGT sensor slots, so the count is a
    hardware fact, not a hardcoded 12. Falls back to 12 if no board yaml is found."""
    import yaml
    boards = Path(__file__).resolve().parent.parent / "definition" / "boards"
    p = boards / f"{board}.board.yaml"
    if not p.is_file():
        avail = sorted(boards.glob("*.board.yaml"))
        if not avail:
            return 12
        p = avail[0]
    data = yaml.safe_load(p.read_text()) or {}
    n = sum(1 for pp in (data.get("pins") or []) if "IGNITION_OUT" in (pp.get("caps") or []))
    return n or 12


def expand_per_slot(schema: dict, board: str | None = None) -> None:
    """Replicate a signal marked `per_slot: <module>.<array>` once per element of that config array.

    The output slots are the case this exists for: a slot's state is one channel each, and there are as
    many as the BOARD has output-capable pins — so the count is not something the schema can spell out
    without pinning it to one board. `{n}` in the label is the 1-based index.

    Runs before the id lock is applied, so the generated ids are ordinary catalog entries that lock and
    append like any other.
    """
    sigs = schema.get("signals") or []
    if not any(e.get("per_slot") for e in sigs):
        return
    normalize_module_config(schema)               # the nested config: {arrays: …} form -> config_arrays
    resolve_array_counts(schema, board)           # idempotent; we need the concrete count
    out = []
    for e in sigs:
        ref = e.get("per_slot")
        if not ref:
            out.append(e); continue
        mod_name, arr_name = ref.split(".", 1)
        count, slot_names = 0, []
        for mname, mod in (schema.get("modules") or {}).items():
            if module_snake(mname) != module_snake(mod_name) and mname != mod_name:
                continue
            for arr in (mod.get("config_arrays") or []):
                if arr.get("name") == arr_name:
                    count = int(arr.get("count") or 0)
                    # The array already knows what each slot IS — resolve_array_counts fills
                    # element_labels from the board (the outputs array gets IGN1..LS1..HS1..), which is
                    # what the config rows are titled with. A channel label can use the same name.
                    slot_names = list(arr.get("element_labels") or [])
        if not count:
            sys.exit(f"ERROR: signal '{e['id']}' has per_slot: {ref}, which names no sized config array")
        stem = e["id"][:-2] if e["id"].endswith("_N") else e["id"]
        for i in range(1, count + 1):
            c = {k: v for k, v in e.items() if k != "per_slot"}
            c["id"] = f"{stem}_{i}"
            c["per_slot_group"] = stem
            if "label" in e:
                # {n} is the slot NUMBER; {pin} is what that slot actually is on this board. A channel
                # per output read "Output 1".."Output 42", which names nothing a wiring diagram or a
                # connector does — the pin is IGN1 or LS7 or HS3, and the config row beside it has said
                # so all along. {pin} falls back to the number if the array has no per-slot names.
                lab = e["label"].replace("{n}", str(i))
                if "{pin}" in lab:
                    nm = slot_names[i - 1] if i - 1 < len(slot_names) else str(i)
                    lab = lab.replace("{pin}", nm)
                c["label"] = lab
            if i > 1:
                c["per_cyl_gen"] = True          # published through a generated array, like the per-cyl set
            out.append(c)
    schema["signals"] = out


def expand_per_cylinder(schema: dict, n: int) -> None:
    """Replicate any sensor-catalog / signal entry marked `per_cylinder: true` into id_1..id_k, where k is
    the board's MAX_CYLINDERS (+ an optional `extra:` count) — so wideband O2 and EGT get one configurable
    input + one bus signal per cylinder (each then scoped Overall / Bank / Cylinder by the sensor's
    assign_mode/assign_to). `extra: N` widens the pool past the cylinder count: wideband O2 uses extra:3 so a
    maximal install can dedicate a sensor to each cylinder AND both banks AND overall at once (12+2+1=15).
    The template id is the STEM (lambda -> lambda_1..15, egt -> egt_1..12); name/label get a numeric suffix.
    Both lists expand identically so a sensor and the signal it produces stay id-aligned. Also replicates the
    RAM-backed `learned.blocks` (per_cylinder -> _1.._N, per_bank -> _1_2) so per-scope LTFT tables get a
    codegen-fixed slice each. Runs before producer/consumer validation so the generated ids are real."""
    def expand(lst, label_key):
        out = []
        for e in lst:
            if not e.get("per_cylinder"):
                out.append(e); continue
            slots = max(1, n) + int(e.get("extra", 0))     # cylinders + optional extra scope slots (banks/overall)
            for i in range(1, slots + 1):
                c = {k: v for k, v in e.items() if k not in ("per_cylinder", "extra")}
                c["id"] = f"{e['id']}_{i}"
                c["per_cyl_group"] = e["id"]    # stem (lambda/egt) — lets consumers iterate the group
                if label_key in e:
                    c[label_key] = f"{e[label_key]} {i}"
                if i > 1:
                    c["per_cyl_gen"] = True     # _2..N: owner:firmware but not referenced yet (skip stale-lock warn)
                out.append(c)
        return out
    if schema.get("sensors"):
        schema["sensors"] = expand(schema["sensors"], "name")
    if schema.get("signals"):
        schema["signals"] = expand(schema["signals"], "label")

    # Learned blocks: replicate per-scope LTFT tables. per_cylinder -> _1.._N, per_bank -> _1,_2. Replicated
    # blocks MUST be declared at the END of learned.blocks so the append-only offset assignment keeps every
    # existing block (and its persisted data) in place. id/label get a numeric suffix; the firmware maps each
    # LEARNED_<ID>_<i>_OFFSET onto its own RAM slice.
    learned = schema.get("learned") or {}
    blocks = learned.get("blocks")
    if blocks:
        exp = []
        for b in blocks:
            k = max(1, n) if b.get("per_cylinder") else (2 if b.get("per_bank") else 0)
            if not k:
                exp.append(b); continue
            for i in range(1, k + 1):
                c = {kk: vv for kk, vv in b.items() if kk not in ("per_cylinder", "per_bank")}
                c["id"] = f"{b['id']}_{i}"
                if "label" in b:
                    c["label"] = f"{b['label']} {i}"
                exp.append(c)
        learned["blocks"] = exp


def config_tables(mod: dict) -> list:
    """The module's tables that occupy EcuConfig BYTES.

    A learned table is an ordinary table in every respect but one: its cells live in the RAM-backed
    learned region, not the flashed config struct (see learned_blocks_as_tables). So anything laying
    out, sizing, packing, aligning or shadowing EcuConfig asks for these; anything DESCRIBING tables —
    the meta, the nav tree, axis resolution — takes mod["tables"] whole, because a learned table is
    every bit as much a table there. One predicate, asked where membership of the config image is what
    matters, rather than a second kind of table."""
    return [t for t in mod.get("tables", []) if not t.get("_learned")]


def learned_blocks_as_tables(schema: dict, modules: dict) -> None:
    """A learned block IS a table. Put it in its module so it goes through the SAME pipeline as every
    other one — inline-axis expansion, <axis>_n / _src / _en synthesis, meta emission, the nav tree.

    It was not. `learned.blocks` was declared in its own top-level section and emitted straight into the
    meta by a private branch that wrote {type, datatype, offset, rows, cols} and dropped the axis on the
    floor, so the studio received 53 tables with no axis at all — unresolvable, unviewable, unresettable.
    The bins still had to come from somewhere, so each module grew its own: Lambda and Knock each declared
    RPM_SPAN/LOAD_SPAN (the same two numbers, twice), VvtControl declared CLT_SPAN/CLT_BINS, and each
    hand-rolled `static_cast<int>(v / SPAN * BINS)` instead of calling TableEngine::locate(). Four
    binning schemes and six constants, all restating an axis the schema already declares.

    The ONLY thing that makes a learned block different is where its bytes live: the RAM-backed learned
    region rather than EcuConfig. That is a storage location, not a kind of table — so it is marked
    `_learned` here, skipped by the config-struct offset walk, and given its offset by learned_layout().
    Everything else about it is now indistinguishable from any other table.

    Axes convert as declared:
      {path: config.idle.ltt_clt_axis}      -> a name ref; the existing tunable array is reused, not copied
      {channel: clt, min: -40, max: 140}    -> an inline axis, expanded to a real breakpoint array by
                                               expand_inline_axes() exactly like any other inline axis
    Run after normalize_module_config(), before expand_inline_axes()."""
    learned = schema.get("learned") or {}
    # A block names its module in snake_case ("vvt_control"); `modules` is keyed by the declared name
    # ("VvtControl"). Same mapping the meta uses for config paths, so a block lands in the module a user
    # will find it under.
    by_snake = {module_snake(k): k for k in modules}
    # Tables with the SAME grid share ONE breakpoint array. The per-scope LTFTs are replicas of one
    # another (overall, per bank, per cylinder) and the schema requires them to sit on the same rpm x load
    # grid "so they compose" — giving each its own copy would make that a coincidence that survives only
    # until someone resizes one, and it cost 3.6 KB of config for 39 identical axes. Keyed on the grid
    # itself, so sharing is a consequence of being the same grid rather than a list to maintain.
    shared_axis: dict = {}
    for b in learned.get("blocks", []):
        mod = by_snake.get(b.get("module"), b.get("module"))
        if mod not in modules:
            sys.exit(f"ERROR: learned block '{b['id']}' names module '{b.get('module')}', "
                     f"which does not exist (known: {', '.join(sorted(by_snake))})")
        rows, cols = int(b.get("rows", 1)), int(b.get("cols", 1))
        # BOUNDS ARE RAW, AND A SCALED BLOCK'S RAW IS NOT ITS UNITS. These were a flat -100..100 for
        # every block, which is right only while scale is 1. lambda_ltft is int16 at 0.01, so the raw
        # +/-100 the studio clamps entry against meant a fuel trim could not be typed past +/-1.00%
        # -- and it clamped silently, so the cell simply read -1.00 whatever you entered.
        # +/-100 in the block's OWN units is the intent (a +/-100% trim); express that raw, and never
        # past what the cell type can hold.
        b_scale = float(b["scale"]) if b.get("scale") is not None else 1.0
        lim = 100.0 / b_scale if b_scale else 100.0
        if b.get("type") == "int16":  lim = min(lim, 32767.0)
        elif b.get("type") == "int8": lim = min(lim, 127.0)
        t = {
            "name": b["id"],
            "type": b.get("type", "float"),        # cell scalar in the learned region (LEARNED_*_CELLS)
            "label": b.get("label", b["id"]),
            "units": b.get("units", ""),
            "help": "Learned trim (runtime RAM, persisted to SD; not part of the tune).",
            "min": -lim, "max": lim,
            "_learned": b["id"],                   # -> learned region, NOT EcuConfig (see collect_offsets)
        }
        if b.get("scale") is not None:             # an int16 block stores raw counts; scale gives the units
            t["scale"] = b["scale"]
        # A single row is a 1-D table and is declared the way every other 1-D table is (size/max_size),
        # not as a 2-D map one row tall — otherwise it takes a different path through the storage
        # annotation for no reason other than how it was written down.
        if rows > 1 or "row_axis" in b:
            t["rows"], t["cols"] = rows, cols
            # A learned block may mirror a 3-D table (the fuel trim mirrors ve_table, ethanol plane
            # included). Depth rides the same path as rows/cols rather than a special case.
            if int(b.get("depth", 1)) > 1 or "depth_axis" in b:
                t["depth"] = int(b.get("depth", 1))
        else:
            t["size"], t["max_size"] = cols, cols
        if b.get("digits") is not None:            # float storage that holds whole numbers (a count)
            t["digits"] = b["digits"]
        for key in ("row_labels", "col_labels"):
            if b.get(key):
                t[key] = b[key]
        if b.get("apply_to"):
            t["apply_to"] = b["apply_to"]
            # HOW it folds in: a percentage multiplies the base (the fuel trim), a duty offset adds to it
            # (boost, VVT). The studio did every trim as a multiply, which folded a +5-point duty trim
            # on a 40 % cell into 42 % instead of 45 %.
            mode = b.get("apply_mode", "multiply")
            if mode not in ("multiply", "add"):
                raise SystemExit(f"learned.{b.get('id')}.apply_mode: '{mode}' is not multiply or add")
            t["apply_mode"] = mode
        for src, ax, n in (("col_axis", "x", cols), ("row_axis", "y", rows),
                           ("depth_axis", "z", int(b.get("depth", 1)))):
            spec = b.get(src)
            if not spec:
                continue
            if spec.get("path"):
                # An existing tunable axis array: reference it. Synthesizing a second one would give the
                # same quantity two sets of breakpoints, which is the bug in miniature.
                t[f"{ax}_axis"] = spec["path"].rsplit(".", 1)[-1]
                if spec.get("channel"):
                    t[f"{ax}_channel"] = spec["channel"]
                # SHARE the referenced table's enable, don't grow a second one. An optional axis that
                # is on for one table and off for the other is two grids of different rank reading the
                # same cells — which is precisely the drift that borrowing the axes exists to prevent.
                if spec.get("en"):
                    t[f"_{ax}_en_field"] = spec["en"]
                continue
            lo, hi = float(spec.get("min", 0.0)), float(spec.get("max", 0.0))
            external = bool(spec.get("external"))      # the firmware supplies the coordinate, not a channel
            if not external and not spec.get("channel"):
                sys.exit(f"ERROR: learned block '{b['id']}' {src} needs a channel, a path, or external: true")
            if hi <= lo or n < 1:
                sys.exit(f"ERROR: learned block '{b['id']}' {src} needs min < max and at least one bin")
            # Evenly spaced across [lo, hi] — the grid the firmware was hard-coding, now stated once and
            # TUNABLE, so a coolant axis can start below zero instead of folding every sub-zero reading
            # into bin 0.
            # Shared on the GRID, and an external axis's coordinate is its own — two tables sharing
            # a cam-index axis is a coincidence of shape, not the same quantity, so it keys on the
            # axis's identity rather than only its numbers.
            key = (mod, spec.get("channel", "@external"), lo, hi, n, external)
            if key in shared_axis:
                t[f"{ax}_axis"]    = shared_axis[key]      # a name ref — expand_inline_axes leaves it alone
                t[f"{ax}_channel"] = spec["channel"]
                continue
            step = (hi - lo) / n
            t[f"{ax}_axis"] = {
                "max":    n,
                "values": [round(lo + step * i, 4) for i in range(n)],
                "label":  f"{t['label']} {ax.upper()} Axis",
                "units":  spec.get("units", ""),
                **({"external": True} if external else {"signal": spec["channel"]}),
            }
            shared_axis[key] = f"{t['name']}_{ax}_axis"    # the name expand_inline_axes will give it
        modules[mod].setdefault("tables", []).append(t)


def _board_battery(board: str | None) -> dict:
    """{index, divider, signal} for the board's dedicated battery-sense pin, or {}.

    Mirrors board_codegen's own rule exactly — the ANALOG_VOLTAGE pin carrying a `divider` —
    so BOARD_BATTERY_AIN_INDEX / BOARD_BATTERY_DIVIDER and everything derived here cannot
    disagree about which pin the battery is on or what it is divided by.
    """
    import yaml
    boards = Path(__file__).resolve().parent.parent / "definition" / "boards"
    p = boards / f"{board}.board.yaml"
    if not board or not p.is_file():
        return {}
    data = yaml.safe_load(p.read_text()) or {}
    by_sig = {q["signal"]: q for q in data.get("pins", [])}
    av = [q["signal"] for q in data.get("pins", []) if "ANALOG_VOLTAGE" in (q.get("caps") or [])]
    for i, sig in enumerate(av):
        if "divider" in by_sig.get(sig, {}):
            return {"index": i, "divider": float(by_sig[sig]["divider"]), "signal": sig}
    return {}


def _board_adc(board: str | None) -> dict:
    """{full_scale, av_mv, at_mv, vref_mv} for the board's ADC — full_scale = 2^adc_bits-1
    (NOT hardcoded 12-bit; some boards aren't 4095-count). Mirrors board_codegen's defaults so
    the raw-count max + the counts->mV display mult track the actual converter. {} if no board."""
    import yaml
    boards = Path(__file__).resolve().parent.parent / "definition" / "boards"
    p = boards / f"{board}.board.yaml"
    if not p.is_file():
        avail = sorted(boards.glob("*.board.yaml"))
        if not avail:
            return {}
        p = avail[0]
    data = yaml.safe_load(p.read_text()) or {}
    vref = int(data.get("adc_vref_mv", 3300))
    bits = int(data.get("adc_bits", 12))
    return {"full_scale": (1 << bits) - 1, "vref_mv": vref,
            "av_mv": int(data.get("analog_av_fullscale_mv", vref)),
            "at_mv": int(data.get("analog_at_fullscale_mv", vref))}


def _raw_telem_channels(board: str | None) -> list:
    """Per analog-input-pin RAW ADC-count telemetry — the single source of truth the host
    display-adjusts to mV/V (the "Raw Input Display" setting), never converted in firmware.
    One uint16 channel per AV/AT pool position, named raw_av<n>/raw_at<n> and tagged with the
    firmware analog pool index so the pack reads platform_read_ain_raw(pool). Empty if no board.

    AT pins start where the AV pins end, so a raw_at channel's pool index is len(AV)+j — matching
    platform_read_ain_raw()'s split on BOARD_ANALOG_T_BASE. This was a literal 16."""
    if not board:
        return []
    caps = _board_pins_by_cap(board)
    out = []
    for i, _ in enumerate(caps.get("ANALOG_VOLTAGE", [])):
        out.append({"id": f"raw_av{i + 1}", "label": f"Raw AV{i + 1}", "pool": i})
    at_base = len(caps.get("ANALOG_VOLTAGE", []))
    for j, _ in enumerate(caps.get("ANALOG_TEMP", [])):
        out.append({"id": f"raw_at{j + 1}", "label": f"Raw AT{j + 1}", "pool": at_base + j})
    return out


def _hw_input_signals(board: str | None) -> list:
    """Raw hardware analog inputs promoted to FIRST-CLASS bus signals (uint32 ADC counts) — one per
    AV/AT pool position, named hw_av<n>/hw_at<n>. main() appends these at the TAIL of schema['signals']
    so every existing SignalId keeps its value. The HardwareInput stage publishes them each frame
    (gen_hw_input_publish); telemetry — and, incrementally, the sensor pipeline — read them FROM THE
    BUS, which is what lets the old telem-only raw_* special case collapse. Board-derived, empty if none."""
    raw_max = float(_board_adc(board).get("full_scale", 4095)) if board else 4095.0
    out = []
    for r in _raw_telem_channels(board):
        name = r["id"][len("raw_"):]                    # raw_av1 -> av1
        out.append({"id": f"hw_{name}", "label": r["label"], "units": "ADC", "scale": 1.0,
                    "min": 0.0, "max": raw_max, "telem_type": "uint16",
                    "category": "Sensors - Raw", "bus_type": "uint32", "hw_raw": True})
    return out


# DIG-pin capture modes promoted to raw bus signals: (schema-id suffix, label, units, capture kind for
# the HardwareInput publish). A pin can carry freq+pulse simultaneously (flex fuel); sent/level are the
# other single-mode reads. All uint32 cells. See _hw_digital_signals / gen_hw_input_publish.
_HW_DIG_MODES = [("freq", "Frequency", "Hz"), ("pulse", "Pulse Width", "us"),
                 ("sent", "SENT", ""), ("level", "Level", "")]


def _hw_digital_signals(board: str | None) -> list:
    """Raw hardware DIGITAL inputs as first-class bus signals — one channel per (DIG pin, capture mode),
    all uint32. HardwareInput reads each mode once and publishes it; the sensor pipeline's digital
    Acquire reads its mode's channel off the bus (the pin is enabled by the acquire, read via the bus).
    Board-derived from the DIGITAL_INPUT pool; empty if no board. `hw_raw` marks them non-selectable in
    signal pickers — they're plumbing, not a source a user picks."""
    if not board:
        return []
    dig = _board_pins_by_cap(board).get("DIGITAL_INPUT", [])
    out = []
    for i in range(len(dig)):
        for m, label, units in _HW_DIG_MODES:
            out.append({"id": f"hw_dig{i+1}_{m}", "label": f"DIG{i+1} {label}", "units": units,
                        "scale": 1.0, "min": 0.0, "max": 4294967295.0, "telem_type": "uint32",
                        "category": "Sensors - Raw", "bus_type": "uint32", "hw_raw": True})
    return out


# C type per telemetry primitive (for the generated packer).
_C_TYPE = {"uint8": "uint8_t", "int8": "int8_t", "uint16": "uint16_t", "int16": "int16_t",
           "uint32": "uint32_t", "int32": "int32_t", "float": "float"}


def telemetry_fields(schema: dict, board: str | None = None) -> list:
    """The telemetry frame, derived straight from the channel catalog — one field per
    schema['signals'] entry (loggable/gaugeable TS OutputChannel; units/scale from the producing
    sensor's type). Pure: returns the field dicts, no module mutation. This IS the single source for
    the EcuTelemetry struct, the .ini [OutputChannels], the bus packer, and the tuneit-meta telemetry.
    (The values are packed from the SignalBus at runtime — see gen_sensor_telem_pack.)

    RAW ADC-count channels are no longer a special case: main() appends them as first-class hw_*
    signals (uint32 ADC counts) at the catalog tail, so they flow through the normal bus path here —
    the bare 0..4095 count (units "ADC", scale 1) the host display-adjusts to mV/V. See _hw_input_signals."""
    if "Sensors" not in (schema.get("modules") or {}):
        return []
    # WHAT A SENSOR MEASURES IS ALWAYS WORTH LOGGING. The catalog's `datalog:` was written on module
    # OUTPUTS — advance, wastegate duty, the fuel calculation — and never on the readings that drove
    # them, so the logged set had ignition advance but no coolant temperature, boost target but no
    # throttle position. A log of what the ECU decided, without what it decided from, cannot answer
    # the question anybody opens a log to ask. So a channel a sensor PUBLISHES is logged by default,
    # and the explicit flag adds the derived channels on top of that.
    sensor_channels = {c for sen in (schema.get("sensors") or [])
                         for c in (sen.get("provides") or [sen["id"]])}
    # HOW OFTEN A CHANNEL ACTUALLY CHANGES. A sensor is decimated to its TYPE's update_hz
    # (Sensors.cpp takes max(type default, highest live claim)), so a log running faster than that
    # records the same value repeatedly — which is the truth about the channel, but only if someone
    # is told. Anything no sensor publishes comes from a module on the control frame, so it moves at
    # frame rate; that is the 1000 below, and it is why rpm and advance resolve at 1 kHz while
    # coolant temperature does not.
    #
    # Highest wins where several sensors publish the same channel: the fastest publisher is the one
    # that decides how fresh the value can be.
    # THE ONE UNKNOWN IS A SENSOR WHOSE JOB IS GENERIC. `generic` is not a type — there is no such
    # entry in sensor_types — it marks an input with no assigned purpose, and the tune gives it one
    # of the established types, whose personality it then takes on entirely. Its cadence is that
    # type's, and which type that is cannot be known here. So it is left ABSENT rather than defaulted
    # to frame rate: claiming 1 kHz for an aux input somebody configured as a temperature would be
    # the meta lying with confidence.
    #
    # There is deliberately no per-sensor override. Raising one sensor's floor breaks the phase
    # alignment of a cross-checked pair (see align_crosschecked in Sensors.cpp) — same phase only
    # lands on the same tick at the same rate, and the partner is chosen by the tune. Modules raise
    # what they read through the claim mechanism instead, which moves both tracks together.
    _type_hz = {t["id"]: int(t.get("update_hz") or 0) for t in (schema.get("sensor_types") or [])}
    _chan_hz: dict[str, int] = {}
    _unknown: set[str] = set()
    for sen in (schema.get("sensors") or []):
        hz = _type_hz.get(sen.get("type"), 0)
        for c in (sen.get("provides") or [sen["id"]]):
            if hz:
                if hz > _chan_hz.get(c, 0):
                    _chan_hz[c] = hz
            else:
                _unknown.add(c)
    fields = [
        {"name": ch["id"], "type": ch["telem_type"], "label": ch["label"], "units": ch["units"],
         "scale": ch["scale"], "min": ch["min"], "max": ch["max"],
         # THE CATALOG'S OWN ANSWER, not True for everything. This was hardcoded True, which threw
         # away the `datalog:` the catalog declares on 173 channels and left the flag saying nothing —
         # every channel was "worth logging", which is the same as no channel being marked. It is what
         # a recorder offers as the standard set, so it has to mean what the definition says.
         "datalog": bool(ch.get("datalog")) or ch["id"] in sensor_channels,
         "category": ch["category"],
         # A typed sensor's cadence; 0 (omitted downstream) for a generic input whose type the tune
         # decides; the control frame's 1000 for anything no sensor publishes at all.
         "update_hz": _chan_hz.get(ch["id"]) or (0 if ch["id"] in _unknown else 1000),
         # Carry the BUS cell type through to the meta: the wire type above says how the value is packed
         # for transport, this says what it IS in the ECU. A consumer that only sees the wire type cannot
         # tell a bitmask from a float (both arrive as U32).
         **({"bus_type": ch["bus_type"]} if ch.get("bus_type") else {}),
         **({"enum": ch["enum"]} if ch.get("enum") else {}),
         # A CHANNEL'S OWN LABELS, when it names them inline rather than through an `enums:` set. The
         # `enum:` ref above resolves against meta["enums"], but a channel whose states belong to
         # nobody else (lambda_cl_state's Off/Wideband/Narrowband/Open loop) declares `options:` on
         # the spot — and that list was dropped here, so the studio had nothing to render and showed
         # the raw digit. A state channel that displays "3" says less than one that says "Open loop".
         **({"options": list(ch["options"])} if ch.get("options") else {})}
        for ch in schema["signals"]   # first-class catalog (incl. the tail-appended hw_* raw inputs)
    ]
    return fields


def gen_sensor_telem_pack(schema: dict, board: str | None = None) -> str:
    """An #include fragment: pack the SensorsTelemetry struct from the SignalBus. OPTIMISED — each
    channel read EXACTLY once, straight-line (no table indirection, no allocation), int channels
    rounded+clamped by type, float channels stored raw. Included inside an EngineTask method that
    provides `t` (SensorsTelemetry&), `bus` (SignalBus&), telem_round<> and the SIG_* ids.

    RAW ADC-count channels are ordinary uint32 hw_* signals now (appended to the catalog by main()),
    so they pack via the uint32 branch below — bus.get_u32, no HAL read here. HardwareInput publishes
    them onto the bus each frame (gen_hw_input_publish)."""
    out = ["// AUTO-GENERATED — do not edit. Run codegen/codegen.py.",
           "// Pack fragment: SensorsTelemetry <- SignalBus, one read per channel."]
    for ch in schema["signals"]:   # first-class catalog
        sig = "SIG_" + ch["id"].upper()
        bt  = ch.get("bus_type", "float")
        ct  = _C_TYPE.get(ch["telem_type"], "int16_t")
        if bt == "uint32":                                  # integer cell — read raw, no float math
            out.append(f"t.{ch['id']} = static_cast<{ct}>(bus.get_u32({sig}, 0));")
        elif bt == "int32":
            out.append(f"t.{ch['id']} = static_cast<{ct}>(bus.get_i32({sig}, 0));")
        elif ch["telem_type"] == "float":                   # float cell, float wire — store raw
            out.append(f"t.{ch['id']} = bus.get({sig}, 0.0f);")
        else:                                               # float cell, int wire — scale + round
            mult = (1.0 / ch["scale"]) if ch["scale"] else 1.0
            out.append(f"t.{ch['id']} = telem_round<{ct}>(bus.get({sig}, 0.0f) * {repr(float(mult))}f);")
    return "\n".join(out) + "\n"


def gen_hw_input_keygate(board: str | None, schema: dict | None = None) -> str:
    """#include fragment for Sensors::update() — RETRACT every raw analog pin except the battery's, in
    the pass that finds the key off.

    HardwareInput publishes all of them a phase earlier and cannot do this itself: it runs BEFORE the key
    is decided, so gating there would cost a frame on the pass the key turns — and "the pass in which the
    key turns is the pass in which the rest publish" is a property the sensors test pins down. Sensors
    owns the key and knows it mid-pass, so the retraction belongs here.

    Retracted rather than left alone because with the key off USB feeds the 5 V followers through a diode,
    and the drop across it moves the reference the ADC measures against: the counts are real numbers
    against the wrong reference. Marked INVALID (value 0) so a reader gets nothing rather than a plausible
    wrong number — 0 in, 0 out, matching what the gated sensors themselves publish.

    The BATTERY's pin is exempt and must be: key_on is derived from it, and its 12 V divider feeds the
    ADC directly rather than through a follower. Gating it would retract the very signal the key state
    is decided from.

    Its pool index comes from THE BOARD — the ANALOG_VOLTAGE pin carrying a `divider` — not from parsing
    the catalog entry's pin name. Parsing it meant two independent bugs waiting: the name had to be a
    literal AV<n>/AT<n> (so a catalog that asked the board instead silently found no battery and gated
    it), and the AT base was written here as a literal 16, correct only for a board with sixteen voltage
    inputs. Neither is visible in the generated output unless you already know which pin to look for."""
    out = ["// AUTO-GENERATED — do not edit. Run codegen/codegen.py.",
           "// Key off: every raw analog pin but the battery's is retracted (invalid), not merely ignored."]
    _batt = _board_battery(board)
    boot_pool = _batt.get("index") if _batt else None
    for r in _raw_telem_channels(board):
        if r["pool"] == boot_pool:
            continue                                       # the key's own source stays live
        sig = "SIG_HW_" + r["id"][len("raw_"):].upper()
        out.append(f"bus.set_u32({sig}, 0u, false, now, 0u);")
    # DIGITAL TOO. A level, a frequency, a pulse width and a SENT word are all read through the same
    # unpowered front end, and a switch pulled up by a rail that is not at its key-on voltage is a
    # reading nobody should act on either. Nothing needs them with the key off — the crank and cam
    # inputs are capture-driven and live elsewhere — so they go quiet with everything else.
    dig = _board_pins_by_cap(board).get("DIGITAL_INPUT", []) if board else []
    for i in range(len(dig)):
        n = i + 1
        for suffix in ("FREQ", "PULSE", "SENT", "LEVEL"):
            out.append(f"bus.set_u32(SIG_HW_DIG{n}_{suffix}, 0u, false, now, 0u);")
    return "\n".join(out) + "\n"


def gen_hw_input_publish(board: str | None, schema: dict | None = None) -> str:
    """#include fragment for HardwareInput::update() — publish each raw analog input onto the
    SignalBus as u32 ADC counts, read exactly ONCE from the HAL (platform_read_ain_raw(pool)). The
    including scope provides `bus` (SignalBus&), `now_ms` (uint32_t) and HW_INPUT_TTL_MS. Signal ids
    (SIG_HW_AV*/SIG_HW_AT*) and pool order match _hw_input_signals()/_raw_telem_channels()."""
    out = ["// AUTO-GENERATED — do not edit. Run codegen/codegen.py.",
           "// HardwareInput publish: raw physical inputs -> SignalBus (u32 cells, one HAL read each)."]
    for r in _raw_telem_channels(board):
        sig = "SIG_HW_" + r["id"][len("raw_"):].upper()    # raw_av1 -> SIG_HW_AV1
        out.append(f"bus.set_u32({sig}, platform_read_ain_raw({r['pool']}), true, now_ms, HW_INPUT_TTL_MS);")
    # DIGITAL: one read per (pin, mode). The capture is ENABLED by the consuming sensor's Acquire
    # (config-driven); an unconfigured pin's counter is simply never fed, so these read 0 harmlessly.
    # SENT is read with CRC enforced (the safe default; a per-sensor 'accept bad CRC' flag is not
    # honoured here — the value is shared on the bus). freq/pulse coexist (flex fuel: freq=ethanol,
    # pulse=fuel temp off the same pin).
    dig = _board_pins_by_cap(board).get("DIGITAL_INPUT", []) if board else []
    for i in range(len(dig)):
        n = i + 1
        out.append(f"bus.set_u32(SIG_HW_DIG{n}_FREQ,  platform_read_freq({i}),          true, now_ms, HW_INPUT_TTL_MS);")
        out.append(f"bus.set_u32(SIG_HW_DIG{n}_PULSE, platform_read_pulse_us({i}),      true, now_ms, HW_INPUT_TTL_MS);")
        out.append(f"bus.set_u32(SIG_HW_DIG{n}_SENT,  platform_read_sent({i}, true),    true, now_ms, HW_INPUT_TTL_MS);")
        out.append(f"bus.set_u32(SIG_HW_DIG{n}_LEVEL, platform_read_din({i}) ? 1u : 0u, true, now_ms, HW_INPUT_TTL_MS);")
    return "\n".join(out) + "\n"


def _hw_pool_signals(board: str | None) -> dict:
    """Pin pool index -> the raw channel HardwareInput publishes for it, per interface id — the meta's
    copy of HW_POOL_SIG / HW_DIG_*_SIG, in the same pool order, so a sensor's `source` indexes it exactly
    as primary_hw_sig() does in firmware. "raw_av1" is the catalog id; "hw_av1" is the published
    channel and the name telemetry is keyed by, which is the strip the SIG_HW_* macro makes too."""
    analog = ["hw_" + r["id"][len("raw_"):] for r in _raw_telem_channels(board)]
    ndig   = len(_board_pins_by_cap(board).get("DIGITAL_INPUT", [])) if board else 0
    dig    = lambda mode: [f"hw_dig{i + 1}_{mode}" for i in range(ndig)]
    return {
        "analog_voltage":      analog,
        "engine_sync_voltage": analog,   # same pin, read on the engine-sync window
        "digital_freq":        dig("freq"),
        "pulse_width":         dig("pulse"),
        "sent":                dig("sent"),
        "digital":             dig("level"),
    }


def gen_hw_input_map(board: str | None) -> str:
    """generated/hw_input_map.h — analog pool index (a sensor's `source` / the platform_read_ain_raw
    arg) -> the raw hardware SignalId HardwareInput publishes for it. The sensor pipeline's analog
    Acquire uses SIG_HW_BY_POOL[source] to read a pin's counts FROM THE BUS instead of the HAL, so the
    pin is touched in one place. Pool order matches _raw_telem_channels()/_hw_input_signals()."""
    chans = _raw_telem_channels(board)
    body = [f"    SIG_HW_{r['id'][len('raw_'):].upper()},   // pool {r['pool']}" for r in chans]
    dig = _board_pins_by_cap(board).get("DIGITAL_INPUT", []) if board else []
    nd = len(dig)
    def dig_arr(name, mode):
        rows = [f"    SIG_HW_DIG{i+1}_{mode.upper()}," for i in range(nd)]
        return [f"static const SignalId {name}[HW_DIG_COUNT] = {{"] + rows + ["};"]
    # NB: arrays are NOT named SIG_* — validate_firmware_signal_refs() scans firmware/ for SIG_<id>
    # tokens, and a SIG_-prefixed array name would read as a bogus (undefined) signal id there.
    lines = [BANNER, "#pragma once", '#include "signal_ids.h"', "",
             "// Physical input source index -> raw hardware SignalId (published by HardwareInput; see",
             "// gen_hw_input_publish). The sensor pipeline's Acquire reads a pin's value from the bus",
             "// via these maps rather than calling the HAL a second time.",
             f"static constexpr uint8_t HW_POOL_COUNT = {len(chans)};   // analog AV/AT pool",
             "static const SignalId HW_POOL_SIG[HW_POOL_COUNT] = {"] + body + ["};", "",
             f"static constexpr uint8_t HW_DIG_COUNT = {nd};   // DIG pin pool (source index 0..N-1)"]
    lines += dig_arr("HW_DIG_FREQ_SIG",  "freq")
    lines += dig_arr("HW_DIG_PULSE_SIG", "pulse")
    lines += dig_arr("HW_DIG_SENT_SIG",  "sent")
    lines += dig_arr("HW_DIG_LEVEL_SIG", "level")
    lines.append("")
    return "\n".join(lines)

# Reserved manufacturer (P1xxx) block for the per-sensor "input not assigned" config DTC: one
# contiguous code per sensor index, P1800.. (just above the P17xx module_dtc cluster). Custom range.
SENSOR_DTC_CONFIG_BASE = 0x1800

def _dtc_to_u16(code) -> int:
    """'P0107' -> 0x0107. The letter prefix (P/C/B/U powertrain/chassis/body/network)
    is dropped; the 4 hex digits are stored. 0 / missing = no DTC."""
    if not code:
        return 0
    s = str(code).strip().upper()
    if s and s[0] in "PCBU":
        s = s[1:]
    try:
        return int(s, 16) & 0xFFFF
    except ValueError:
        return 0

# Reserved manufacturer (P1xxx) block for per-sensor DIAGNOSTIC codes: base + (sensor_index * 8) + slot,
# so P1000..P1307 covers 97 sensors x 6 checks and a code decodes straight back to the fault that raised
# it — (code - base) >> 3 is the sensor index, & 7 is the check slot. Sits below the P17xx module cluster
# and the P18xx per-sensor config block.
_DTC_NOTE_SHOWN = False       # the reassignment note is printed once a run, not once a caller
SENSOR_DTC_DIAG_BASE = 0x1000
RAW_UNITS = {"volts": "ADC", "hz": "Hz", "state": ""}

DIAG_SLOTS = ["raw_min", "raw_max", "operating_min", "operating_max", "stuck", "max_derivative"]


def sensor_diag_dtcs(schema: dict, sensor_types: list, sensors: list) -> dict:
    """Per-sensor DTC code for every check that sensor can actually raise.

    Rules, in order:
      1. Only checks the sensor's TYPE supports (its `diagnostics` list) can raise anything at all.
      2. A standard OBD code declared in the catalog wins — we support real P-codes wherever one exists.
      3. A standard code claimed by more than one fault cannot pinpoint anything, so the FIRST claim keeps
         it (catalog order, then slot order) and the rest fall through to rule 4. Reported, never silent.
      4. Anything still uncoded is allocated from the manufacturer block, uniquely and deterministically.

    Enforced afterwards: every applicable check has a non-zero code, and no code is shared. A check the
    user can arm that raises nothing is worse than not offering it — the fault is detected and then
    thrown away.
    """
    types = {t["id"]: t for t in sensor_types}
    out, owner, reassigned = {}, {}, []
    for idx, sen in enumerate(sensors):
        applicable = sensor_diagnostics(schema, sen)
        declared   = sen.get("dtc") or {}
        row = {}
        for slot, check in enumerate(DIAG_SLOTS):
            if check not in applicable:
                row[check] = 0
                continue
            std = _dtc_to_u16(declared.get(check))
            if std and std not in owner:
                owner[std] = (sen["id"], check)
                row[check] = std
                continue
            if std:                                    # already claimed by an earlier fault
                reassigned.append((sen["id"], check, std, owner[std]))
            code = SENSOR_DTC_DIAG_BASE + (idx << 3) + slot
            if code in owner:
                raise SystemExit(f"sensor diag DTC P{code:04X} collides with {owner[code]}")
            owner[code] = (sen["id"], check)
            row[check] = code
        # Slot 6 of the same per-sensor block: this sensor's PRECONDITION EXPRESSION failed
        # validation. Every sensor gets one — the field exists on all of them, so a bad program is
        # possible on all of them, and "which sensor's gate is broken" is exactly the question the
        # code has to answer. Slots 0..5 are the checks above; 7 is spare.
        pcode = SENSOR_DTC_DIAG_BASE + (idx << 3) + 6
        if pcode in owner:
            raise SystemExit(f"sensor precond DTC P{pcode:04X} collides with {owner[pcode]}")
        owner[pcode] = (sen["id"], "precond_invalid")
        row["precond_invalid"] = pcode
        # Slot 7, the last of the block: the reading matched NONE of this sensor's calibrated bands.
        # Only a band-decoding type can say that, so only they get one. It is not one of the six checks
        # — there is no threshold to choose and nothing to enable — it is the decoder reporting that the
        # calibration does not describe what the pin is doing.
        if "bands" in (types.get(sen.get("type"), {}).get("stages") or []):
            nbcode = SENSOR_DTC_DIAG_BASE + (idx << 3) + 7
            if nbcode in owner:
                raise SystemExit(f"sensor no-band DTC P{nbcode:04X} collides with {owner[nbcode]}")
            owner[nbcode] = (sen["id"], "no_band")
            row["no_band"] = nbcode
        out[sen["id"]] = row

    for sen in sensors:                                 # rule 1 + the guarantee that makes it worth arming
        applicable = sensor_diagnostics(schema, sen)
        for check in applicable:
            if not out[sen["id"]][check]:
                raise SystemExit(f"{sen['id']}.{check} is armable but has no DTC code")
    # SAID ONCE. This allocation is deterministic and several generators need its answer, so the note
    # was printed once per CALLER — and one of those callers had it inside a per-sensor loop, which put
    # a hundred identical copies of it in front of anything a build actually wanted read.
    global _DTC_NOTE_SHOWN
    if reassigned and not _DTC_NOTE_SHOWN:
        _DTC_NOTE_SHOWN = True
        print(f"  note: {len(reassigned)} fault(s) could not keep a shared standard code "
              f"(a code names ONE fault); allocated from P1xxx instead:")
        for sid, check, std, holder in reassigned[:10]:
            print(f"        {sid}.{check} wanted P{std:04X} (held by {holder[0]}.{holder[1]})")
    if SENSOR_DTC_DIAG_BASE + (len(sensors) << 3) > 0x17FF:
        raise SystemExit("sensor diag DTC block overflows into the P17xx module cluster")
    return out


# -----------------------------------------------------------------------
# DTC category indicators (schema `dtc_indicators`) — the bit map that drives the
# TS front-page error indicators. Range-based so standard P-codes auto-bucket.
# -----------------------------------------------------------------------

def _parse_dtc_code_list(codes) -> list:
    """[P0115-P0119, P0125] -> [(0x0115, 0x0119), (0x0125, 0x0125)] inclusive u16 ranges."""
    out = []
    for entry in (codes or []):
        s = str(entry).strip().upper()
        if "-" in s:
            a, b = s.split("-", 1)
            lo, hi = _dtc_to_u16(a), _dtc_to_u16(b)
        else:
            lo = hi = _dtc_to_u16(s)
        if lo and hi and lo <= hi:
            out.append((lo, hi))
    return out


def _dtc_categories(schema: dict) -> list:
    """[(bit, id, label, [(lo,hi),...]), ...] sorted by bit; empty if none defined."""
    cats = []
    seen_bits = {}
    for c in schema.get("dtc_indicators", []) or []:
        bit = int(c["bit"])
        if bit in seen_bits:
            raise SystemExit(f"dtc_indicators: bit {bit} used by both "
                             f"'{seen_bits[bit]}' and '{c['id']}'")
        if bit > 31:
            raise SystemExit(f"dtc_indicators: bit {bit} ('{c['id']}') exceeds the u32 mask")
        seen_bits[bit] = c["id"]
        cats.append((bit, c["id"], c.get("label", c["id"]),
                     _parse_dtc_code_list(c.get("codes"))))
    cats.sort(key=lambda x: x[0])
    return cats


def _code_category_bit(code: int, cats: list) -> int:
    """First category whose ranges contain `code`, else -1 (build-time warn helper)."""
    for bit, _id, _label, ranges in cats:
        for lo, hi in ranges:
            if lo <= code <= hi:
                return bit
    return -1


def gen_dtc_categories_h(cats: list) -> str:
    guard = "JAYECU_GENERATED_DTC_CATEGORIES_H"
    lines = [BANNER, "#pragma once", f"#ifndef {guard}", f"#define {guard}", "",
             "#include <cstdint>", "",
             "// Maps an OBD P-code to its DTC category bit (schema dtc_indicators).",
             "// Returns the bit index [0..DTC_INDICATOR_COUNT) or -1 if uncategorised.",
             "// Range-based, so a standards-compliant sensor code auto-buckets.",
             "inline int dtc_indicator_bit(uint16_t code) {"]
    for bit, cid, label, ranges in cats:
        tests = [(f"code == 0x{lo:04X}" if lo == hi
                  else f"(code >= 0x{lo:04X} && code <= 0x{hi:04X})") for lo, hi in ranges]
        cond = " || ".join(tests) if tests else "false"
        lines.append(f"    if ({cond}) return {bit};   // {cid}: {label}")
    lines += ["    return -1;", "}", "",
              f"static constexpr uint8_t DTC_INDICATOR_COUNT = {len(cats)};",
              "", f"#endif // {guard}", ""]
    return "\n".join(lines)


# -----------------------------------------------------------------------
# Control-module signal-validity DTCs (schema `module_dtc`). One unique P-code per
# (module, required input) — generated/module_dtc.h gives the firmware the named
# constants it raises; the meta `dtc_descriptions` gives the studio the hover text.
# -----------------------------------------------------------------------

# Sensor `dtc:` fault keys -> a human phrase, so a sensor P-code reads as
# "<Sensor Name> — <phrase>" in the studio DTC dock hover.
_DTC_FAULT_PHRASE = {
    "raw_min":        "circuit low (short to ground / open)",
    "raw_max":        "circuit high (short to supply)",
    "operating_min":  "reading below operating range",
    "operating_max":  "reading above operating range",
    "stuck":          "signal stuck / not responding",
    "max_derivative": "implausible rate of change",
}

_DTC_SEV = {"level1": 1, "level2": 2, "level3": 3}   # schema invalid_sev -> DTC_SEV_LEVEL* numeric

def _module_dtc_entries(schema: dict) -> list:
    """[(name, code_u16, label, invalid_sev), ...] from schema `module_dtc`; validates uniqueness.
    invalid_sev = severity raised when a CONFIGURED signal goes invalid (default level2); the
    unconfigured (sentinel) case always raises level 1 at the call site."""
    out, seen = [], {}
    for e in schema.get("module_dtc", []) or []:
        name = e["name"]
        code = _dtc_to_u16(e["code"])
        if not code:
            raise SystemExit(f"module_dtc '{name}': bad code {e.get('code')!r}")
        if code in seen:
            raise SystemExit(f"module_dtc: code {e['code']} used by both "
                             f"'{seen[code]}' and '{name}' (a DTC must be unique)")
        seen[code] = name
        sev_s = str(e.get("invalid_sev", "level2")).lower()
        if sev_s not in _DTC_SEV:
            raise SystemExit(f"module_dtc '{name}': bad invalid_sev {e.get('invalid_sev')!r} "
                             f"(expected level1/level2/level3)")
        out.append((name, code, e.get("label", name), _DTC_SEV[sev_s]))
    return out

def gen_module_dtc_h(entries: list) -> str:
    guard = "JAYECU_GENERATED_MODULE_DTC_H"
    lines = [BANNER, "#pragma once", f"#ifndef {guard}", f"#define {guard}", "",
             "#include <cstdint>", "",
             "// Module signal-validity DTC codes (DtcSource::MODULE). Raised by a consumer when a",
             "// bus signal it REQUIRES is invalid/missing, healed when it returns. One code per",
             "// (module, signal) so a fault is traceable to its exact source. Schema: module_dtc.",
             "namespace ModuleDtc {"]
    w = max((len(n) for n, *_ in entries), default=1)
    for name, code, label, _sev in entries:
        lines.append(f"    constexpr uint16_t {name:<{w}} = 0x{code:04X};   // {label}")
    lines += ["",
              "    // Severity raised when the signal is CONFIGURED but INVALID at runtime (schema",
              "    // invalid_sev; 1..3 = level 1..3). An UNCONFIGURED slot (sentinel 255) always",
              "    // raises level 1 at the call site — it's a tuning gap, not a live fault."]
    for name, _code, _label, sev in entries:
        lines.append(f"    constexpr uint8_t  {name+'_SEV':<{w+4}} = {sev};")
    lines += ["}", "", f"#endif // {guard}", ""]
    return "\n".join(lines)

def _autotune_meta(schema: dict) -> dict:
    """The studio VE autotuner's contract, validated against this schema.

    Everything here is a NAME the studio will look up at runtime, and a name that does not resolve
    fails silently over there — the panel simply never accumulates a record and nothing says why.
    So every one is checked against the schema that emitted it, here, where a mistake is a build
    error rather than a feature that quietly does nothing."""
    at = schema.get("autotune") or {}
    if not at:
        return {}
    chans = {s.get("id") for s in (schema.get("signals") or [])}
    tables = set()
    for mod_name, mod in (schema.get("modules") or {}).items():
        ms = module_snake(mod_name)
        for t in (mod.get("tables") or []):
            if t.get("name"):
                tables.add(f"{ms}.{t['name']}")

    def need_chan(v, where):
        if v and v not in chans:
            raise SystemExit(f"autotune.{where}: '{v}' is not a signal id")
        return v

    def need_table(v, where):
        if v and v not in tables:
            raise SystemExit(f"autotune.{where}: '{v}' is not a module table")
        return v

    out = {
        "table":          need_table(at.get("table"), "table"),
        "lambda_channel": need_chan(at.get("lambda_channel"), "lambda_channel"),
        "target_channel": need_chan(at.get("target_channel"), "target_channel"),
        "ego_channels":   [need_chan(c, "ego_channels") for c in (at.get("ego_channels") or [])],
        "filters": [{"name":    f.get("name", f.get("channel", "?")),
                     "channel": need_chan(f.get("channel"), "filters"),
                     "op":      ">" if str(f.get("op", "<")).strip() == ">" else "<",
                     "value":   float(f.get("value", 0.0))}
                    for f in (at.get("filters") or [])],
    }
    delay = (at.get("delay") or {}).get("table")
    if delay:
        out["delay_table"] = need_table(delay, "delay.table")
    return out


def _value_autotune_meta(schema: dict) -> list:
    """The studio's VALUE-learned tables (schema `value_autotune`), validated like the VE contract: every
    name is looked up at runtime, so one that does not resolve is a build error here rather than a target
    that silently never learns."""
    targets = schema.get("value_autotune") or []
    if not targets:
        return []
    chans = {s.get("id") for s in (schema.get("signals") or [])}
    tables = set()
    for mod_name, mod in (schema.get("modules") or {}).items():
        ms = module_snake(mod_name)
        for t in (mod.get("tables") or []):
            if t.get("name"):
                tables.add(f"{ms}.{t['name']}")
    out = []
    for i, t in enumerate(targets):
        where = f"value_autotune[{i}]"
        def need_chan(v, what):
            if not v or v not in chans:
                raise SystemExit(f"{where}.{what}: '{v}' is not a signal id")
            return v
        if t.get("table") not in tables:
            raise SystemExit(f"{where}.table: '{t.get('table')}' is not a module table")
        out.append({
            "name":          t.get("name") or t["table"],
            "table":         t["table"],
            "value_channel": need_chan(t.get("value_channel"), "value_channel"),
            "settle_ms":     float(t.get("settle_ms", 500)),
            "steady":  [{"channel": need_chan(x.get("channel"), "steady"), "span": float(x.get("span", 0.0))}
                        for x in (t.get("steady") or [])],
            "filters": [{"name":    f.get("name", f.get("channel", "?")),
                         "channel": need_chan(f.get("channel"), "filters"),
                         "op":      ">" if str(f.get("op", "<")).strip() == ">" else "<",
                         "value":   float(f.get("value", 0.0))}
                        for f in (t.get("filters") or [])],
        })
    return out


def _dtc_descriptions(schema: dict) -> dict:
    """{ 'P0122': 'Throttle Position — circuit low …', … } for the studio dock hover.
    Built from the sensor catalog `dtc:` assignments + the module_dtc list (one source)."""
    desc: dict = {}
    for i, s in enumerate(schema.get("sensors", []) or []):
        name = s.get("name", s.get("id", "?"))
        for chk, val in (s.get("dtc") or {}).items():
            code = _dtc_to_u16(val)
            if code and _DTC_FAULT_PHRASE.get(chk):
                desc[f"P{code:04X}"] = f"{name} — {_DTC_FAULT_PHRASE[chk]}"
        # Auto-allocated per-sensor config DTC (P18xx): enabled but no input pin assigned.
        desc[f"P{SENSOR_DTC_CONFIG_BASE + i:04X}"] = f"{name} — input not assigned (interface set, no pin)"
    for _name, code, label, _sev in _module_dtc_entries(schema):
        desc[f"P{code:04X}"] = label
    # Firmware-logic codes (protection trips, config errors, throttle faults). Declared at their raise
    # site, described here -- see the schema `firmware_dtc` comment. A sensor/module entry wins if a
    # code is somehow in both, since that one carries the sensor's own name.
    for e in schema.get("firmware_dtc", []) or []:
        code = _dtc_to_u16(e["code"])
        desc.setdefault(f"P{code:04X}", e["label"])
    return desc


def gen_dtc_reference_md(schema: dict) -> str:
    """docs/dtc-codes.md — every code the firmware can raise, and exactly which fault raises it.

    Generated from the catalog, never hand-written: a code that is not in here cannot be raised, and a
    fault that can be raised is always in here. That is the whole point — a technician reading a code off
    a scan tool has to land on ONE sensor and ONE check, not a guess."""
    types   = {t["id"]: t for t in schema.get("sensor_types", [])}
    sensors = schema.get("sensors", [])
    diag    = sensor_diag_dtcs(schema, schema.get("sensor_types", []), sensors)
    pretty  = {"raw_min": "Detect Raw Low", "raw_max": "Detect Raw High",
               "operating_min": "Detect Reading Low", "operating_max": "Detect Reading High",
               "stuck": "Detect Stuck", "max_derivative": "Detect Rate Spike"}
    what = {
        "raw_min": "raw input below Raw Low Threshold (mV, before calibration)",
        "raw_max": "raw input above Raw High Threshold (mV, before calibration)",
        "operating_min": "calibrated reading below Reading Low Threshold, while the preconditions hold",
        "operating_max": "calibrated reading above Reading High Threshold, while the preconditions hold",
        "stuck": "switch input unchanged for the stuck delay",
        "max_derivative": "value changing faster than Max Rate of Change",
    }
    rows = []
    for idx, sen in enumerate(sensors):
        applicable = sensor_diagnostics(schema, sen)
        for check in DIAG_SLOTS:
            if check not in applicable:
                continue
            code = diag[sen["id"]][check]
            std  = "standard" if code < SENSOR_DTC_DIAG_BASE else "manufacturer"
            rows.append((f"P{code:04X}", sen["name"], sen["id"], pretty[check], what[check], std))
        # Every sensor can carry a precondition expression, so every sensor can have a broken one.
        pc = diag[sen["id"]]["precond_invalid"]
        rows.append((f"P{pc:04X}", sen["name"], sen["id"], "Precondition Invalid",
                     "this sensor's precondition expression failed validation — the reading checks "
                     "stay ARMED rather than silently switching off", "manufacturer"))
        # Band-decoding types only: the voltage matched none of the bands the user defined. Not one of
        # the six arm-able checks — there is no threshold to pick — so it is listed here beside the
        # precondition code, which has the same always-on shape.
        nb = diag[sen["id"]].get("no_band")
        if nb:
            rows.append((f"P{nb:04X}", sen["name"], sen["id"], "No Calibrated Band",
                         "the reading matched none of this sensor's voltage bands — the channel goes "
                         "invalid, which shuts down anything consuming it", "manufacturer"))
    rows.sort(key=lambda r: r[0])

    out = ["# Diagnostic Trouble Codes", "",
           "GENERATED by codegen.py from `definition/ecu.schema.yaml` — do not edit by hand.", "",
           "Every check a user can arm raises a code, and every code names exactly one sensor and one",
           "check. Standard OBD-II codes are used wherever one exists for that specific fault; the rest",
           "come from the manufacturer block, laid out as "
           f"`P{SENSOR_DTC_DIAG_BASE:04X} + (sensor index * 8) + check slot`, so a manufacturer code",
           "decodes straight back to the fault that raised it.", "",
           "A code is raised when its check trips and healed when it stops tripping. The severity",
           "recorded alongside it is per-sensor and per-check (Diag Severity).", "",
           f"| Code | Sensor | Check | Raised when | Range |", "|---|---|---|---|---|"]
    for code, name, sid, check, wh, std in rows:
        out.append(f"| `{code}` | {name} (`{sid}`) | {check} | {wh} | {std} |")
    out += ["", f"{len(rows)} codes.", ""]
    return "\n".join(out)

def sensor_type_token(sen):
    """A sensor row's compile-time type as a C token. A `generic` row has none, so it carries the
    sentinel — the catalog states the absence rather than naming a stand-in type."""
    return "SENSOR_TYPE_NONE" if sen.get("type") == NO_TYPE else f'SENSOR_TYPE_{sen["type"].upper()}'

def gen_sensors_catalog_h(sensor_types: list, sensors: list, channel_ids: set,
                          interfaces: list, cal_points: int, schema: dict = None) -> str:
    guard = "JAYECU_GENERATED_SENSORS_CATALOG_H"
    type_ids = [t["id"] for t in sensor_types]
    type_update_hz = {t["id"]: int(t.get("update_hz", 0)) for t in sensor_types}
    # Engineering-value scale per type = 10^-decimals. cal_val_* / diag_op_* are int16 in
    # this scale (cal_val=1000 with decimals=1 → 100.0). Picked so each type's min..max fits
    # int16: kPa/degC at 0.1 → ±3276, lambda/V at 0.01 → ±327. The firmware multiplies the
    # stored int by val_scale; TS displays at this scale with `decimals` digits.
    type_scale = {t["id"]: 10.0 ** -int(t.get("decimals", 2)) for t in sensor_types}
    groups = []
    for s in sensors:
        g = s.get("group", "other")
        if g not in groups:
            groups.append(g)
    iface_bit = {iface: 1 << i for i, iface in enumerate(interfaces)}

    lines = [BANNER, "#pragma once", f"#ifndef {guard}", f"#define {guard}", "",
             "#include <cstdint>", '#include "signal_ids.h"', "",
             "// Tier-1 type, Tier-2 group, and the input classes a sensor may bind.",
             "enum SensorType : uint8_t {"]
    lines += [f"    SENSOR_TYPE_{t.upper()}," for t in type_ids]
    lines += [f"    SENSOR_TYPE_COUNT = {len(type_ids)},",
              "    SENSOR_TYPE_NONE = 255,   // NOT a type: this input has none yet. Deliberately outside",
              "                              // 0..COUNT so it can never index SENSOR_TYPE_CATALOG, and every",
              "                              // reader has to handle 'no type' on purpose rather than by luck.",
              "};", ""]

    # The TYPE table — units, precision and value domain, one row per SensorType. This is what a
    # sensor's engineering numbers come from: the descriptor no longer carries its own val_scale
    # copy, because a `generic` input's type is chosen in the tune and a baked copy could not follow
    # it. Everything reads SENSOR_TYPE_CATALOG[effective_type(d, c)].
    lines += ["// Units, precision and value domain per sensor type. `val_scale` converts a stored",
              "// int16 (cal_val, diag_op_min/max) to engineering units; min/max is the domain those",
              "// values live in. `selectable` marks a type a `generic` input may be set to.",
              "struct SensorTypeDescriptor {",
              "    const char* id;",
              "    const char* units;",
              "    float    val_scale;     // engineering value = stored int16 * val_scale",
              "    uint16_t update_hz;     // the type's natural sample rate (0 = every frame)",
              "    float   min, max;       // the type's value domain, in engineering units",
              "    uint8_t selectable;     // 1 = a generic input may take this type",
              "};",
              f"static const SensorTypeDescriptor SENSOR_TYPE_CATALOG[SENSOR_TYPE_COUNT] = {{"]
    for t in sensor_types:
        # A generic input is read as an analog voltage through a cal curve, so it can take any type
        # built that same way. Derived from the type's own declaration — never a hand-kept list.
        sel = 1 if t.get("raw") == "volts" else 0
        lines.append(f'    {{ "{t["id"]}", "{t.get("units", "")}", '
                     f'{repr(float(type_scale[t["id"]]))}f, {int(t.get("update_hz", 0))}, '
                     f'{repr(float(t["min"]))}f, {repr(float(t["max"]))}f, {sel}, '
                     f'}},')
    lines += ["};", "",
              "enum SensorGroup : uint8_t {"]
    lines += [f"    SENSOR_GROUP_{g.upper()}," for g in groups]
    lines += [f"    SENSOR_GROUP_COUNT = {len(groups)},", "};", "",
              "enum SensorInterface : uint8_t {"]
    lines += [f"    IFACE_{iface.upper()} = 1u << {i}," for i, iface in enumerate(interfaces)]
    lines += ["    IFACE_NONE = 0,", "};", "",
              "// Config selector for SensorConfig.interface: a 0-based dropdown index into the",
              "// interface list (NO 'Default' — index 0 is the first interface), mapped to a",
              "// SensorInterface bit via SENSOR_IFACE_SEL_TO_BIT[]. Matches the TS",
              "// 'sensor_interfaces' option list order. active_iface() applies the catalog lock first.",
              "enum SensorIfaceSel : uint8_t {"]
    lines += [f"    IFSEL_{iface.upper()}," for iface in interfaces]   # first interface == 0
    lines += ["    IFSEL_COUNT,", "};",
              "static const uint8_t SENSOR_IFACE_SEL_TO_BIT[IFSEL_COUNT] = {"]
    lines += [f"    IFACE_{iface.upper()}," for iface in interfaces]
    lines += ["};", "",
              "// One row per named sensor. Read-only catalog (Tier 2); the mutable",
              "// per-sensor tune (Tier 3) lives in EcuConfig.sensors.",
              "struct SensorDescriptor {",
              "    const char* id;",
              "    const char* name;",
              "    uint8_t  type;              // SensorType",
              "    uint8_t  group;             // SensorGroup",
              "    uint16_t primary_channel;   // SignalId of provides[0] (the value the pipeline publishes)",
              "    uint8_t  interface_mask;    // OR of SensorInterface bits",
              "    uint8_t  locked_interface;  // a single SensorInterface bit, or 0 = selectable",
              "    uint16_t provides_off;      // index into SENSOR_PROVIDES[] of this node's first signal",
              "    uint8_t  provides_count;    // # signals this node provides (1 = single-output)",
              "    uint16_t aux_off;           // index into SENSOR_AUX_OUTPUTS[] of this sensor's first aux output",
              "    uint8_t  aux_count;         // # secondary outputs (each its own full pipeline); 0 = none",
              "    uint16_t dtc_raw_min, dtc_raw_max, dtc_op_min, dtc_op_max, dtc_stuck, dtc_max_deriv;",
              "    uint16_t dtc_precond;       // raised when this sensor's precondition EXPRESSION fails",
              "                                // validation (Expr.h). The gate then fails ARMED — a broken",
              "                                // expression must never silently switch detection off.",
              "    uint16_t dtc_config;        // raised when ENABLED but its interface needs a pin and none",
              "                                // is assigned (source == NONE). Auto-allocated P18xx; DtcSource::CONFIG.",
              "    uint16_t dtc_no_band;       // band-decoding types only: the reading matched NONE of this",
              "                                // sensor's calibrated bands. 0 for every other type.",
              "};", ""]
    # Flat pool of every node's provided SignalIds, concatenated in catalog order. A descriptor's
    # provides are SENSOR_PROVIDES[provides_off .. provides_off+provides_count). Lets a node provide
    # any signals (no consecutive-primary assumption); single-output nodes are just count==1.
    provides_pool: list[str] = []
    provides_span: list[tuple[int, int]] = []
    for s in sensors:
        chans = s.get("provides", [s["id"]])
        off = len(provides_pool)
        for ch in chans:
            provides_pool.append(f"SIG_{ch.upper()}" if ch in channel_ids else "SIG_NONE")
        provides_span.append((off, len(chans)))
    lines += [f"static constexpr uint16_t SENSOR_PROVIDES_COUNT = {len(provides_pool)};",
              "static const uint16_t SENSOR_PROVIDES[SENSOR_PROVIDES_COUNT] = {"]
    lines += ["    " + ", ".join(provides_pool[i:i+12]) + ","
              for i in range(0, len(provides_pool), 12)]
    lines += ["};", ""]

    # Secondary outputs: a sensor_type may declare `outputs:` — extra signals derived from the SAME
    # input, each its own full pipeline (acquire -> decode_linear -> cond_op_window ->
    # publish). The k-th declared output binds provides[1+k]. Each row is fully type-defined (acquire
    # kind, linear transform derived from the two `cal` points, operating range + P-codes); the
    # parent sensor supplies the source pin + filter via SensorConfig.
    aux_acq = {"pulse_us": 0, "freq": 1}        # AuxAcquireKind
    type_outputs = {t["id"]: (t.get("outputs") or []) for t in sensor_types}
    aux_rows: list[str] = []
    aux_span: list[tuple[int, int]] = []
    for si, s in enumerate(sensors):
        outs  = type_outputs.get(s["type"], [])
        chans = s.get("provides", [s["id"]])
        off   = len(aux_rows)
        for k, out in enumerate(outs):
            if 1 + k >= len(chans):
                break                            # sensor doesn't bind this secondary output
            sig = chans[1 + k]
            sig_c = f"SIG_{sig.upper()}" if sig in channel_ids else "SIG_NONE"
            (x0, y0), (x1, y1) = (out.get("cal") or [[0, 0], [1, 1]])[:2]
            scale  = (float(y1) - float(y0)) / (float(x1) - float(x0)) if x1 != x0 else 0.0
            offset = float(y0) - float(x0) * scale
            rng = out.get("range") or {}
            od  = out.get("dtc") or {}
            aux_rows.append(
                f'    {{ {si}, {sig_c}, {aux_acq.get(out.get("raw", "pulse_us"), 0)}, '
                f'{repr(scale)}f, {repr(offset)}f, '
                f'{repr(float(rng.get("min", 0.0)))}f, {repr(float(rng.get("max", 0.0)))}f, '
                f'0x{_dtc_to_u16(od.get("operating_min")):04X}, 0x{_dtc_to_u16(od.get("operating_max")):04X}, '
                f'{int(out.get("severity", 1))} }},  // {s["id"]} -> {sig}')
        aux_span.append((off, len(aux_rows) - off))
    lines += ["// One secondary output: its own pipeline, reusing the standard stages.",
              "struct SensorAuxOutput {",
              "    uint8_t  sensor_index;   // parent sensor (source pin + filter come from its config)",
              "    uint16_t signal;         // SignalId published",
              "    uint8_t  acquire;        // AuxAcquireKind (0 = pulse width, 1 = frequency)",
              "    float    scale, offset;  // raw -> engineering (decode_linear)",
              "    float    range_min, range_max;   // operating-window (always armed)",
              "    uint16_t dtc_op_min, dtc_op_max;",
              "    uint8_t  severity;       // raised severity for an out-of-range fault",
              "};",
              f"static constexpr uint8_t SENSOR_AUX_COUNT = {len(aux_rows)};",
              "static const SensorAuxOutput SENSOR_AUX_OUTPUTS[SENSOR_AUX_COUNT ? SENSOR_AUX_COUNT : 1] = {"]
    lines += aux_rows or ["    { 0, SIG_NONE, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0x0000, 0x0000, 0 },"]
    lines += ["};", "",
              "// Calibration breakpoints per sensor — FIXED (the TS curve editor binds all of them).",
              "// The decode curve interpolates over exactly this many cal_raw_*/cal_val_* points.",
              f"static constexpr uint8_t SENSOR_CAL_POINTS = {cal_points};",
              "",
              f"static constexpr uint8_t SENSOR_COUNT = {len(sensors)};",
              "static const SensorDescriptor SENSOR_CATALOG[SENSOR_COUNT] = {"]
    # Per-sensor config-error DTC: auto-allocated in the manufacturer range, one contiguous code per
    # sensor index (P1800..). Manufacturer (P1xxx) is the custom range; P17xx is the module_dtc cluster,
    # so this block sits just above it. Guard the block is free of any explicit schema code AND fits.
    used_codes = {int(c, 16) for c in re.findall(r'\b[PCBU]([0-9A-Fa-f]{4})\b',
                                                  yaml.safe_dump(schema or {}))}
    if SENSOR_DTC_CONFIG_BASE + len(sensors) - 1 > 0x18FF:
        raise SystemExit(f"sensor dtc_config block overflows P18FF ({len(sensors)} sensors)")
    for i in range(len(sensors)):
        if (SENSOR_DTC_CONFIG_BASE + i) in used_codes:
            raise SystemExit(f"sensor dtc_config P{SENSOR_DTC_CONFIG_BASE+i:04X} collides with an "
                             f"explicit schema P-code; move SENSOR_DTC_CONFIG_BASE")
    diag_dtc = sensor_diag_dtcs(schema, sensor_types, sensors)
    for i, (s, (off, count), (aoff, acount)) in enumerate(zip(sensors, provides_span, aux_span)):
        prim_sig = provides_pool[off]
        mask = 0
        for iface in sensor_offered_interfaces(schema, s):
            mask |= iface_bit.get(iface, 0)
        locked = iface_bit.get(s.get("locked"), 0)
        d = diag_dtc[s["id"]]
        lines.append(
            f'    {{ "{s["id"]}", "{s["name"]}", {sensor_type_token(s)}, '
            f'SENSOR_GROUP_{s.get("group", "other").upper()}, {prim_sig}, '
            f'0x{mask:02X}, 0x{locked:02X}, {off}, {count}, {aoff}, {acount}, '
            f'0x{d["raw_min"]:04X}, 0x{d["raw_max"]:04X}, '
            f'0x{d["operating_min"]:04X}, 0x{d["operating_max"]:04X}, '
            f'0x{d["stuck"]:04X}, 0x{d["max_derivative"]:04X}, '
            f'0x{d["precond_invalid"]:04X}, '
            f'0x{SENSOR_DTC_CONFIG_BASE + i:04X}, '
            f'0x{d.get("no_band", 0):04X}, '
            f'}},')
    lines += ["};", "",
              "// The type a sensor is actually read as. A catalogued sensor's type is fixed and the",
              "// tune's `type` byte is ignored for it — only a `generic` input (one with no type yet)",
              "// takes its type from the tune. Same shape as active_iface() applying the catalog's",
              "// interface lock: the catalog wins wherever it has an opinion.",
              "inline uint8_t effective_type(const SensorDescriptor& d, uint8_t cfg_type) {",
              "    if (d.type != SENSOR_TYPE_NONE) return d.type;   // settled at build time",
              "    return (cfg_type < SENSOR_TYPE_COUNT && SENSOR_TYPE_CATALOG[cfg_type].selectable)",
              "         ? cfg_type : static_cast<uint8_t>(SENSOR_TYPE_NONE);   // not configured yet",
              "}", "",
              f"#endif // {guard}", ""]
    return "\n".join(lines)

# -----------------------------------------------------------------------
# Per-module config header (supports scalars, tables, and config_arrays)
# -----------------------------------------------------------------------

def _elem_member_decl(prim_types: dict, f: dict) -> tuple:
    """(c_type, declarator) for one element scalar/enum/bool/string field."""
    if f.get("type") in ("enum", "bool"):
        return "uint8_t", f["name"]
    if f.get("type") == "string":
        return "char", f"{f['name']}[{int(f['length'])}]"
    if f.get("type") == "expression":
        return "uint8_t", f"{f['name']}[{int(f.get('length', EXPR_PROGRAM_MAX))}]"
    return c_type(prim_types, f["type"]), f["name"]

def gen_config_array_structs(prim_types: dict, config_arrays: list) -> list[str]:
    """Return C lines defining element structs for all config_arrays. Element `type: table` /
    `type: array` fields (desugared by expand_element_primitives) emit as REAL arrays / nested
    structs via the recorded `_element_layout` plan — byte-identical to the flat run the
    offset/serializer machinery sees, so sizeof still matches array_element_size."""
    out = []
    for arr in config_arrays:
        elem_name = array_name_pascal(arr["name"]) + "Config"
        elem_size = array_element_size(prim_types, arr)
        plan = arr.get("_element_layout")

        # Nested sub-struct types (precond/cand …) first — the element struct references them.
        for item in (plan or []):
            if item["kind"] != "struct_array":
                continue
            out.append(f"struct {item['struct_name']} {{")
            for sf in item["subfields"]:
                ct, decl = _elem_member_decl(prim_types, sf)
                out.append(f"    {ct:<12} {decl};  // {sf.get('label', sf['name'])}")
            out.append("};")
            sub_sz = sum(field_size(prim_types, sf) for sf in item["subfields"])
            out.append(f"static_assert(sizeof({item['struct_name']}) == {sub_sz},")
            out.append(f'             "Sub-element size mismatch for {item["struct_name"]}");')
            out.append("")

        out.append(f"struct {elem_name} {{")
        if plan:
            for item in plan:
                if item["kind"] == "scalar":
                    f = item["field"]
                    ct, decl = _elem_member_decl(prim_types, f)
                    out.append(f"    {ct:<12} {decl};" + ("" if is_pad(f) else f"  // {f.get('label', f['name'])}"))
                elif item["kind"] == "prim_array":
                    ct = c_type(prim_types, item["ctype"])
                    out.append(f"    {ct:<12} {item['handle']}[{item['len']}];  // {item['handle']}")
                elif item["kind"] == "struct_array":
                    out.append(f"    {item['struct_name']:<12} {item['handle']}[{item['len']}];")
        else:                                            # no plan (shouldn't happen post-expand) — flat
            for f in arr["element"]:
                ct, decl = _elem_member_decl(prim_types, f)
                out.append(f"    {ct:<12} {decl};" + ("" if is_pad(f) else f"  // {f.get('label', f['name'])}"))
        out.append("};")
        out.append(f"static_assert(sizeof({elem_name}) == {elem_size},")
        out.append(f'             "Element size mismatch for {elem_name}");')
        out.append("")
    return out

def _config_decl_line(prim_types: dict, mod_name: str, kind: str, d: dict) -> str:
    """One C struct-member declaration for a config scalar/table/array. Shared by
    the module config struct AND the subset shadow struct so the two never drift —
    a shadowed field declares identically in both. Table members reference the
    module header's <PFX>_<NAME>_ALLOC constant; array members the element struct."""
    pfx = module_snake(mod_name).upper()
    if kind == "scalar":
        label = d.get("label", d["name"])
        if d.get("type") == "string":
            return f"    char         {d['name']}[{int(d['length'])}];  // {label}"
        if d.get("type") == "expression":
            n = int(d.get("length", EXPR_PROGRAM_MAX))
            return f"    uint8_t      {d['name']}[{n}];  // {label} (expr bytecode)"
        if d.get("type") in ("enum", "bool"):
            return f"    uint8_t      {d['name']};  // {label}"
        ct = c_type(prim_types, d["type"])
        return f"    {ct:<12} {d['name']};  // {label}"
    if kind == "table":
        ct = c_type(prim_types, d["type"])
        return f"    {ct:<12} {d['name']}[{pfx}_{d['name'].upper()}_ALLOC];  // {d['label']}"
    if kind == "array":
        cnt = f"{pfx}_{d['name'].upper()}_COUNT"   # named element-count constant (see gen_module_config_h)
        return f"    {array_name_pascal(d['name']) + 'Config'} {d['name']}[{cnt}];"
    raise ValueError(f"unknown config member kind '{kind}'")

def gen_module_config_h(prim_types: dict, mod_name: str,
                         config: list, tables: list,
                         config_arrays: list = None) -> str:
    if config_arrays is None:
        config_arrays = []
    ms = module_snake(mod_name)
    struct_name = f"{mod_name}Config"
    guard = f"JAYECU_GENERATED_{ms.upper()}_CONFIG_H"
    sz = struct_size(prim_types, config, tables, config_arrays)

    lines = [
        BANNER,
        "#pragma once",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        "#include <cstdint>",
        "",
        "#pragma pack(push, 1)",
        "",
    ]

    # Emit element structs first (before the module struct that uses them)
    if config_arrays:
        lines += gen_config_array_structs(prim_types, config_arrays)

    # Derived MAX allocation constants for tables — firmware/tests reference
    # these instead of a literal. Tables are stored FLAT (max-sized); the live
    # row/col count is a runtime <axis>_n scalar.
    pfx = ms.upper()
    for t in tables:
        alloc = t.get("_alloc_elems",
                       t["rows"] * t["cols"] if "rows" in t else t["size"])
        tu = t["name"].upper()
        if "rows" in t:
            lines.append(f"static constexpr unsigned {pfx}_{tu}_MAX_COLS = {t.get('_max_cols', t['cols'])};")
            lines.append(f"static constexpr unsigned {pfx}_{tu}_MAX_ROWS = {t.get('_max_rows', t['rows'])};")
            if "_max_depth" in t:
                lines.append(f"static constexpr unsigned {pfx}_{tu}_MAX_DEPTH = {t['_max_depth']};")
        lines.append(f"static constexpr unsigned {pfx}_{tu}_ALLOC = {alloc};")
    # Element-count constants for config_arrays — the array dimension is a NAMED
    # constant (mirroring a table's _ALLOC) so firmware references e.g.
    # SENSORS_ANALOG_COUNT instead of re-hardcoding the row count. Single source of
    # truth: the schema `count:`.
    for arr in config_arrays:
        lines.append(f"static constexpr unsigned {pfx}_{arr['name'].upper()}_COUNT = {arr['count']};")
    if tables or config_arrays:
        lines.append("")

    lines.append(f"struct {struct_name} {{")

    for f in config:
        lines.append(_config_decl_line(prim_types, mod_name, "scalar", f))

    # Tables are FLAT max-sized buffers (row-major, stride = live cols).
    for t in tables:
        lines.append(_config_decl_line(prim_types, mod_name, "table", t))

    for slot in array_union_slots(config_arrays):
        if len(slot) > 1:
            # Mutually-exclusive pattern buffers → overlay in one anonymous union.
            # Members keep their names (anonymous union members sit at struct scope),
            # so cfg.<array>[i] access is unchanged; they just share storage.
            lines.append(f"    union {{   // overlay: '{slot[0]['union']}' (one active strategy)")
            for arr in slot:
                lines.append("    " + _config_decl_line(prim_types, mod_name, "array", arr))
            lines.append("    };")
        else:
            lines.append(_config_decl_line(prim_types, mod_name, "array", slot[0]))

    lines += [
        "};",
        "#pragma pack(pop)",
        "",
        f"static_assert(sizeof({struct_name}) == {sz},",
        f'             "Schema/struct size mismatch for {struct_name}");',
        "",
        f"#endif // {guard}",
        "",
    ]
    return "\n".join(lines)

# -----------------------------------------------------------------------
# ecu_telemetry.h
# -----------------------------------------------------------------------

def gen_ecu_telemetry_h(prim_types: dict, telem: list) -> str:
    guard = "JAYECU_GENERATED_ECU_TELEMETRY_H"
    struct_lines = []
    total = 0

    # Flat: one field per channel, in channel order (telemetry_fields() — derived from the signals
    # catalog + raw ADC, NOT a module section). No per-module sub-structs; the packer writes these
    # fields directly.
    for f in telem:
        ct = c_type(prim_types, f["type"])
        struct_lines.append(f"    {ct:<12} {f['name']};  // offset {total}")
        total += field_size(prim_types, f)

    lines = [
        BANNER,
        "#pragma once",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        "#include <cstdint>",
        "",
        "// Flat packed telemetry frame — one field per channel; layout = TS OutputChannels.",
        "// Packed by comms (and CAN broadcasters) from the SignalBus on demand.",
        "#pragma pack(push, 1)",
        "struct EcuTelemetry {",
    ] + struct_lines + [
        "};",
        "#pragma pack(pop)",
        "",
        f"static constexpr unsigned ECU_TELEMETRY_SIZE = {total};",
        f"static_assert(sizeof(EcuTelemetry) == ECU_TELEMETRY_SIZE,",
        '             "EcuTelemetry size mismatch");',
        "",
        f"#endif // {guard}",
        "",
    ]
    return "\n".join(lines)

# -----------------------------------------------------------------------
# ecu_config.h
# -----------------------------------------------------------------------

def gen_ecu_config_h(prim_types: dict, modules: dict) -> str:
    guard = "JAYECU_GENERATED_ECU_CONFIG_H"
    includes = []
    struct_lines = []
    total = 4  # layout_hash (field 0)

    for mod_name, mod in modules.items():
        if mod_name == "System":
            continue
        cfg    = mod.get("config", [])
        tbls   = config_tables(mod)
        arrays = mod.get("config_arrays", [])
        if not cfg and not tbls and not arrays:
            continue
        ms = module_snake(mod_name)
        includes.append(f'#include "modules/{ms}_config.h"')
        sz = struct_size(prim_types, cfg, tbls, arrays)
        struct_lines.append(f"    // {mod_name} config (offset {total}, {sz} bytes)")
        struct_lines.append(f"    {mod_name}Config  {ms};")
        total += sz

    # total = layout_hash + modules. No trailing CRC field: record integrity
    # is the storage ConfigHeader's CRC over the whole persisted record.

    lines = [
        BANNER,
        "#pragma once",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        "#include <cstdint>",
        '#include "schema_meta.h"   // JAYECU_CONFIG_SIZE (the externally-shared literal)',
    ] + includes + [
        "",
        "// Flat packed configuration. The struct is the single source of truth for",
        "// the config size (use sizeof(EcuConfig) / sizeof(g_config)); integrity is",
        "// the storage ConfigHeader CRC over the persisted record, not a field here.",
        "#pragma pack(push, 1)",
        "struct EcuConfig {",
        "    uint32_t     layout_hash;  // == JAYECU_LAYOUT_HASH; boot gate rejects a tune whose",
        "                               // stored hash differs (auto content hash, never hand-bumped)",
    ] + struct_lines + [
        "};",
        "#pragma pack(pop)",
        "",
        "// Tooling (the studio's meta, test scripts) can't call sizeof, so codegen",
        "// emits the size as a literal in schema_meta.h. Assert it matches the",
        "// struct so the two can never drift.",
        "static_assert(sizeof(EcuConfig) == JAYECU_CONFIG_SIZE,",
        '             "EcuConfig size drifted from JAYECU_CONFIG_SIZE");',
        "",
        "extern EcuConfig g_config;",
        "",
        f"#endif // {guard}",
        "",
    ]
    return "\n".join(lines)

# -----------------------------------------------------------------------
# schema_meta.h
# -----------------------------------------------------------------------

def _mod_shadow_default(mod: dict) -> bool:
    return bool((mod.get("shadow") or {}).get("default", False))

def _mod_shadow_when(mod: dict) -> str:
    return str((mod.get("shadow") or {}).get("when", "engine_stop"))

def is_shadow_field(f: dict, mod: dict) -> bool:
    """A field is shadowed if it (or its module default) sets `shadow`. ANY field may
    be shadowed — scalar, table, anything; it is not limited to 'structural' fields."""
    sf = f.get("shadow", _mod_shadow_default(mod))
    return bool(sf if not isinstance(sf, dict) else sf.get("default", True))

_SHADOW_WHENS = ("engine_stop", "reboot", "live")

def _field_shadow_when(f: dict, mod: dict) -> str:
    """A shadowed member's apply policy: a per-field `shadow: {when: ...}` override,
    else the module default (`shadow.when`, default 'engine_stop'). 'engine_stop' is
    refreshed into the shadow at the next stopped-engine reconfigure; 'reboot' is
    never shifted at runtime — the write lands in g_config RAM, is flashed on burn,
    and the shadow picks it up only at the next boot."""
    sf = f.get("shadow", None)
    if isinstance(sf, dict) and "when" in sf:
        return str(sf["when"])
    return _mod_shadow_when(mod)

def _module_shadow_members(prim_types: dict, modules: dict):
    """Per-module ordered member list (declaration order = struct layout order). Each
    entry is {kind, def, off, end, shadow, when}: kind in {scalar, table, array}, def
    the schema dict, [off,end) the GLOBAL g_config byte span, shadow the resolved flag,
    when the apply policy. Used by both the watch-region computation and the shadow
    struct emitter so they share one notion of what is shadowed. Array shadow honours a
    per-array `shadow` override, defaulting to the module default — ANY member may be
    shadowed, with ANY when."""
    _, scal, tbls, arrs, _, _ = collect_offsets(prim_types, modules)

    scal_off = {(mn, f["name"]): (off, off + field_size(prim_types, f))
                for (name, typ, off, f, mn) in scal}
    tbl_off  = {(mn, t["name"]): (off, off + table_size(prim_types, t))
                for (name, typ, off, t, mn) in tbls}
    arr_off = {}   # (mod, arr_name) -> [lo, hi] across all element bytes
    for (name, typ, off, f, mn, arr_name, idx) in arrs:
        end = off + field_size(prim_types, f)   # enum/bool=1, string=len
        cur = arr_off.get((mn, arr_name))
        arr_off[(mn, arr_name)] = [off, end] if cur is None else [min(cur[0], off), max(cur[1], end)]

    out = {}
    for mod_name, mod in modules.items():
        if mod_name == "System":
            continue
        members = []
        def add(kind, d, span):
            members.append({"kind": kind, "def": d, "off": span[0], "end": span[1],
                            "shadow": is_shadow_field(d, mod), "when": _field_shadow_when(d, mod)})
        for f in mod.get("config", []):
            add("scalar", f, scal_off[(mod_name, f["name"])])
        for t in config_tables(mod):
            add("table", t, tbl_off[(mod_name, t["name"])])
        for arr in mod.get("config_arrays", []):
            add("array", arr, arr_off[(mod_name, arr["name"])])
        out[mod_name] = members
    return out

def compute_shadow_regions(prim_types: dict, modules: dict):
    """Per-module shadow descriptors (declaration order), one per module with >=1
    shadowed member. Each is a dict:
        mod         — module name
        all_shadow  — every member of the module is shadowed
        members     — all shadowed members (struct order, for the shadow struct)
        es_span     — (lo,hi) contiguous engine_stop run = the runtime 'w' watch region,
                      or None if no engine_stop-shadowed member
        has_reboot  — at least one shadowed member has when=reboot
    Only engine_stop members get a watch region: a 'w' to a reboot field must NOT
    trigger a runtime reconfigure (it applies on the next boot). engine_stop members
    must be CONTIGUOUS — no live or reboot member may sit inside the es_span, else the
    'w' bounds classifier would mis-fire; that is a build error so they get grouped."""
    per_mod = _module_shadow_members(prim_types, modules)

    out = []
    for mod_name in modules:
        members = per_mod.get(mod_name, [])
        sh = [m for m in members if m["shadow"]]
        if not sh:
            continue                       # module has no shadowed members → omitted
        for m in sh:
            if m["when"] not in _SHADOW_WHENS:
                raise SystemExit(
                    f"ERROR: {mod_name}.{m['def']['name']} has shadow when='{m['when']}'; "
                    f"valid: {', '.join(_SHADOW_WHENS)}.")
        es = [m for m in sh if m["when"] == "engine_stop"]
        es_span = None
        if es:
            lo = min(m["off"] for m in es)
            hi = max(m["end"] for m in es)
            # Purity: nothing that ISN'T an engine_stop shadow member (live, or a reboot
            # shadow member) may overlap the watch span, or a 'w' to it would falsely
            # request a runtime reconfigure.
            for m in members:
                is_es = m["shadow"] and m["when"] == "engine_stop"
                if (not is_es) and m["off"] < hi and m["end"] > lo:
                    raise SystemExit(
                        f"ERROR: module '{mod_name}' has a non-engine_stop field inside its "
                        f"engine_stop watch span [{lo},{hi}). Group the engine_stop shadow "
                        f"fields contiguously (apart from live/reboot fields) so the 'w' "
                        f"classifier detects them by bounds.")
            es_span = (lo, hi)
        # 'live' members get their OWN watch region (so a 'w' into the block sets the module's
        # bit) but are applied IMMEDIATELY by the owning manager — NOT gated on engine-stop the way
        # engine_stop is (main.cpp's pos_bits only covers the structural modules). Used by the
        # runtime Outputs config so OutputManager rebuilds only when its region is written.
        live = [m for m in sh if m["when"] == "live"]
        live_span = None
        if live:
            live_span = (min(m["off"] for m in live), max(m["end"] for m in live))
        out.append({
            "mod": mod_name,
            "all_shadow": all(m["shadow"] for m in members),
            "members": sh,
            "es_span": es_span,
            "live_span": live_span,
            "has_reboot": any(m["when"] == "reboot" for m in sh),
        })
    return out

def _shadow_pull_stmt(kind: str, d: dict) -> str:
    """One refresh statement copying a shadowed member from live g_config into the
    shadow. Scalars assign; strings/tables/arrays (C arrays, can't assign) memcpy."""
    name = d["name"]
    if kind == "scalar" and d.get("type") not in ("string", "expression"):
        return f"    dst.{name} = live.{name};"
    return f"    memcpy(dst.{name}, live.{name}, sizeof(dst.{name}));"

def _needs_cstring(members: list) -> bool:
    return any(m["kind"] != "scalar" or m["def"].get("type") in ("string", "expression")
               for m in members)

def _subset_struct_lines(prim_types: dict, mod_name: str, members: list) -> list:
    """A PARTIAL module's shadow STRUCT: holds ONLY the shadowed members (their
    contiguous run). Live members stay read-direct from g_config — never copied here.
    Members declare identically to the module config struct (shared _config_decl_line)
    so a shadowed table/array keeps its type and ALLOC. Pulls are emitted separately."""
    shadow = f"{mod_name}Shadow"
    decls = []
    i = 0
    while i < len(members):
        m = members[i]
        u = m["def"].get("union") if m["kind"] == "array" else None
        if u:
            grp = [m]
            j = i + 1
            while (j < len(members) and members[j]["kind"] == "array"
                   and members[j]["def"].get("union") == u):
                grp.append(members[j]); j += 1
            if len(grp) > 1:
                decls.append(f"    union {{   // overlay: '{u}' (one active strategy)")
                for gm in grp:
                    decls.append("    " + _config_decl_line(prim_types, mod_name, "array", gm["def"]))
                decls.append("    };")
            else:
                decls.append(_config_decl_line(prim_types, mod_name, "array", m["def"]))
            i = j
        else:
            decls.append(_config_decl_line(prim_types, mod_name, m["kind"], m["def"]))
            i += 1
    return [
        f"// {mod_name}: PARTIAL shadow — only the schema-flagged shadow members are",
        f"// copied; every other field stays read-direct from live g_config.{module_snake(mod_name)}.",
        "#pragma pack(push, 1)",
        f"struct {shadow} {{",
    ] + decls + [
        "};",
        "#pragma pack(pop)",
    ]

def _pull_fn_lines(mod_name: str, suffix: str, members: list, use_alias: bool) -> list:
    """A <mod>_shadow_pull[suffix]() that copies `members` from live into the shadow.
    use_alias collapses to `dst = live` (only valid when the shadow IS the whole config
    AND `members` is every config member, i.e. an all-shadow module with no exclusions)."""
    ms = module_snake(mod_name)
    cfg = f"{mod_name}Config"
    shadow = f"{mod_name}Shadow"
    sig = f"inline void {ms}_shadow_pull{suffix}({shadow}& dst, const {cfg}& live) noexcept"
    if use_alias:
        return [f"{sig} {{ dst = live; }}"]
    return [f"{sig} {{"] + [_shadow_pull_stmt(m["kind"], m["def"]) for m in members] + ["}"]

def gen_shadow_meta_h(prim_types: dict, mods) -> str:
    """Codegen owns the shadow machinery: the engine_stop 'w' watch-region table AND the
    per-module config SHADOW types + pull/refresh helpers — all derived from the schema
    `shadow` flags, so flipping a field shadow/live/when regenerates them with no
    hand-editing. Two pulls per module:
      <mod>_shadow_pull          — FULL refresh (all shadowed members). Boot / first start.
      <mod>_shadow_pull_runtime  — engine_stop members only. Stopped-engine reconfigure;
                                   reboot members keep their boot values until next boot."""
    enum_lines, tbl_lines, inc_lines, shadow_lines = [], [], [], []
    seen_inc = set()
    need_cstring = False

    # Watch regions + enum: modules with an engine_stop span (structural, applied at the safe
    # stopped boundary) OR a live span (applied immediately by the owner). reboot-only modules have
    # no runtime watch — their writes never reconfigure. The region is the engine_stop span if
    # present, else the live span.
    region_mods = [d for d in mods if d["es_span"] is not None or d.get("live_span") is not None]
    for i, d in enumerate(region_mods):
        lo, hi = d["es_span"] if d["es_span"] is not None else d["live_span"]
        enum_lines.append(f"    JAYECU_SHADOW_{d['mod'].upper()} = {i},")
        tbl_lines.append(f"    {{ {lo}u, {hi}u }},  // {d['mod']}")

    # Shadow struct + pulls: EVERY module with a shadowed member (incl. reboot-only).
    for d in mods:
        mod_name = d["mod"]
        ms = module_snake(mod_name)
        cfg = f"{mod_name}Config"
        shadow = f"{mod_name}Shadow"
        members = d["members"]
        es_members = [m for m in members if m["when"] == "engine_stop"]
        inc = f'#include "modules/{ms}_config.h"'
        if inc not in seen_inc:
            inc_lines.append(inc); seen_inc.add(inc)

        if d["all_shadow"]:
            shadow_lines.append(f"using {shadow} = {cfg};")
        else:
            shadow_lines += _subset_struct_lines(prim_types, mod_name, members)
            need_cstring |= _needs_cstring(members)

        # FULL pull (boot): all shadowed members. alias only when the shadow is the whole
        # config (all_shadow) — then dst=live copies everything.
        shadow_lines += _pull_fn_lines(mod_name, "", members, use_alias=d["all_shadow"])
        # RUNTIME pull: engine_stop members only. alias only when that set == every config
        # member (all_shadow AND no reboot member excluded it).
        rt_alias = d["all_shadow"] and not d["has_reboot"]
        shadow_lines += _pull_fn_lines(mod_name, "_runtime", es_members, use_alias=rt_alias)
        if not rt_alias:
            need_cstring |= _needs_cstring(es_members)
        shadow_lines.append("")

    n_reg = len(region_mods)
    return "\n".join([
        BANNER,
        "#pragma once",
        "#include <cstdint>",
    ] + (["#include <cstring>   // field-wise shadow pull (memcpy of table/array members)"]
         if need_cstring else []) + inc_lines + [
        "",
        "// PER-MODULE shadow metadata, all derived from the schema `shadow` flags.",
        "//",
        "// Watch regions: a page ('w') write whose [offset,size) intersects a module's",
        "// engine_stop [lo,hi) means THAT module has a structural field changed — the bytes",
        "// are in g_config RAM, but the owning subsystem reads a private SHADOW and only",
        "// pulls the new bytes at its safe boundary (reconfigure() with the engine stopped).",
        "// reboot-flagged shadow fields are deliberately NOT watched: a 'w' to one lands in",
        "// g_config RAM, is flashed on burn, and is picked up by the shadow only on the next",
        "// boot — never shifted in at runtime. Detection is per module so a write to one",
        "// never triggers another's reconfigure.",
        "//",
        "// Shadows: each module's config SHADOW type + two pulls. A module reads its shadow",
        "// in the hot path; it calls <mod>_shadow_pull() once at boot (full refresh, incl.",
        "// reboot fields from the flashed tune) and <mod>_shadow_pull_runtime() at each",
        "// stopped-engine reconfigure (engine_stop fields only). Modules with no shadowed",
        "// fields are omitted.",
        "",
        "enum JayecuShadowModule {",
    ] + enum_lines + [
        f"    JAYECU_SHADOW_MODULE_COUNT = {n_reg}",
        "};",
        "",
        # 32-bit offsets: the config page can exceed 65535 bytes (Use32BitOffsets), so a region's
        # [lo,hi) byte range must hold a full 32-bit offset — uint16 would truncate any region
        # above 64 KB and mis-classify writes there.
        "struct JayecuShadowRegion { uint32_t lo; uint32_t hi; };",
        "",
        f"static constexpr JayecuShadowRegion JAYECU_SHADOW_REGIONS[{max(1,n_reg)}] = {{",
    ] + (tbl_lines or ["    { 0u, 0u },  // (none)"]) + [
        "};",
        "",
    ] + shadow_lines)

def gen_schema_meta_h(schema: dict, total_telem: int, total_cfg: int,
                      layout_hash: int) -> str:
    ver = schema.get("schema_version", "0.0")
    lines = [
        BANNER,
        "#pragma once",
        "#ifndef JAYECU_GENERATED_SCHEMA_META_H",
        "#define JAYECU_GENERATED_SCHEMA_META_H",
        "",
        f'#define JAYECU_SCHEMA_VERSION  "{ver}"',
        "// Content hash of the config byte-layout (compute_layout_hash). EcuConfig field 0;",
        "// the boot gate accepts a stored tune only if its stored hash == JAYECU_LAYOUT_HASH.",
        "// _STR is the same value as an 8-char hex literal for the wire signature.",
        f"#define JAYECU_LAYOUT_HASH     0x{layout_hash:08x}u",
        f'#define JAYECU_LAYOUT_HASH_STR "{layout_hash:08x}"',
        f"#define JAYECU_TELEMETRY_SIZE  {total_telem}u",
        f"#define JAYECU_CONFIG_SIZE     {total_cfg}u",
        f"#define JAYECU_BLOCK_SIZE      {BLOCK_SIZE}u",
        "",
        "#endif // JAYECU_GENERATED_SCHEMA_META_H",
        "",
    ]
    return "\n".join(lines)


# -----------------------------------------------------------------------
# Offset collection (for JSON generation)
# -----------------------------------------------------------------------

def collect_offsets(prim_types, modules, telem=None):
    """Assign byte offsets. CONFIG offsets come from the modules; TELEMETRY offsets are assigned to
    the `telem` field list (from telemetry_fields() — derived from signals, NOT a module section).
    Callers that only need config offsets pass no telem and get an empty telem list back."""
    telem_fields   = []
    config_scalars = []
    config_tables_out = []
    config_arrays_out = []

    telem_offset = 0
    for f in (telem or []):
        # The meta "module" groups the studio Dictionary's Telemetry tree. Use each signal's real
        # category (e.g. IgnitionController / BoostControl / Sensors - Air Con) — NOT a blanket
        # "Sensors" (rpm/advance/boost_target aren't sensors). Cosmetic: not in layout_hash.
        telem_fields.append((f["name"], f["type"], telem_offset, f, f.get("category") or "Uncategorised"))
        telem_offset += field_size(prim_types, f)

    cfg_offset = 4  # after layout_hash (uint32, field 0)
    for mod_name, mod in modules.items():
        if mod_name == "System":
            continue
        for f in mod.get("config", []):
            config_scalars.append((const_name(mod_name, f["name"]), f["type"], cfg_offset, f, mod_name))
            cfg_offset += field_size(prim_types, f)
        # A learned table's AXIS array is an ordinary config array and is walked above like any other:
        # the breakpoints are tunable and flashed; only the learned CELLS are not.
        for t in config_tables(mod):
            config_tables_out.append((const_name(mod_name, t["name"]), t["type"], cfg_offset, t, mod_name))
            cfg_offset += table_size(prim_types, t)
        for slot in array_union_slots(mod.get("config_arrays", [])):
            slot_base = cfg_offset                 # union members all start here (overlay)
            for arr in slot:
                elem_sz = array_element_size(prim_types, arr)
                for i in range(arr["count"]):
                    field_off = slot_base + i * elem_sz
                    for f in arr["element"]:
                        config_arrays_out.append((
                            const_name(mod_name, f"{arr['name']}_{i}_{f['name']}"),
                            f["type"], field_off, f, mod_name,
                            arr["name"], i,
                        ))
                        field_off += field_size(prim_types, f)   # enum/bool=1, string=len
            cfg_offset += array_slot_size(prim_types, slot)      # advance once, by the max

    return telem_fields, config_scalars, config_tables_out, config_arrays_out, \
           telem_offset, cfg_offset


def compute_layout_hash(prim_types, modules, signals=None, telem=None) -> int:
    """Deterministic 32-bit content hash of everything that decides what the ECU's bytes
    MEAN — the on-flash identity that replaces the old hand-bumped config_version. Three
    things go in, and each of them changes the meaning of bytes already stored or on the
    wire:

      CONFIG LAYOUT   each field's (kind, offset, size, primitive-type, qualified-name),
                      table MAX dims, total config size. A field moves, the hash moves.

      THE SIGNAL MAP  because a `kind: signal` config field stores a raw SignalId IN THE
                      CONFIG BYTES — signal_ids.lock says so itself, "stored raw in every
                      tune's kind:'signal' selector". Renumber the catalogue and those
                      bytes silently point somewhere else: a brake input reading a cruise
                      demand, with nothing anywhere saying the meta changed. That was a
                      real hole. The hash covered the box and not what was written in it.

      TELEMETRY LAYOUT  each channel's (offset, size, type). The studio decodes the wire
                      by these; a channel that moves makes every later one read garbage,
                      and the old meta looked equally valid because the config had not
                      moved.

    Still deliberately NOT a function of labels/units/scale/min/max/defaults or of
    git/build metadata — those change presentation, not meaning — so a no-op rebuild
    reproduces the hash exactly and the firmware still accepts its own saved tune.
    Truncated to 32 bits to fit EcuConfig field 0 (uint32); at the handful-of-stored-
    layouts scale that is collision-safe."""
    telem_fields, scal, tbls, arrs, _tsize, cfg_size = collect_offsets(prim_types, modules, telem)
    # HOW CELLS SIT INSIDE A TABLE is part of the byte-layout contract, and it is the one part that is
    # not implied by an offset or a size. Rev 1 packed a table's cells to its LIVE size (<axis>_n), so
    # the same bytes meant different rows depending on a count stored elsewhere; rev 2 lays them at the
    # ALLOCATION stride, which is what makes a resize move nothing. An image written under one and read
    # under the other is silently wrong in every row but the first — nothing in the offsets moves, so
    # without this the hash would call the two identical and the boot gate would accept either.
    rows = [["cell-layout-rev", 2]]
    for (name, typ, off, f, _mod) in scal:
        rows.append(["s", off, field_size(prim_types, f), typ, name])
    for (name, typ, off, t, _mod) in tbls:
        rows.append(["t", off, table_size(prim_types, t), typ, name,
                     t.get("_max_cols"), t.get("_max_rows"), t.get("_max_depth")])
    for (name, typ, off, f, _mod, arr_name, idx) in arrs:
        rows.append(["a", off, field_size(prim_types, f), typ, name, arr_name, idx])
    # The signal catalogue, by name and index. Not the labels — the ID, which is the number
    # a selector field holds.
    for i, c in enumerate(signals or []):
        rows.append(["g", i, c["id"]])
    # The telemetry wire layout, by offset and width.
    for (name, typ, off, f, _cat) in telem_fields:
        rows.append(["y", off, field_size(prim_types, f), typ, name])
    rows = sorted(json.dumps(r, separators=(",", ":")) for r in rows)
    canon = json.dumps([cfg_size, rows], separators=(",", ":"))
    return int.from_bytes(hashlib.sha256(canon.encode()).digest()[:4], "big")

# -----------------------------------------------------------------------
# INI generation
# -----------------------------------------------------------------------

def _read_version_h() -> tuple:
    """Read firmware/version.h (written by codegen/update_version.sh) and
    return (version, git_hash, board_profile). Returns sane defaults
    if the file isn't present. (No build number — signature is version + hash.)"""
    ver_path = Path(__file__).resolve().parent.parent / "firmware" / "version.h"
    if not ver_path.exists():
        return ("0.1.0", "unknown", "jayecu")
    txt = ver_path.read_text()
    m_v = re.search(r'JAYECU_FIRMWARE_VERSION\s+"([^"]+)"', txt)
    m_g = re.search(r'JAYECU_GIT_HASH\s+"([^"]+)"', txt)
    m_p = re.search(r'JAYECU_BOARD_PROFILE\s+"([^"]+)"', txt)
    return (
        m_v.group(1) if m_v else "0.1.0",
        m_g.group(1) if m_g else "unknown",
        m_p.group(1) if m_p else "jayecu",
    )


def synthesize_sensor_defaults(schema: dict, modules: dict, board: str | None = None) -> None:
    """Per-sensor defaults derived from the catalog (index-aligned lists on the sensor-array fields):
      • `interface` defaults to the sensor's PRIMARY interface index (the catalog lock, else the
        lowest-bit allowed interface) — there's no 'Default' selector, so an enabled sensor starts on
        an interface it actually supports.
      • `enabled` defaults to 1 ONLY for board-managed (`locked: on_board`) sensors — battery/ecu_temp
        are always-on board features (the HAL reads them regardless), so the tune should reflect that
        they're on, not a misleading unchecked box. Sensors locked to a wired interface (e.g. MAP on
        engine_sync_voltage) are still user-wired and default disabled (0), like any user sensor.
    Run after normalize_module_config."""
    sensors = schema.get("sensors", [])
    mod = modules.get("Sensors")
    if not sensors or mod is None:
        return
    # Idempotent: this runs from BOTH main() (before the default-image generators) and gen_ini().
    # Re-applying would corrupt the cal `base = f["default"]` read (it would see a list), so bail if
    # a per-sensor list default is already in place.
    sensor_arr = next((a for a in mod.get("config_arrays", []) if a.get("name") == "sensor"), None)
    if sensor_arr and any(f.get("name") == "interface" and isinstance(f.get("default"), list)
                          for f in sensor_arr.get("element", [])):
        return
    idx_of = {iid: i for i, iid in enumerate(enum_ids(schema, "sensor_interface"))}
    def primary(s):
        locked = s.get("locked")
        if locked in idx_of:
            return idx_of[locked]
        cand = [idx_of[i] for i in (s.get("interfaces") or []) if i in idx_of]
        return min(cand) if cand else 0
    iface_defaults   = [primary(s) for s in sensors]
    # A sensor wired to a DEDICATED board pin (`source_pin:`, e.g. battery on AV12) is always
    # present, so it defaults ON like a board-managed sensor.
    enabled_defaults = [1 if (s.get("locked") == "on_board" or s.get("source_pin")) else 0
                        for s in sensors]

    _at_base = len(_board_pins_by_cap(board).get("ANALOG_VOLTAGE", [])) if board else 0
    _batt = _board_battery(board)
    def pool_index(pin: str):
        """Analog-pool index for a dedicated `source_pin`.

        "@battery" asks the BOARD which pin that is. A literal "AV12" resolved arithmetically to
        11 without ever checking the board had such a pin, let alone that the battery was on it —
        so it was right on jaytek_v1 and right on proteus_f7 only by the coincidence that both put
        their battery at index 11. A board that placed it anywhere else would have bound this
        sensor, silently, to whatever ordinary analog input happened to sit at 11.

        Plain AVn/ATn still work for anything genuinely fixed to a numbered pin; ATn's base is the
        board's voltage-pin count (BOARD_ANALOG_T_BASE), not a literal 16.
        """
        if not pin:
            return None
        if pin == "@battery":
            if not _batt:
                sys.exit("ERROR: a sensor asks for source_pin '@battery' but "
                         f"definition/boards/{board}.board.yaml declares no ANALOG_VOLTAGE pin "
                         "with a `divider` — that pin IS the battery sense.")
            return _batt["index"]
        n = int(pin[2:]) - 1
        return n if pin.startswith("AV") else _at_base + n if pin.startswith("AT") else None
    source_defaults = [pool_index(s.get("source_pin")) if s.get("source_pin") else -1   # int8 source: -1 = unassigned
                       for s in sensors]

    # Per-sensor DEFAULT calibration curve: a sensor with `default_cal: [[raw, eng], ...]` ships its
    # own breakpoints (e.g. the battery's raw_mV -> V divider line) at their NATURAL point count — a
    # 2-point linear sensor stores exactly 2 points (cal_n = 2), no interpolation to the max. Cells
    # past cal_n are held flat (last value) — the firmware reads only the live cal_n points. eng is
    # converted to the stored int via the type's scale. Sensors without default_cal keep the table's
    # generic fallback (already on the flat cal_n / cal_raw_* / cal_val_* fields from the expander).
    type_dec = {t["id"]: int(t.get("decimals", 2)) for t in schema.get("sensor_types", [])}
    n_cal = sum(1 for f in next((a for a in mod.get("config_arrays", []) if a.get("name") == "sensor"),
                                {}).get("element", []) if f.get("name", "").startswith("cal_raw_"))
    # The ECU is ADC-counts-native; cal raws are AUTHORED in mV (human-meaningful in the schema), so bake
    # them to counts HERE, at the one boundary. counts = mV * full_scale / fullscale_mv, picking the AV or
    # AT front-end full-scale by the sensor's interface. Non-analog raws (freq Hz / pulse µs / SENT 12-bit)
    # are already native units — left untouched. The studio converts counts->V the other way for display.
    adc = _board_adc(board)
    def mv_to_counts(mv, fs):
        return int(round(mv * adc["full_scale"] / fs)) if (fs and adc.get("full_scale")) else int(round(mv))
    def raw_to_counts(s, mv):
        ifaces = s.get("interfaces") or []
        fs = (adc.get("at_mv") if "analog_temp" in ifaces else
              adc.get("av_mv") if any(i in ifaces for i in ("analog_voltage", "engine_sync_voltage")) else None)
        return mv_to_counts(mv, fs)
    av_counts = lambda mv: mv_to_counts(mv, adc.get("av_mv"))   # generic (AV front-end) for shared fallbacks
    type_cal = {t["id"]: t.get("default_cal") for t in schema.get("sensor_types", [])}
    def cal_curve(s):
        # THE SENSOR'S OWN CURVE, ELSE ITS TYPE'S. Without the second half every frequency pickup fell
        # to the table's generic [[0, 0], [5000, 1000]] — a 0.2 slope, which is right for nothing and
        # silently wrong for a pickup: 1200 Hz at the pin published as 240. A type that HAS a natural
        # shape says so once, rather than nine sensors repeating it and a tenth forgetting.
        dc = s.get("default_cal") or type_cal.get(s.get("type"))
        # "@battery": DERIVE the divider line from the board instead of writing it out. The raw axis
        # is connector mV (0 .. analog_av_fullscale_mv, which is full scale at the ADC pin), so the
        # engineering end is simply vref / divider — 3.3/0.1091 = 30.25 V on jaytek_v1 and
        # 3.3/0.1087 = 30.36 V on proteus_f7. It was written out as [[0,0],[5000,30.25]], which is
        # jaytek's divider sitting in the board-agnostic firmware schema: correct for one board and
        # quietly ~0.4% out on the next, with nothing anywhere to catch it.
        if dc == "@battery":
            if not _batt or not adc.get("av_mv"):
                sys.exit("ERROR: a sensor asks for default_cal '@battery' but the board declares no "
                         "battery-sense pin (an ANALOG_VOLTAGE pin with a `divider`).")
            full_v = (adc["vref_mv"] / 1000.0) / _batt["divider"]
            dc = [[0, 0.0], [adc["av_mv"], round(full_v, 2)]]
        if not dc or n_cal < 2:
            return None                                    # -> generic fallback
        pts  = sorted((float(r), float(v)) for r, v in dc)
        vs   = 10.0 ** -type_dec.get(s.get("type"), 2)
        n    = max(2, min(n_cal, len(pts)))
        raws = [raw_to_counts(s, r) for r, _ in pts][:n]   # mV -> ADC counts (analog sensors)
        vals = [int(round(v / vs)) for _, v in pts][:n]
        raws += [raws[-1]] * (n_cal - len(raws))           # hold last value across the unused tail
        vals += [vals[-1]] * (n_cal - len(vals))
        return n, raws, vals                               # (live count, raw bins, value bins)
    sensor_cal = [cal_curve(s) for s in sensors]           # (n, raws, vals) per sensor, or None

    # Per-element value scale + units for a `type_scaled` element table (the calibration curve): the
    # element struct is shared by all sensors, but the engineering scale/units are PER TYPE (°C@0.1,
    # kPa@0.1, λ@0.01…). Attach the per-sensor lists to the table descriptor so the studio shows each
    # sensor's cal in its own units — the type's units/decimals flow through, not raw stored counts.
    type_units = {t["id"]: t.get("units", "") for t in schema.get("sensor_types", [])}
    # What a sensor's RAW reading is measured in, by the type's `raw:` kind. Analog raws are stored as
    # ADC counts (the studio converts to mV/V from there); a frequency raw is hertz and needs no
    # conversion at all.
    type_raw = {t["id"]: t.get("raw", "volts") for t in schema.get("sensor_types", [])}
    type_index = {t["id"]: i for i, t in enumerate(schema.get("sensor_types", []))}
    elem_scales = [10.0 ** -type_dec.get(s.get("type"), 2) for s in sensors]
    elem_units  = [type_units.get(s.get("type"), "") for s in sensors]

    for arr in mod.get("config_arrays", []):
        if arr.get("name") != "sensor":
            continue
        for td in arr.get("_element_tables", []):
            if td.get("std"):
                continue                                   # standard multi-axis form: no per-type value scaling
            if td["value"].get("type_scaled"):
                td["value"]["element_scales"] = list(elem_scales)
                td["value"]["element_units"]  = list(elem_units)
                # AND THE RAW AXIS IS NOT ALWAYS VOLTS. It is labelled ADC for every sensor because most
                # of them are analog — but a frequency sensor's raw IS Hz, and a curve whose axis says
                # "ADC counts" over breakpoints in hertz tells the reader the wrong thing about the one
                # number they are entering. The type already says which: `raw: volts | hz | state`.
                td["axis"]["element_units"] = [RAW_UNITS.get(type_raw.get(s.get("type")), "ADC")
                                               for s in sensors]
        for f in arr.get("element", []):
            name = f.get("name", "")
            if name == "interface":
                f["default"] = iface_defaults
            elif name == "type":
                # Seed each row with its own catalog type, so a fixed-type sensor's stored value
                # agrees with the type the firmware will use for it anyway.
                f["default"] = [255 if s.get("type") == NO_TYPE else type_index.get(s.get("type"), 0)
                                for s in sensors]
            elif name == "enabled":
                f["default"] = enabled_defaults
            elif name == "source":
                f["default"] = source_defaults
            elif name == "cal_n":
                base = f["default"]                        # generic fallback point count
                f["default"] = [(sensor_cal[i][0] if sensor_cal[i] else base)
                                for i in range(len(sensors))]
            elif name.startswith("cal_raw_") or name.startswith("cal_val_"):
                k    = int(name.rsplit("_", 1)[1])
                is_raw = name.startswith("cal_raw_")
                sel  = 1 if is_raw else 2
                base = av_counts(f["default"]) if is_raw else f["default"]   # mV fallback raw -> counts
                f["default"] = [(sensor_cal[i][sel][k] if sensor_cal[i] else base)
                                for i in range(len(sensors))]


def bake_raw_adc_fields(modules: dict, board: str | None) -> None:
    """`raw_adc: true` config scalars are AUTHORED in mV — human-useful in the schema — but the ECU is
    ADC-counts-native, so bake their default + range to counts HERE and relabel units 'ADC' (the studio
    converts counts <-> mV/V back for display). counts = mV * full_scale / av_mv. Same one-boundary idea
    as cal_raw: humans read/write mV; the firmware only ever sees counts. Run after the per-sensor
    defaults are in place (it tolerates either a scalar or an already-expanded per-element list)."""
    adc = _board_adc(board)
    fs, av = adc.get("full_scale"), adc.get("av_mv")
    if not (fs and av):
        return
    to_counts = lambda mv: int(round(mv * fs / av))
    def bake(f):
        if not f.get("raw_adc"):
            return
        d = f.get("default")
        f["default"] = [to_counts(v) for v in d] if isinstance(d, list) else (to_counts(d) if d is not None else d)
        if isinstance(f.get("max"), (int, float)):
            f["max"] = fs                                  # mV ceiling (e.g. 5000) -> ADC full scale
        f["units"] = "ADC"                                 # the STORED unit; studio shows counts/mV/V
    for mod in modules.values():
        for f in mod.get("config", []):
            bake(f)
        for arr in mod.get("config_arrays", []):
            for f in arr.get("element", []):
                bake(f)


def _board_pins_by_cap(board: str) -> dict:
    """For the active board, {capability: [pin signal names in declaration/pool order]}. Lets a
    config field source its dropdown options from the board (e.g. pin_options: TRIGGER_INPUT →
    the capture pool VR1,VR2,DIG1..), so the option index matches the firmware pool index and
    nothing is hand-listed. Returns {} if the board yaml isn't found."""
    import yaml
    boards = Path(__file__).resolve().parent.parent / "definition" / "boards"
    p = boards / f"{board}.board.yaml"
    if not p.is_file():
        # version.h's board PROFILE (the TS signature name) can differ from the board YAML
        # filename — fall back to the available board so pin dropdowns still resolve.
        avail = sorted(boards.glob("*.board.yaml"))
        if not avail:
            return {}
        p = avail[0]
    data = yaml.safe_load(p.read_text()) or {}
    caps: dict = {}
    for pin in data.get("pins", []) or []:
        sig = pin.get("signal", "?")
        # Honour `assignable: false` (e.g. AV12 battery sense, the knock inputs): the pin is
        # consumed by the HAL and must NOT be user-selectable. Keep its pool POSITION (the
        # firmware indexes inputs by it) but label it INVALID, which the picker builder omits from
        # the dropdown — so the option index stays aligned while the entry is hidden.
        #
        # THE NAME IS STILL NEEDED, THOUGH — see _board_fixed_pins() below. Hiding the entry from a
        # dropdown is right; leaving nothing able to NAME the pin is not, and it made the battery's
        # wiring row read "(none)" for a pin it can never leave.
        label = sig if pin.get("assignable", True) else "INVALID"
        for c in pin.get("caps", []) or []:
            caps.setdefault(c, []).append(label)
    return caps


def _board_fixed_pins(board: str) -> dict:
    """{capability: {pool index: signal name}} for the pins the board marks `assignable: false`.

    _board_pins_by_cap() labels those INVALID so every dropdown built from it omits them, which is
    what `assignable: false` means. But a pin can be unselectable and still be WHERE SOMETHING IS: the
    battery sense is permanently on AV12, and with the name blanked its wiring row said "(none)" — for
    a pin that cannot be anything else, on a sensor whose interface is locked as well.

    So the names come back separately, keyed by the pool index the label was blanked at, and the picker
    emits them as options marked `fixed`: displayed, never offered.
    """
    import yaml
    boards = Path(__file__).resolve().parent.parent / "definition" / "boards"
    p = boards / f"{board}.board.yaml"
    if not p.is_file():
        avail = sorted(boards.glob("*.board.yaml"))
        if not avail:
            return {}
        p = avail[0]
    data = yaml.safe_load(p.read_text()) or {}
    out: dict = {}
    idx: dict = {}
    for pin in data.get("pins", []) or []:
        for c in pin.get("caps", []) or []:
            j = idx.get(c, 0)
            idx[c] = j + 1
            if not pin.get("assignable", True):
                out.setdefault(c, {})[j] = pin.get("signal", "?")
    return out


def _board_connectors(board: str) -> dict:
    """{signal name: {"pin": "<connector>-<terminal>", "color": "<wire code>"}} from the board yaml's
    `connectors:` map — the outside-world terminal each board resource lands on and its wire colour.
    The studio's wiring widget resolves a bound resource (e.g. "AV2") to its external pin + colour
    through this. A differential / multi-line signal (VR±, CAN H/L, HBRIDGE±) keeps its FIRST terminal.
    Returns {} if the board yaml or its connectors are absent (colour is a placeholder until loomed)."""
    import yaml
    boards = Path(__file__).resolve().parent.parent / "definition" / "boards"
    p = boards / f"{board}.board.yaml"
    if not p.is_file():
        avail = sorted(boards.glob("*.board.yaml"))
        if not avail:
            return {}
        p = avail[0]
    data = yaml.safe_load(p.read_text()) or {}
    out: dict = {}
    for con in data.get("connectors", []) or []:
        cid = con.get("id", "")
        for t in con.get("terminals", []) or []:
            sig = t.get("signal")
            if not sig:
                continue                                   # ground / power / sensor_supply carry a role, not a signal
            out.setdefault(sig, {"pin": f"{cid}-{t.get('pin')}", "color": t.get("color", "")})
    return out


def _board_connector_shells(board: str) -> dict:
    """{connector id: {"color", "part", "desc"}} from the board yaml's `connectors:` — the shell colour
    and part of each physical connector (CN3 = blue 776231-1). The studio draws the connector diagram
    (and colour-codes a terminal's shell) from this. Returns {} if the board yaml / connectors absent."""
    import yaml
    boards = Path(__file__).resolve().parent.parent / "definition" / "boards"
    p = boards / f"{board}.board.yaml"
    if not p.is_file():
        avail = sorted(boards.glob("*.board.yaml"))
        if not avail:
            return {}
        p = avail[0]
    data = yaml.safe_load(p.read_text()) or {}
    out: dict = {}
    for con in data.get("connectors", []) or []:
        cid = con.get("id")
        if not cid:
            continue
        # `mate` is the HARNESS-side housing. A connector has two part numbers and only one of them
        # can be ordered by whoever is building the loom; carrying just the board's was carrying the
        # one nobody needs to buy.
        out[cid] = {"color": con.get("color", ""), "part": con.get("part", ""),
                    "mate": con.get("mate", ""), "desc": con.get("desc", "")}
    return out


# -----------------------------------------------------------------------
# Display settings (schema `display_settings`) -> TS [SettingGroups] that #if-convert
# a channel/gauge's display units. Firmware stays metric; this is display-only.
# -----------------------------------------------------------------------

# -----------------------------------------------------------------------
# JSON descriptor
# -----------------------------------------------------------------------

# Argument shape (CliRegistry Args enum) -> per-arg primitive types.
_CLI_ARG_SHAPES = {"NONE": [], "I": ["int"], "II": ["int", "int"],
                   "F": ["float"], "S": ["string"], "SS": ["string", "string"]}

def extract_cli_commands() -> list:
    """Auto-derive the command catalog from the firmware CLI registry (firmware/Cli/CliCommands.cpp
    kCmds[]) — the SINGLE source of truth. Each command's arg count/types come from its `Args` shape;
    arg labels + ranges + units are parsed from the help string's `<...>` hints the author already
    writes (e.g. `throttle <etb 0..1> <demand 0..100%>`). Adding/removing a command there flows through
    to the studio (draggable dictionary entry + typed-arg button) with NO hand-authoring here or in the
    schema. A command that can't be parsed is simply omitted — never a build break."""
    src = REPO_ROOT / "firmware" / "Cli" / "CliCommands.cpp"
    if not src.exists():
        return []
    text = src.read_text()
    m = re.search(r"const\s+Command\s+kCmds\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        return []
    cmds = []
    for em in re.finditer(r'\{\s*"([^"]+)"\s*,\s*Args::(\w+)\s*,\s*\w+\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}', m.group(1)):
        name, shape, help_ = em.group(1), em.group(2), em.group(3)
        # Optional "@refresh <region> <region>..." tail: the config regions this command rewrites
        # firmware-side (gen-watch scoped refresh; arg-templated with $0/$1). The studio re-reads just
        # these byte ranges when config_gen next moves. Region = a binding path ("module.array" whole
        # array, "module.array[$0]" one element, "module.array[$0].field" a field). Stripped from help.
        refresh = []
        if "@refresh" in help_:
            help_, _, rtail = help_.partition("@refresh")
            help_ = help_.strip()
            refresh = rtail.split()
        types = _CLI_ARG_SHAPES.get(shape, [])
        hints = re.findall(r"<([^>]+)>", help_)          # e.g. ["etb 0..1", "demand 0..100%"]
        args = []
        for idx, t in enumerate(types):
            hint = hints[idx] if idx < len(hints) else ""
            arg = {"type": t, "label": (re.split(r"[\s0-9]", hint, maxsplit=1)[0] or f"arg{idx + 1}").strip(" .") or f"arg{idx + 1}"}
            rng = re.search(r"(-?[\d.]+)\s*\.\.\s*(-?[\d.]+)\s*(%?)", hint)
            if rng:
                num = lambda s: float(s) if "." in s else int(s)
                arg["min"], arg["max"] = num(rng.group(1)), num(rng.group(2))
                if rng.group(3):
                    arg["units"] = "%"
            args.append(arg)
        cmd = {"name": name, "help": help_, "args": args}
        if refresh:
            cmd["refresh"] = refresh
        cmds.append(cmd)
    return cmds


def _read_min_studio() -> str:
    """firmware/min_studio.txt — the oldest jayecu Studio that can use this firmware's meta and
    dashboard. Raise it in the same change that makes the meta or dashboard use something a released
    studio does not understand: a studio older than this refuses the meta with a message, instead of
    reading a format it does not know."""
    p = Path(__file__).resolve().parent.parent / "firmware" / "min_studio.txt"
    return p.read_text().strip() if p.exists() else "0.1.0"


def gen_tuneit_meta(schema: dict, active_board=None, *, product: str = "jayecu",
                    board_profile: str = "", fw_version: str = "0.0",
                    fw_build: str = "", layout_hash: int = 0, min_studio: str = "0.1.0") -> str:
    """shared/tuneit-meta.json — the fully-resolved, app-facing Data Dictionary the tuner binds to.

    Reuses collect_offsets() (the SAME byte offsets/sizes emitted into the C++ structs), so the
    JSON can never drift from the firmware layout. Config (read/write constants: scalars, tables,
    struct-arrays) is kept separate from Telemetry (read-only live stream), each keyed hierarchically
    by module so the app can build its nav tree. The `meta` block is the device-identity contract:
    `layout_hash` is the exact meta-match key (== the firmware's JAYECU_LAYOUT_HASH); product/board/
    fw_version form the coarse family tag a GUI binds to; fw_build is display-only."""
    prim    = schema["primitive_types"]
    modules = schema["modules"]
    telem_fields, config_scalars, config_tables, config_arrays, telem_size, cfg_size = \
        collect_offsets(prim, modules, telemetry_fields(schema, active_board))

    def datatype(t: str) -> str:
        """TS-style base type tag (U08/S16/F32/…). enum/bool live in one byte; string is ASCII."""
        if t in ("enum", "bool"):
            return "U08"
        if t == "string":
            return "ASCII"
        if t == "expression":
            return "EXPR"
        return ini_type(prim, t)

    # ---- config: { module_snake: { field: {…} } } ------------------------
    config: dict = {}
    def cmod(mod_name):
        return config.setdefault(module_snake(mod_name), {})

    for (name, typ, offset, f, mod_name) in config_scalars:
        if is_pad(f):
            continue
        scale = f.get("scale", 1.0)
        ftype = f.get("type")
        cmod(mod_name)[f["name"]] = {
            "type": "scalar",
            "datatype": datatype(typ),
            "offset": offset,
            "size": field_size(prim, f),
            "label": f.get("label", f["name"]),
            "units": f.get("units", ""),
            "help": f.get("help", ""),                  # hover tooltip (TS-style field description)
            "scale": scale,
            "translate": f.get("translate", 0.0),
            "min": f.get("min", 0),
            "max": f.get("max", 0),
            "digits": scalar_digits(scale, datatype(typ)),
            "default": f.get("default", 0),
            # Control hint for the app: which input fits this field (signal picker / enum / bool / box),
            # and for an enum WHAT IS IN THE LIST. field_enum_meta is the one place that answers both —
            # the same call an array element's field makes. This was hand-rolled here instead, and the
            # hand-rolled copy only knew an INLINE `options:` list: a scalar that named a shared set
            # (`enum: bool_active`) reached the studio with the set's NAME and no options in it, which
            # draws as a dropdown with nothing to pick and no value shown.
            **field_enum_meta(schema, f),
            # An expression field is a BYTECODE blob, not a number: the studio must
            # bind it to the expression editor, never to a spin box. `length` is the
            # program block size so the compiler knows what it has to fit into.
            **({"kind": "expression"} if ftype == "expression" else {}),
            **({"length": field_size(prim, f)} if ftype in ("expression", "string") else {}),
            # A board-pin PICKER on a module scalar. Only array ELEMENTS could carry one, which is
            # where every pin field happened to live — until the tacho, whose pin is a plain scalar
            # and was therefore a number you had to know the pool order to type.
            **field_pickers_meta(f, active_board),
            **field_bits_meta(f),                       # named bit groups, each addressable on its own
        }

    def _axis_ref(ref, cur_mod, channel, n_max, optional, cfg=None):
        """Resolve an x/y/z_axis ref to a self-describing axis: which breakpoint array it is,
        the live channel it reads, its live bin-count scalar (<axis>_n, present only if the axis
        is resizable — null means a fixed grid), the max bins, the runtime channel selector + enable
        scalars (src/en, so the app's Axis Setup can re-point/resize/toggle it), and whether it can be
        disabled. A widget binds straight to this; it never reconstructs the grid from byte offsets."""
        if not ref:
            return None
        try:
            _owner, raw, at = _resolve_axis_table(ref, cur_mod, modules)
        except Exception:
            raw, at = ref, {}
        a = {"array": raw, "channel": channel, "n_max": n_max}
        a["n"] = (raw + "_n") if at.get("_resizable") else None   # live bin-count scalar, or fixed
        if cfg:
            a["src"] = cfg.get("src")                             # uint8 channel selector (SignalId)
            if cfg.get("en"):
                a["en"] = cfg["en"]                              # uint8 enable toggle (optional axis)
        if optional:
            a["optional"] = True
        return a

    # ONE table entry builder. It used to be this loop plus a private copy further down that built a
    # learned block's entry by hand — and that copy wrote no axes, which is how 53 tables reached the
    # studio with no axis to resolve. A table is a table; the only thing its storage decides is the
    # ADDRESS, so that is the only thing passed in.
    def emit_table(t, mod_name, typ, offset):
        scale  = t.get("scale", 1.0)
        is_map = "rows" in t                               # 2D/3D map vs 1D curve / axis
        acfg   = t.get("_axis_cfg", {})
        axes   = [a for a in (
            _axis_ref(t.get("x_axis"), mod_name, t.get("x_channel"), t.get("_max_cols"),  False, acfg.get("x")),
            _axis_ref(t.get("y_axis"), mod_name, t.get("y_channel"), t.get("_max_rows"),  False, acfg.get("y")),
            _axis_ref(t.get("z_axis"), mod_name, t.get("z_channel"), t.get("_max_depth"), t.get("z_optional"), acfg.get("z")),
        ) if a]
        entry = {
            "type": "table",
            "datatype": datatype(typ),                     # cell type (U08/I08/U16/I16…)
            "offset": offset,
            "size": table_size(prim, t),                   # full reserved bytes (cols_max·rows_max·depth_max·cell)
            "label": t.get("label", t["name"]),
            "units": t.get("units", ""),
            "help": t.get("help", ""),                     # hover tooltip
            "scale": scale,
            # Display precision. The schema states it when it knows better than the storage does (a
            # count kept in a float); otherwise it is derived from scale (SSOT) — with the F32 floor
            # only when this entry is a real grid of CELLS. An entry with no axes and no rows is a
            # breakpoint array, and the studio prints a breakpoint at its own precision with digits as
            # a FLOOR (TableImage.h fmtBreak), so a floor of 3 there would turn every rpm bin into
            # "6500.000". The same split the studio makes reading this back (tables_ vs arrays1d_).
            "digits": int(t["digits"]) if t.get("digits") is not None
                      else scalar_digits(scale, datatype(typ) if (is_map or axes) else ""),
            "translate": t.get("translate", 0.0),
            "min": t.get("min", 0),
            "max": t.get("max", 0),
            "resizable": bool(t.get("_resizable_table")),
            "cols": t.get("cols", t.get("size")),          # live grid
            "cols_max": t.get("_max_cols", t.get("cols", t.get("size"))),
            "axes": axes,                                  # x[, y[, z]] — each self-describing
        }
        # AXIS HEADINGS THAT ARE NAMES, NOT NUMBERS. Some axes have no quantity to print: the VVT
        # trim's rows are the four cams and the bank trim's columns are banks 1 and 2. The schema has
        # been able to say so (`row_labels`) since the learned region was added — and this emitter
        # dropped it on the floor, so every one of those axes showed its storage index and the
        # declaration did nothing for months. `col_labels` is its counterpart.
        for key in ("row_labels", "col_labels"):
            if t.get(key):
                entry[key] = list(t[key])
        if is_map:                                         # 2D/3D maps carry rows (+ depth for 3D)
            entry["rows"]     = t["rows"]
            entry["rows_max"] = t.get("_max_rows", t["rows"])
            depth_max = t.get("_max_depth")
            if depth_max:
                entry["depth"]     = t.get("depth", 1)
                entry["depth_max"] = depth_max
        # WHERE THIS TABLE'S VALUES CAN BE ROLLED INTO. A learned surface is a correction ON another
        # table, and the schema says which ("apply_to"). Without carrying it here the studio had the
        # relationship described in three comments and expressed in none of its data, so the operation
        # the schema specifies — apply, then reset — could not be offered generically.
        if t.get("apply_to"):
            entry["apply_to"] = list(t["apply_to"])
            entry["apply_mode"] = t.get("apply_mode", "multiply")
        if t.get("_is_axis") or t.get("_synth_axis"):
            entry["axis"] = True                           # a table's breakpoint array, not a bindable map
            if t.get("_axis_owned_by"):
                entry["axis_of"] = t["_axis_owned_by"]
        cmod(mod_name)[t["name"]] = entry
        return entry

    for (name, typ, offset, t, mod_name) in config_tables:
        if t.get("_align_pad"):                            # float-alignment filler — real bytes (kept in
            continue                                       # the struct + layout_hash) but NOT user data,
                                                           # so don't surface it as a bindable 1D array.
        emit_table(t, mod_name, typ, offset)

    # ---- config_arrays: an array-of-structs (base + stride + per-element field layout), so the
    #      Java app computes any cell as base_offset + index*stride + field.rel_offset ----------
    arr_defs = {(module_snake(mn), arr["name"]): arr
                for mn, md in modules.items() for arr in (md.get("config_arrays") or [])}

    def grouped_names(arr: dict) -> set:
        """Flat field names absorbed by an element table/array — they surface via the table/array
        descriptor, not as individual scalar `fields`."""
        s = set()
        for td in (arr.get("_element_tables") or []):
            if td.get("std"):                              # standard form: cells + per-axis arrays + _n/src/en
                nm = td["name"]; alloc = 1
                for ax, cfg in td["axis_cfg"].items():
                    alloc *= cfg["max"]
                    for k in range(cfg["max"]):
                        s.add(f"{nm}_{ax}_axis_{k}")
                    s.add(cfg["n"])
                    if "src" in cfg: s.add(cfg["src"])
                    if "en" in cfg:  s.add(cfg["en"])
                for k in range(alloc):
                    s.add(f"{nm}_{k}")
                continue
            s.add(td["n_field"])
            for k in range(td["points"]):
                s.add(f"{td['axis']['field']}_{k}")
                s.add(f"{td['value']['field']}_{k}")
        for ad in (arr.get("_element_arrays") or []):
            for j in range(ad["count"]):
                for sf in ad["subfields"]:
                    s.add(f"{ad['name']}_{j}_{sf['name']}")
        return s

    arr_acc: dict = {}
    arr_rel: dict = {}                                     # key -> {field name: rel_offset} (i==0)
    arr_skip: dict = {}
    for (cname, typ, offset, f, mod_name, arr_name, i) in config_arrays:
        key = (module_snake(mod_name), arr_name)
        a = arr_acc.get(key)
        if a is None:
            a = arr_acc[key] = {"type": "struct_array", "base_offset": offset,
                                "count": 0, "stride": 0, "fields": {}}
            arr_rel[key] = {}
            arr_skip[key] = grouped_names(arr_defs.get(key) or {})
        a["count"] = max(a["count"], i + 1)
        if i == 0:                                        # element layout comes from index 0
            rel = offset - a["base_offset"]
            a["stride"] = rel + field_size(prim, f)       # span over every field incl. padding
            if not is_pad(f):
                arr_rel[key][f["name"]] = rel
                if f["name"] not in arr_skip[key]:        # grouped cells surface via tables/arrays
                    a["fields"][f["name"]] = {
                        "datatype": datatype(typ),
                        "rel_offset": rel,
                        "size": field_size(prim, f),
                        "label": f.get("label", f["name"]),
                        "units": f.get("units", ""),
                        "help": f.get("help", ""),         # hover tooltip
                        "scale": f.get("scale", 1.0),
                        "digits": scalar_digits(f.get("scale", 1.0), datatype(typ)),
                        "min": f.get("min", 0),
                        "max": f.get("max", 0),
                        # WHEN IT TAKES EFFECT, where that is not at once and the module's shadow cannot say
                        # it for one field (an output's coil/injector assignment waits for an engine stop in
                        # a module whose generic outputs are live). The studio greys it while turning.
                        **({"applies": f["applies"]} if f.get("applies") in ("engine_stop", "reboot") else {}),
                        # The field's own default, so a client can RESTORE it rather than hardcode a
                        # sentinel. A picker field gated by a sibling (sensor source, gated by interface)
                        # goes stale when that sibling changes -- its stored number is a pool index, and
                        # the pool is chosen by the gate -- and the studio resets it to this.
                        **({"default": f["default"]} if "default" in f else {}),
                        # Optional dictionary-tree sub-category: fields sharing a `subcat` nest under a
                        # node of that name inside the element (e.g. the sensor's Diagnostics fields).
                        **({"subcat": f["subcat"]} if f.get("subcat") else {}),
                        # A FIELD IN ITS SENSOR'S UNITS (the Reading Low/High thresholds): stored at the
                        # scale of the TYPE in that slot, which only the element knows. Without the flag
                        # the studio showed and wrote the raw int — 115 typed for a coolant limit stored
                        # 11.5 °C, because the firmware multiplies by the type's 0.1.
                        **({"type_scaled": True} if f.get("type_scaled") else {}),
                        **field_enum_meta(schema, f),     # kind/options/enum so an element enum picks a dropdown
                        **field_pickers_meta(f, active_board),   # interface-gated board-pin picker (sensor source)
                        **field_bits_meta(f),             # named bit groups (diag_enable, diag_severity, flags)
                        # An expression element (a sensor's precondition) must reach the
                        # expression EDITOR when dragged out, not a spin box over byte 0.
                        **({"kind": "expression", "length": field_size(prim, f)}
                           if f.get("type") == "expression" else {}),
                    }
    for (mod_snake, arr_name), a in arr_acc.items():
        key = (mod_snake, arr_name)
        arr = arr_defs.get(key)
        rel_by = arr_rel[key]
        # Per-element names: when an array is sized from a catalog (count_from: <key>), each element IS
        # the i-th catalog entry — surface its name so the app shows "MAP Sensor", "CLT", … not "sensor 0".
        cf = arr.get("count_from") if arr else None
        cat = schema.get(cf) if cf else None
        if isinstance(cat, list):
            a["element_labels"] = [str(cat[i].get("name") or cat[i].get("id") or i)
                                   for i in range(min(a["count"], len(cat)))]
            # Stable per-element id (the catalog id, e.g. "clt") so a binding path can read
            # sensors.sensor[clt].enabled instead of the opaque numeric index.
            a["element_ids"] = [str(cat[i].get("id") or i)
                                for i in range(min(a["count"], len(cat)))]
            # Per-element PRIMARY telemetry channel: provides[0], defaulting to the id. The id is the
            # config key ([*]); the signal is what the pipeline publishes ($*). They differ for the few
            # sensors that name a canonical channel (boost_pressure -> boost_kpa) or emit several
            # (flex_fuel -> ethanol). Surfacing it lets a templated gauge's "$*" resolve to the right
            # channel for EVERY sensor, not just the ones whose id happens to equal their signal.
            a["element_signals"] = [str((cat[i].get("provides") or [cat[i].get("id") or i])[0])
                                    for i in range(min(a["count"], len(cat)))]
            # Which TYPE each element is, when the catalog settles one at build time. null = the
            # element has no type of its own (a `generic` input) and takes one from the tune's own
            # `type` byte — the only rows where that byte is read, and the only rows whose channel
            # descriptor is a union wire rather than a description of a real sensor. Without this the
            # app cannot tell the two apart: every element carries the byte, because they share one
            # struct, and nothing else says where it means anything.
            if any(isinstance(c, dict) and "type" in c for c in cat[:a["count"]]):
                a["element_types"] = [
                    (None if cat[i].get("type") in (None, "", "generic") else str(cat[i]["type"]))
                    for i in range(min(a["count"], len(cat)))
                ]
            # WHICH INTERFACES each element may be read through — the catalogue's own `interfaces:`
            # list, collapsed to the single `locked:` one where the catalogue settles it. The firmware
            # has carried this as `interface_mask` since the catalogue existed and the meta never
            # published it, so every client offered the WHOLE interface enum for every sensor: a
            # coolant-flow switch could be set to SENT, an oil-pressure sender to Pulse Width, and the
            # pipeline built for it reads a pin that is not wired to anything of the kind. The
            # declaration is the sensor's capability, and until it is exported it constrains nothing.
            # Ids, not indices, exactly as element_types — the reader joins them through the enum's
            # own option ids rather than depending on two orders never drifting apart.
            if any(isinstance(c, dict) and c.get("interfaces") for c in cat[:a["count"]]):
                a["element_interfaces"] = [sensor_offered_interfaces(schema, cat[i])
                                           for i in range(min(a["count"], len(cat)))]
        # ...or the array STATES its element names outright. An array whose elements are fixed roles
        # rather than instances of a catalog has no catalog to draw from — trigger.streams[] is one
        # slot per role, and CAN buses are CAN0/CAN1 — and every consumer was falling back to the
        # index, so the dictionary listed six trigger streams as "0..5" when each one has a name that
        # says exactly what it is. The declaration already existed in the schema and was simply never
        # read on the way out.
        elif isinstance(arr, dict) and isinstance(arr.get("element_labels"), list):
            lbl = [str(x) for x in arr["element_labels"]][:a["count"]]
            # Short of `count`, the rest keep their index — a partial naming is better than none, and
            # silently padding would invent names for slots the schema deliberately left unnamed.
            a["element_labels"] = lbl + [str(i) for i in range(len(lbl), a["count"])]
        # WHICH OPTIONS each element may take, per enum field, as option INDICES — for an array whose
        # elements are different hardware (outputs.output[i] is pin i: an IGN pin cannot be an injector).
        # The studio greys the rest; the firmware refuses them anyway (OutputMap.h).
        if isinstance(arr, dict) and isinstance(arr.get("_element_options"), dict):
            a["element_options"] = arr["_element_options"]
        # Element tables — a 1-D lookup (resizable axis + value cell + live count), e.g. the sensor
        # calibration. ONE descriptor per declaration; the app addresses cell k at
        # base + idx*stride + {axis|value}.rel_offset + k*size; `display:curve` picks the curve widget.
        for td in (arr.get("_element_tables") if arr else []) or []:
            if td.get("std"):                              # STANDARD multi-axis element table (per-instance)
                nm = td["name"]
                desc = {"std": True, "display": td["display"], "label": td["label"],
                        "cell": {"rel_offset": rel_by[f"{nm}_0"], "datatype": datatype(td["cell_type"]),
                                 "scale": td["scale"],
                                 "digits": scalar_digits(td["scale"], datatype(td["cell_type"]))},
                        "axes": {}}
                for ax, cfg in td["axis_cfg"].items():
                    spec = td["axis_specs"][ax]
                    _axsc = spec.get("scale", 1.0)                 # axis breakpoint scale (studio reads it as breakScale)
                    axd = {"rel_offset": rel_by[f"{nm}_{ax}_axis_0"], "n_rel": rel_by[cfg["n"]],
                           "max": cfg["max"], "min": cfg.get("min", 2), "datatype": datatype(cfg["type"]),
                           # No F32 floor: breakpoints, not cells — see emit_table above.
                           "scale": _axsc, "digits": scalar_digits(_axsc),
                           "units": spec.get("units", ""), "label": spec.get("label", ax.upper())}
                    if spec.get("value_min") is not None: axd["value_min"] = spec["value_min"]   # breakpoint
                    if spec.get("value_max") is not None: axd["value_max"] = spec["value_max"]   # value bounds
                    if "src" in cfg: axd["src_rel"] = rel_by[cfg["src"]]
                    if "en"  in cfg: axd["en_rel"]  = rel_by[cfg["en"]]
                    desc["axes"][ax] = axd
                a.setdefault("tables", {})[nm] = desc
                continue
            asz = field_size(prim, {"type": td["axis"]["type"]})
            vsz = field_size(prim, {"type": td["value"]["type"]})
            a.setdefault("tables", {})[td["name"]] = {
                "display": td["display"], "points": td["points"], "label": td["label"],
                "n": {"rel_offset": rel_by[td["n_field"]], "datatype": "U08"},
                "axis": {"rel_offset": rel_by[f"{td['axis']['field']}_0"], "size": asz,
                         "datatype": datatype(td["axis"]["type"]), "scale": td["axis"]["scale"],
                         "units": td["axis"]["units"], "label": td["axis"]["label"],
                         # Per-element raw units, where the elements do not share one: a sensor cal's
                         # axis is ADC counts for an analog input and HERTZ for a frequency one.
                         **({"element_units": td["axis"]["element_units"]}
                            if td["axis"].get("element_units") else {})},
                "value": {"rel_offset": rel_by[f"{td['value']['field']}_0"], "size": vsz,
                          "datatype": datatype(td["value"]["type"]), "scale": td["value"]["scale"],
                          "digits": scalar_digits(td["value"]["scale"], datatype(td["value"]["type"])),
                          "type_scaled": td["value"]["type_scaled"], "label": td["value"]["label"],
                          "min": td["value"]["min"], "max": td["value"]["max"],
                          **({"element_scales": td["value"]["element_scales"]}
                             if td["value"].get("element_scales") else {}),
                          **({"element_units": td["value"]["element_units"]}
                             if td["value"].get("element_units") else {})},
            }
        # Element arrays — a fixed repeated sub-struct (precond/cand). cell = base + idx*stride +
        # rel_offset + j*sub_stride + field.rel_offset.
        for ad in (arr.get("_element_arrays") if arr else []) or []:
            base_rel   = rel_by[f"{ad['name']}_0_{ad['subfields'][0]['name']}"]
            sub_stride = sum(field_size(prim, {"type": sf["type"]}) for sf in ad["subfields"])
            sub_fields = {}
            for sf in ad["subfields"]:
                sub_fields[sf["name"]] = {
                    "rel_offset": rel_by[f"{ad['name']}_0_{sf['name']}"] - base_rel,
                    "datatype": datatype(sf["type"]), "size": field_size(prim, {"type": sf["type"]}),
                    "label": sf.get("label", sf["name"]), "units": sf.get("units", ""),
                    "help": sf.get("help", ""),        # hover tooltip, same as any other field
                    "scale": sf.get("scale", 1.0),
                    "digits": scalar_digits(sf.get("scale", 1.0), datatype(sf["type"])),
                    "min": sf.get("min", 0), "max": sf.get("max", 0),
                    # THE SAME CONTROL HINTS EVERY OTHER FIELD GETS. This emitter wrote raw options and
                    # nothing else, so a nested array's fields reached the studio with no `kind`: an
                    # output slot's cand[].sig — the field whose whole job is to name a bus channel —
                    # arrived as a bare uint16 and drew an empty dropdown with nothing in it. Its role,
                    # three fixed options, drew the generic picker for the same reason.
                    **field_enum_meta(schema, sf),
                    **field_bits_meta(sf),
                    **({"options_from": sf["options_from"]} if sf.get("options_from") else {}),
                }
            a.setdefault("arrays", {})[ad["name"]] = {
                "count": ad["count"], "rel_offset": base_rel, "stride": sub_stride, "fields": sub_fields}
        config.setdefault(mod_snake, {})[arr_name] = a

    # ---- learned segments: a learned table is emitted by the ordinary table loop above (it is an
    # ordinary table — see learned_blocks_as_tables), so all it needs here is its real address. Its cells
    # live in the RAM-backed learned region at page_base + the append-only offset learned_layout assigns,
    # and it carries `segment: "learned"` so the studio Cache reads/writes it through that block instead
    # of the config image. Meta-only: not in EcuConfig / default_tune / layout_hash.
    #
    # This used to be a PRIVATE emitter that built the whole entry by hand — and wrote no axes at all,
    # which is why 53 tables arrived at the studio unresolvable. Patching an "axes" key into that copy
    # would have kept two table emitters in step by hand; there is one now, and this only supplies the
    # address the shared one cannot know.
    learned_blocks, learned_used, _ = learned_layout(schema)
    learned_base = int((schema.get("learned") or {}).get("page_base", "0xF0000000"), 16)
    learned_by_id = {}
    for mn, mod in modules.items():
        for t in mod.get("tables", []):
            if t.get("_learned"):
                learned_by_id[t["_learned"]] = (t, mn)
    for blk in learned_blocks:
        found = learned_by_id.get(blk["id"])
        if not found:
            sys.exit(f"ERROR: learned block '{blk['id']}' produced no table — "
                     "learned_blocks_as_tables did not run, or its module was renamed")
        t, mn = found
        entry = emit_table(t, mn, t["type"], learned_base + blk["offset"])
        entry["segment"] = "learned"                       # nvram block — see doc["segments"]

    # ---- telemetry: flat by channel id (read-only live stream) -----------
    telemetry = {
        name: {
            "datatype": datatype(typ),
            "offset": offset,
            "size": field_size(prim, f),
            "label": f.get("label", name),
            "units": f.get("units", ""),
            "scale": f.get("scale", 1.0),
            "digits": scalar_digits(f.get("scale", 1.0), datatype(typ)),
            "min": f.get("min", 0),
            "max": f.get("max", 0),
            "module": mod_name,
            # Which SignalBus cell the value occupies in the ECU: "float" for engineering-unit signals,
            # "uint32"/"int32" for masks, counters and state words. It is NOT the wire type above — a
            # channel is commonly a float on the bus and a scaled int16 on the wire. The studio needs it to
            # decode a channel the way the firmware holds it: our own channels come off a float bus, so
            # float-cell values decode in float to carry exactly the precision sent, while an integer cell
            # (dtc_indicators is a bitmask) must not be pushed through a float32 at all.
            **({"bus_type": f["bus_type"]} if f.get("bus_type") else {}),
            **({"enum": f["enum"]} if f.get("enum") else {}),
            # …or its OWN labels, when the states belong to this channel alone and there is no shared
            # `enums:` set to point at. lambda_cl_state and app_state both declare their words inline;
            # this dict dropped them, so the studio had nothing to render and a state gauge showed "3".
            **({"options": f["options"]} if f.get("options") else {}),
            # WORTH RECORDING, as the signal catalog declares it. 173 channels carry `datalog: true`
            # and nothing consumed it: the SD logger writes the whole frame and the studio's recorder
            # wrote every column, so the flag stated an intent no reader could act on. Published here
            # so a recorder can offer "the standard set" as something the DEFINITION chose rather than
            # a list the studio would otherwise have to invent and keep in step by hand.
            **({"datalog": True} if f.get("datalog") else {}),
            # HOW OFTEN IT ACTUALLY CHANGES, in Hz. A sensor channel moves at its type's cadence
            # (Sensors.cpp decimates to it); everything else is published by a module on the control
            # frame and moves at frame rate. Carried so a recorder can say when the log rate has
            # outrun the data — a 1 kHz log of a 5 Hz coolant sensor is two hundred copies of one
            # reading, which is the truth about that channel but only useful if somebody is told.
            **({"update_hz": f["update_hz"]} if f.get("update_hz") else {}),
        }
        for (name, typ, offset, f, mod_name) in telem_fields
    }

    # ---- navigation_tree: Configuration → module → its tables/arrays; + Live Data ------------
    # An axis belongs to its table — you edit it via the table's Axis Setup, not as a standalone node.
    # Collect every table used as an x/y/z axis ANYWHERE (synth inline AND named/shared alike) and keep
    # them out of the seeded nav tree. After expand_inline_axes every axis ref is a string table name.
    axis_paths = set()
    for mn in modules:
        ms = module_snake(mn)
        for t in modules[mn].get("tables", []):
            if t.get("axis"):                                   # a standalone axis table (e.g. a learned
                axis_paths.add(f"config.{ms}.{t['name']}")      # store's breakpoints — no config consumer)
            for ax in ("x", "y", "z"):
                ref = t.get(f"{ax}_axis")
                if isinstance(ref, str):
                    try:
                        owner, raw, _ = _resolve_axis_table(ref, mn, modules)
                        axis_paths.add(f"config.{module_snake(owner)}.{raw}")
                    except Exception:
                        pass
    cfg_children = []
    for mod_name in modules:
        msnake = module_snake(mod_name)
        if msnake not in config:
            continue
        title = (modules[mod_name].get("ui") or {}).get("title") or mod_name
        kids = []
        for fname, meta in config[msnake].items():
            if meta.get("type") not in ("table", "struct_array"):
                continue
            if f"config.{msnake}.{fname}" in axis_paths:
                continue
            node = {"name": meta.get("label", fname), "target_path": f"config.{msnake}.{fname}"}
            # A catalog-sized array IS a list of named things (the 120 sensors, …) — so seed it as that
            # list, not as one opaque node the user has to know expands. element_labels/_ids already
            # exist for the dictionary; the nav tree simply never used them, which is why a freshly
            # seeded project showed "sensor" where it should show CLT, MAP, Wideband O2 1, …
            labels, ids = meta.get("element_labels") or [], meta.get("element_ids") or []
            if labels:
                node["children"] = [
                    {"name": labels[i],
                     "target_path": f"config.{msnake}.{fname}[{ids[i] if i < len(ids) else i}]"}
                    for i in range(len(labels))
                ]
            kids.append(node)
        cfg_children.append({"name": title, "target_path": f"config.{msnake}",
                             **({"children": kids} if kids else {})})
    # Configuration only. A "Live Data" root used to sit here pointing at telemetry, but seedNode drops
    # target_path (the pages are the user's to author) and nothing ever populated its children, so it
    # seeded as an empty node no code read.
    navigation_tree = [
        {"name": "Configuration", "children": cfg_children},
    ]

    # ---- enums: value-sets a widget resolves to labels (enum-typed channels/fields carry the id) --
    enum_ids_used = set(schema.get("enums", {}).keys())
    for sig in schema.get("signals", []):
        if sig.get("enum"):
            enum_ids_used.add(sig["enum"])
    enums = {}
    for eid in sorted(enum_ids_used):
        pairs = enum_pairs(schema, eid)
        if pairs:
            enums[eid] = [{"value": v, "id": i, "label": lbl}
                          for v, (i, lbl) in enumerate(pairs)]

    # WHEN EACH SETTING TAKES EFFECT, for the settings that do not take effect at once. A shadowed module's
    # members are copied into the firmware's working copy only at the next engine stop (Trigger, Engine,
    # …) or the next boot, so an edit made while the engine turns sits in RAM doing nothing — a trigger
    # stream switched off on a running engine kept its RPM. The studio greys those controls out while the
    # engine is turning (engine_stop) and says a reboot one applies at the next restart; live and
    # unshadowed settings carry nothing.
    for _mod_name, _members in _module_shadow_members(prim, modules).items():
        _cm = config.get(module_snake(_mod_name), {})
        for _m in _members:
            if not _m["shadow"] or _m["when"] not in ("engine_stop", "reboot"):
                continue
            _e = _cm.get(_m["def"]["name"])
            if isinstance(_e, dict):
                _e["applies"] = _m["when"]
    doc = {
        "meta": {
            "proto": 1,                                   # immortal handshake/format version
            "product": product,                           # "jayecu"
            "board": board_profile,                       # "jaytek_v1"
            "fw_version": fw_version,                      # "0.1.0"  (+ board → GUI family tag)
            "fw_build": fw_build,                          # "06d07e8-dirty" — display only
            "min_studio": min_studio,                      # oldest studio that can read this (firmware/min_studio.txt)
            "layout_hash": f"{layout_hash:08x}",           # exact meta-match key (== JAYECU_LAYOUT_HASH)
            "config_size": cfg_size,                       # flat 32-bit address space — no pages
            "telemetry_size": telem_size,
        },
        "protocol": PROTOCOL,                              # the wire contract (same as generated/protocol.h)
        # The board's HARDWARE self-description. One meta serves one hardware (codegen bakes the board in),
        # so the board's analog front-end lives here. The firmware is ADC-counts-native; the CLIENT uses
        # this to convert counts <-> mV/V for display/entry. Per pin-TYPE: AV (divider front-end) vs AT.
        #   mV = counts * <av|at>.fullscale_mv / adc.full_scale ;  V = mV / 1000.
        # (Per-channel specials like AV12's battery divider live in that SENSOR's cal, not here.)
        "hardware": (lambda a: {
            "board": board_profile,
            "adc":   {"full_scale": a.get("full_scale"), "vref_mv": a.get("vref_mv")},
            "av":    {"fullscale_mv": a.get("av_mv")},
            "at":    {"fullscale_mv": a.get("at_mv")},
        })(_board_adc(board_profile)),
        # Outside-world wiring: each board resource's connector terminal + wire colour, from the board
        # yaml's `connectors:` map. The studio's wiring widget resolves a bound resource to its external
        # pin + colour through this (signal name -> {pin, color}); colour is a placeholder until loomed.
        # Pin pool index -> the RAW channel HardwareInput publishes for that pin, per interface: the
        # same maps the firmware compiles as HW_POOL_SIG / HW_DIG_*_SIG, and the same join
        # primary_hw_sig() makes. A sensor's `source` is an index into one of these, so this is what
        # turns "AV3" into a channel that has units, a range and a precision already described in
        # telemetry — hw_av3 is ADC, 0..4095. Without it a client editing a calibration has to invent
        # the axis's domain from the board's ADC width and a guess about what the interface means.
        "hw_pool_signals": _hw_pool_signals(board_profile),
        "wiring": _board_connectors(board_profile),
        # The physical connectors themselves: id -> {shell colour, part, desc} (CN3 = blue 776231-1).
        # The studio draws the connector diagram / colour-codes a terminal's shell from this.
        "connectors": _board_connector_shells(board_profile),
        # Cache blocks. Config bindings carry an absolute device offset; the studio Cache holds one buffer
        # per segment and routes offset -> (segment, local). `config` is block 0 (the flashed tune); other
        # segments (e.g. the battery-backed `learned` region at page_base) are read/written by meta path
        # exactly like config, but are NOT part of the tune (excluded from save / flash / layout_hash).
        "segments": [
            {"id": "config", "base": 0, "size": cfg_size, "kind": "tune", "writable": True},
        ] + ([{"id": "learned", "base": learned_base, "size": learned_used, "kind": "nvram", "writable": True}]
             if learned_used else []),
        "navigation_tree": navigation_tree,
        "enums": enums,
        # The sensor TYPE catalog: what each type reads in, how finely, and over what domain — the
        # same four facts SENSOR_TYPE_CATALOG carries in firmware (units, val_scale, min, max) plus
        # whether a `generic` input may be set to it.
        #
        # A CATALOGUED sensor needs none of this from here: its type is settled at build time, so
        # resolve_sensor_signals() already stamped the type's units/decimals/range onto its channel's
        # telemetry descriptor. A GENERIC input cannot be described that way — its type is not known
        # until the tune says, so its channel is wire-sized to the UNION of every type it could be
        # given (widest range, finest scale: -720..3000 @ 0.01 here). Read as precision, that union
        # describes no sensor at all. Anything that wants to know what a generic input actually reads
        # has to resolve its configured type and ask the TYPE — which is what this block is for.
        #
        # A LIST, not a map: position IS the stored `type` value, exactly as it indexes the firmware's
        # SENSOR_TYPE_CATALOG. A map would make the reader join the byte to an id through some other
        # ordered list (the field's enum options) and depend on the two never drifting apart.
        "sensor_types": [
            {
                "id":         t["id"],
                "units":      t.get("units", ""),
                "digits":     int(t.get("decimals", 0)),
                # The engineering scale of a stored int16 (cal_val, diag_op_min/max) — SensorTypeDescriptor
                # .val_scale, derived from the type's decimals exactly as the firmware catalog derives it.
                # Published rather than re-derived downstream: the FIRMWARE decodes a cal at the scale of
                # the type the input is actually set to, so anything editing that cal has to use the same
                # number or the two disagree about what the bytes mean — which they did for a generic
                # input, whose per-element scale is a build-time guess of 0.01 made before it had a type.
                "scale":      10.0 ** -int(t.get("decimals", 0)),
                "min":        float(t["min"]),
                "max":        float(t["max"]),
                # Derived exactly as the firmware catalog derives it — a generic input is read as an
                # analog voltage through a cal curve, so it can take any type built that same way.
                "selectable": t.get("raw") == "volts",
                # Below this the input is a FAULT, not a reading (0 = the type has no such floor). The
                # firmware forces it and refuses a calibration that reaches down into it; the studio has
                # to know the same number to say so before the calibration is written.
            }
            for t in (schema.get("sensor_types") or [])
        ],
        # Every SELECTABLE bus signal name -> its SignalId index. A channel selector scalar (a table
        # axis's `src`, a sensor source, …) stores this index; the app maps a chosen channel name to it.
        # Raw hardware channels (hw_raw: HardwareInput's SIG_HW_*) are EXCLUDED — they are plumbing, not
        # user-selectable sources. (Non-hw signals are all appended before the reserved-id hole, so list
        # index == SignalId here.)
        "signals": {c["id"]: i for i, c in enumerate(schema["signals"]) if not c.get("hw_raw")},
        "config": config,
        "telemetry": telemetry,
        # The Lua scripting API (functions + callbacks, with signatures/docs) for the studio editor's
        # context-sensitive autocomplete. Validated against firmware ECU_API by validate_lua_api().
        "lua_api": schema.get("lua_api", {"functions": [], "callbacks": []}),
        # The SHIPPED trigger wheel library. Reference data describing how to fill trigger.streams[],
        # so it travels with the schema like everything else the studio resolves. It used to be a
        # hardcoded list inside the studio binary, which made the wheels on offer a property of the
        # studio build rather than of the firmware the user is actually connected to.
        "trigger_wheels": schema.get("trigger_wheels", []),
        # WHAT AN OUTPUT SLOT CAN BE, passed through as data: the studio's wizard reads it, and so does
        # the page builder. One definition — a template that drifts from the page offering it is a
        # template that installs something other than what it says.
        "output_templates": schema.get("output_templates", []),
        # WHICH CHANNELS a recording carries, as data — the studio offers the sets this firmware
        # declares rather than a list compiled into the app. See datalog_templates in the schema.
        "datalog_templates": schema.get("datalog_templates", []),
        # HOST VARIABLES the shipped dashboard binds to ("pc.<name>"): studio-side values the ECU never
        # sees. Declared here so they arrive with the meta rather than in a project side-car nothing
        # installs. F32, scale 1 — see pc_vars in the schema. Not part of the layout hash (no ECU bytes).
        "pcVars": [dict({"name": v["name"], "datatype": "F32", "scale": 1.0,
                         "min": float(v.get("min", 0)), "max": float(v.get("max", 0)),
                         "units": v.get("units", ""), "label": v.get("label", v["name"]),
                         "kind": v.get("kind", "")},
                        **({"default": float(v["default"])} if "default" in v else {}),
                        **({"options": list(v["options"])} if v.get("options") else {}))
                   for v in schema.get("pc_vars", [])],
        # EVERY TABLE AN ID CAN NAME, in id order — the same list the firmware's table_registry.h
        # switches over. The studio needs it twice: to compile table(<name>) / interp(<name>, x) to
        # the id the VM expects, and to OFFER the names in the expression builder. It rode in as the
        # option list of a table_ref field before, which meant the language could only see it where a
        # config field happened to mention it.
        "table_registry": [{"id": i, "name": e["key"], "label": e["label"]}
                           for i, e in enumerate(expr_table_registry(modules))],
        # Battery-backed learned tables (LTFT/LTT) the studio views/applies/resets via the config r/w
        # block protocol at learned.page_base + block.offset — a FIXED, codegen-assigned layout (the exact
        # parallel of `config`, same offsets the firmware compiles against in generated/learned_layout.h).
        "learned": (lambda bl: {
            "page_base":   (schema.get("learned") or {}).get("page_base", "0xF0000000"),
            "region_cap":  bl[2],
            "region_used": bl[1],
            "blocks":      bl[0],
        })(learned_layout(schema)),
        # Command catalog auto-derived from the firmware CLI registry (kCmds[]). The studio lists these
        # in the dictionary (draggable) and builds a typed-arg button per command — zero per-command
        # code. New firmware commands appear here on the next build. See extract_cli_commands().
        "commands": extract_cli_commands(),
        # The default tune as the exact packed EcuConfig payload (base64) — same bytes the firmware
        # flashes. Lets the app populate every scalar/table/axis OFFLINE (no ECU); a live read
        # overrides it. cfg_size bytes, layout_hash at offset 0.
        "default_tune": base64.b64encode(
            serialize_default_config(prim, modules, layout_hash)).decode("ascii"),
        # DTC hover text for the studio's DTC dock. `dtc_descriptions` is an exact P-code -> phrase
        # map (sensor catalog faults + module signal-validity codes); `dtc_categories` gives the
        # OBD subsystem ranges + labels so a code with no exact entry still resolves to its category.
        # The studio's VE autotuner: which map it tunes, which channels it measures and which
        # conditions disqualify a reading. Host-side feature, ECU-specific facts — see the schema.
        "autotune": _autotune_meta(schema),
        # …and the tables it learns from a channel that measures the answer (Predicted MAP).
        "value_autotune": _value_autotune_meta(schema),
        "dtc_descriptions": _dtc_descriptions(schema),
        "dtc_categories": [
            {"lo": f"P{lo:04X}", "hi": f"P{hi:04X}", "label": label}
            for _bit, _cid, label, ranges in _dtc_categories(schema) for (lo, hi) in ranges
        ],
    }
    return json.dumps(doc, indent=2)

# -----------------------------------------------------------------------
# Default config C++ initialiser
# -----------------------------------------------------------------------

_PACK_FMT = {(1, False): "<B", (1, True): "<b", (2, False): "<H", (2, True): "<h",
             (4, False): "<I", (4, True): "<i"}


def _scalar_fmt(prim_types, type_name):
    ti = type_info(prim_types, type_name)
    if ti["c_type"] == "float":
        return "<f"                                     # IEEE-754, not the (4,False) uint32 slot
    return _PACK_FMT[(ti["size"], ti["c_type"].startswith("int"))]


def _place_at_stride(t: dict, live: list) -> list:
    """Lay a table's LIVE default cells into its PHYSICAL grid.

    A table is a fixed allocation (32x32x6) whose <axis>_n says how much of it is in play, and the cells
    are addressed at the allocation's stride — so row r starts at r * cols_max, not r * cols. Defaults
    were written packed to the live size, which put every row after the first in the wrong place the
    moment a table was allocated wider than its default (which is most of them: ve_table defaults 22
    wide inside 32). Padding is C++ zero-fill's job for the tail; the gaps BETWEEN live rows have to be
    written out, because rows follow them.
    """
    if not live or "rows" not in t:
        return live                      # 1D: a single row, so stride cannot differ
    cols, rows = t["cols"], t["rows"]
    depth = t.get("depth", 1) or 1
    mc, mr = t.get("_max_cols", cols), t.get("_max_rows", rows)
    if mc == cols and mr == rows:
        return live                      # the live grid IS the allocation: nothing to pad
    out: list = []
    for z in range(depth):
        for r in range(rows):
            base = (z * rows + r) * cols
            row = live[base:base + cols]
            if not row:
                break
            out += list(row) + [0] * (mc - len(row))
        if z < depth - 1:
            out += [0] * ((mr - rows) * mc)   # the rest of this plane, so the next starts at its base
    return out


def _table_default_vals(t: dict) -> list:
    """The live-region default cells for a table, row-major (x fastest). One source of truth
    for both the binary serializer and the C++ initializer (a host test asserts they match):
      - `default_row`: a 1D list broadcast across every row (tile it `rows` times). Used by 2D
        tables that want a curve over X replicated up the Y axis (e.g. clt_corr: the warmup
        curve over CLT, MAP-flat — so enabling the Y axis is a no-op until the tuner edits it).
      - `default_values`: an explicit row-major list of cells. May also be given as a list of
        rows (one inner list per Y bin) — flattened row-major here — so a big surface like
        ign_table can be written in the schema as a readable grid instead of one long line.
      - else a uniform `default` fill over the live element count."""
    if t.get("default_row"):
        return _place_at_stride(t, list(t["default_row"]) * (t["rows"] if "rows" in t else 1))
    dv = t.get("default_values")
    if dv:
        if isinstance(dv[0], list):
            return _place_at_stride(t, [c for row in dv for c in row])
        return _place_at_stride(t, list(dv))
    if "rows" in t:
        return _place_at_stride(t, [t.get("default", 50)] * t.get("_live_elems", t["rows"] * t["cols"]))
    return []


def _pack_field(buf, off, prim_types, f, idx=None):
    """Pack one scalar/enum/bool/string field's default into buf at off — byte layout
    identical to the C++ packed struct (verified against the compiled g_config). `idx` selects
    a per-element default for config-array elements (list defaults)."""
    t = f.get("type")
    if t == "expression":
        # An empty program is all-zero: byte 0 is OP_END, which the VM reads as
        # "no expression" (always armed). The buffer is already zeroed.
        return
    if t == "string":
        s = f.get("default", "")
        s = s if isinstance(s, str) else ""
        b = s.encode("ascii", "ignore")[: int(f["length"])]
        buf[off:off + len(b)] = b                       # remainder stays 0
        return
    if t in ("enum", "bool"):
        struct.pack_into("<B", buf, off, int(_elem_default(f, idx)) & 0xFF)
        return
    # Float scalars must NOT go through int(). This line cast every scalar to an integer, so a
    # `type: float` field's default was truncated on its way into the binary image — the ETB's
    # kp/ki/kd shipped as 6.0/96.0/0.0 instead of 6.435/96.34/0.07. The compiled g_config had the
    # right values, and bank A's default tune had the truncated ones; since the stored tune wins
    # boot arbitration, the truncated pair is what the ECU actually ran. The table packer below
    # already made this distinction; the scalar packer did not.
    v = _elem_default(f, idx)
    is_float = (type_info(prim_types, t)["c_type"] == "float")
    struct.pack_into(_scalar_fmt(prim_types, t), buf, off, float(v) if is_float else int(v))


def serialize_default_config(prim_types, modules, layout_hash: int) -> bytes:
    """Serialize the schema defaults into the exact packed EcuConfig byte image — the same
    bytes the firmware's g_config holds. Walks modules in the SAME order as collect_offsets
    and uses the SAME default values as gen_default_config_cpp, so the result is byte-for-
    byte identical to the compiled struct (a host test asserts this). Field 0 is the
    layout_hash, matching JAYECU_LAYOUT_HASH so the default image passes the boot gate."""
    _, _, _, _, _, cfg_size = collect_offsets(prim_types, modules)
    buf = bytearray(cfg_size)
    struct.pack_into("<I", buf, 0, layout_hash)

    off = 4
    for mod_name, mod in modules.items():
        if mod_name == "System":
            continue
        for f in mod.get("config", []):
            _pack_field(buf, off, prim_types, f)
            off += field_size(prim_types, f)
        for t in config_tables(mod):
            fmt = _scalar_fmt(prim_types, t["type"])
            esz = type_info(prim_types, t["type"])["size"]
            is_float = (type_info(prim_types, t["type"])["c_type"] == "float")
            vals = _table_default_vals(t)
            for i, v in enumerate(vals):
                struct.pack_into(fmt, buf, off + i * esz, float(v) if is_float else int(v))
            off += table_size(prim_types, t)            # advance past the full MAX allocation
        for slot in array_union_slots(mod.get("config_arrays", [])):
            arr = slot[0]                               # a union designates only its first member
            esz = array_element_size(prim_types, arr)
            for i in range(arr["count"]):
                fo = off + i * esz
                for f in arr["element"]:
                    _pack_field(buf, fo, prim_types, f, i)
                    fo += field_size(prim_types, f)
            off += array_slot_size(prim_types, slot)
    return bytes(buf)


def default_tune_image(prim_types, modules, layout_hash: int) -> bytes:
    """A complete, CRC-valid tune-bank record: ConfigHeader(24) + EcuConfig payload. This
    is what the linker drops at bank A so a freshly-flashed board boots the default tune
    (StorageManager finds it valid on first run). CRC32 == firmware Crc32 (IEEE 802.3)."""
    payload = serialize_default_config(prim_types, modules, layout_hash)
    MAGIC, SEQ, SRC_DEFAULT = 0x4A454355, 1, 3
    pre = struct.pack("<III", MAGIC, SEQ, len(payload))            # magic+sequence+data_len
    crc = zlib.crc32(payload, zlib.crc32(pre)) & 0xFFFFFFFF        # CRC over [pre]+payload
    header = pre + struct.pack("<II", crc, 0) + bytes([SRC_DEFAULT, 0, 0, 0])  # 24 bytes
    return header + payload


def gen_default_tune_cpp(prim_types, modules, layout_hash: int) -> str:
    img = default_tune_image(prim_types, modules, layout_hash)
    body = ",\n    ".join(", ".join(f"0x{b:02x}" for b in img[i:i + 16])
                          for i in range(0, len(img), 16))
    return "\n".join([
        BANNER,
        "#include <cstdint>",
        "",
        "// A complete CRC-valid config record (ConfigHeader + EcuConfig payload) linked",
        "// into flash bank A (.flash_cfg_a @ 0x08180000). On a freshly-flashed board the",
        "// StorageManager finds this valid on first boot and loads it as the default tune.",
        f"// {len(img)} bytes = 24 (header) + {len(img) - 24} (payload).",
        'extern "C" const unsigned char DEFAULT_TUNE_IMAGE[]',
        '    __attribute__((section(".flash_cfg_a"), used)) = {',
        f"    {body},",
        "};",
        f'extern "C" const unsigned DEFAULT_TUNE_IMAGE_LEN = {len(img)};',
        "",
    ])


def gen_default_config_cpp(prim_types, modules):
    lines = [
        BANNER,
        '#include "ecu_config.h"',
        "",
        "// The live tune. On the FIRMWARE this is uninitialised storage (.bss): boot loads a",
        "// validated record from flash bank A/B (or SD) into it, and fail-stops on the red ERROR",
        "// LED if none qualifies. There are deliberately no compiled defaults in RAM —",
        "//   * the initialiser cost 111 KB of the flash image and a full-size startup copy that the",
        "//     bank A load then overwrote on every boot;",
        "//   * `make flash` mass-erases and writes bank A with the current layout, verified, so a",
        "//     board that cannot produce a valid tune is a bricked flash, not a case to paper over;",
        "//   * running an engine on fallback defaults nobody chose is worse than not running.",
        "//",
        "// HOST TESTS define JAYECU_COMPILED_DEFAULTS: they have no flash banks, so they need the",
        "// schema defaults compiled in. Costs nothing on a host.",
        "#ifdef JAYECU_COMPILED_DEFAULTS",
        "EcuConfig g_config = {",
        "    .layout_hash = JAYECU_LAYOUT_HASH,",
    ]

    for mod_name, mod in modules.items():
        if mod_name == "System":
            continue
        cfg    = mod.get("config", [])
        tbls   = config_tables(mod)
        arrays = mod.get("config_arrays", [])
        if not cfg and not tbls and not arrays:
            continue
        ms = module_snake(mod_name)
        lines.append(f"    .{ms} = {{")

        for f in cfg:
            if f.get("type") == "expression":
                lines.append(f'        .{f["name"]} = {{}},   // empty program = always armed')
                continue
            if f.get("type") == "string":
                dv = f.get("default", "")
                dv = dv if isinstance(dv, str) else ""   # numeric leftover -> empty string, not "0"
                # json.dumps -> a properly-escaped C string literal (\n \" \\ ...), so multi-line
                # defaults like a Lua script bake into valid C. ASCII content only (matches the
                # binary serializer's .encode('ascii'), which the default_config host test checks).
                lines.append(f'        .{f["name"]} = {json.dumps(dv)},')
            else:
                lines.append(f"        .{f['name']} = {f.get('default', 0)},")

        # Tables are FLAT max-sized arrays. Initialise only the live region
        # (packed row-major, stride = default cols); C++ zero-fills the rest of
        # the reserved MAX allocation.
        for t in tbls:
            cells = _table_default_vals(t)
            if cells:
                vals = ", ".join(str(v) for v in cells)
                lines.append(f"        .{t['name']} = {{{vals}}},")
            else:
                lines.append(f"        .{t['name']} = {{}},")

        for slot in array_union_slots(arrays):
            # A union can designate only ONE member; init the slot's first array. The
            # other overlay members (and any tail up to the max) are zeroed by static
            # init of g_config. (Defaults for all pattern buffers are zero anyway.)
            arr = slot[0]
            # per-element init (so a per-element `default` LIST sets each element explicitly,
            # e.g. streams.capture_index: default: [0,1,2,3]; the pin arbiter de-dups anyway).
            # Element table/array fields init via the emit plan: a real array `{...}` / nested
            # `{{...}}` so the C++ matches the regrouped struct (and the flat binary serializer).
            elems = [_elem_init_str(prim_types, arr, i) for i in range(arr["count"])]
            lines.append(f"        .{arr['name']} = {{{', '.join(elems)}}},")

        lines.append("    },")

    lines += [
        "};",
        "#else",
        "// Firmware: .bss. Boot fills it from a validated flash record or fail-stops.",
        "EcuConfig g_config;",
        "#endif",
        "",
    ]
    return "\n".join(lines)

# --- CAN protocol templates -------------------------------------------------------------------------

def _parse_can_bits(spec) -> tuple:
    """A spec's own bit addressing -> (bit_off, width), in the standard byte*8+bit numbering.

    Kept because it is how a published table gets into a template: the forms below are what the
    documents actually print, so a field can be COPIED rather than translated and the transcription
    is proof-readable against the page it came from. Bit 7 is the most significant of its byte, which
    is the numbering DBC files and every CAN tool use.
        "0-1"      bytes 0..1 inclusive           -> (7, 16)   starts at byte 0's MSB
        "4"        one whole byte                 -> (39, 8)
        "2:5"      byte 2, bit 5                  -> (21, 1)
        "6:3-7:0"  byte 6 bit 3 .. byte 7 bit 0   -> (51, 12)
    """
    def addr(tok: str, at_start: bool) -> tuple:
        tok = tok.strip()
        if ":" in tok:
            b, bit = tok.split(":", 1)
            return int(b), int(bit)
        # A bare byte as a range endpoint means the whole byte: its MSB when it opens the range,
        # its LSB when it closes one.
        return int(tok), (7 if at_start else 0)

    text = str(spec).strip()
    if "-" in text:
        (b0, i0), (b1, i1) = addr(text.split("-", 1)[0], True), addr(text.split("-", 1)[1], False)
        # A big-endian field runs DOWN from its start bit, so its width is the number of steps taken
        # to reach the end address — not the difference between two indices.
        width = (b1 - b0) * 8 + (i0 - i1) + 1
        if width < 1:
            raise SystemExit(f"can_templates: bit range '{spec}' ends before it starts")
        return b0 * 8 + i0, width
    b, i = addr(text, True)
    return b * 8 + i, (1 if ":" in text else 8)


# --- CAN protocol templates -------------------------------------------------------------------------
#
# A published protocol is no longer a flash table. It is a TEMPLATE — definition/can_templates/*.json —
# that the studio loads into the tune's generic CAN pool, so the ECU carries only the one protocol this
# car actually speaks, and a frame that turns out to be wrong is a tune edit rather than a firmware
# release.
#
# Codegen's job is to VALIDATE them and publish them beside the meta. Validation matters more here than
# it did for the flash table: a template naming a signal the catalog no longer has would load as a field
# bound to nothing and transmit zeros for ever, which is exactly the kind of fault that survives a bench
# test. Caught here, it stops the build instead.

def install_can_templates(sig_ids: set, out_dir) -> list:
    """Validate definition/can_templates/*.json against the signal catalog and publish them to
    <out_dir>/can_templates/. Returns (id, frames, fields) per template installed."""
    import json, shutil
    src = REPO_ROOT / "definition" / "can_templates"
    dst = out_dir / "can_templates"
    if not src.is_dir():
        return []
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True, exist_ok=True)

    installed = []
    for path in sorted(src.glob("*.json")):
        t = json.loads(path.read_text())
        frames = t.get("frames", [])
        for m in frames:
            mid, dlc = int(m["id"]), int(m.get("dlc", 8))
            if not (0 <= dlc <= 8):
                raise SystemExit(f"can_templates/{path.name}: frame 0x{mid:X} dlc {dlc} out of range")
            limit = 0x1FFFFFFF if m.get("ext") else 0x7FF
            if not (0 <= mid <= limit):
                raise SystemExit(f"can_templates/{path.name}: id 0x{mid:X} too large for its id type")
            for f in m.get("fields", []):
                # A null channel is not a mistake: it is a field a SENSOR consumes, so the sensor
                # publishes the reading with its own calibration and diagnostics instead of the field
                # writing the channel underneath it.
                if f.get("sig") is not None and f["sig"] not in sig_ids:
                    raise SystemExit(f"can_templates/{path.name}: '{f['sig']}' is not a signal id")
                # A field may be written either way: `bits` in the spec's own notation for a fresh
                # transcription, or a numeric start/width once it has been normalised. Normalising
                # here means the studio only ever reads one form.
                if "bits" in f:
                    off, width = _parse_can_bits(f["bits"])
                    f["bit_off"], f["width"] = off, width
                    del f["bits"]
                else:
                    off, width = int(f["bit_off"]), int(f["width"])
                if not (1 <= width <= 32):
                    raise SystemExit(f"can_templates/{path.name}: '{f['sig']}' width {width} out of range")
                # A big-endian field runs DOWN from its start bit and continues at the top of the
                # next byte, so its extent is NOT off+width — that arithmetic condemns every
                # correctly-placed Motorola field that starts anywhere but a byte's LSB.
                little = bool(int(f.get("flags", 0)) & 2)
                if little:
                    last_byte = (off + width - 1) // 8
                else:
                    last_byte = off // 8 + ((7 - off % 8) + width - 1) // 8
                if last_byte >= dlc:
                    raise SystemExit(f"can_templates/{path.name}: '{f['sig']}' runs past the end of "
                                     f"frame 0x{mid:X}")
        write_if_changed(dst / path.name, json.dumps(t, indent=2) + "\n")
        installed.append((t.get("id", path.stem), len(frames),
                          sum(len(m.get("fields", [])) for m in frames)))
    return installed



def gen_outputs_h(outputs: list) -> str:
    """generated/outputs.h — the static output-binding table (OUTPUTS[]). Each binding: pin +
    kind + roled candidates + encode + failsafe. The OutputManager builds a pipeline per
    binding and resolves the pin at the composition edge. Roled enums/structs live in
    firmware/Pipeline/OutputStages.h (the pure core)."""
    ROLE = {"primary": "pipe::ROLE_PRIMARY", "limit": "pipe::ROLE_LIMIT", "override": "pipe::ROLE_OVERRIDE"}
    POL  = {"first": "pipe::PRIM_FIRST", "avg": "pipe::PRIM_AVG", "min": "pipe::PRIM_MIN", "max": "pipe::PRIM_MAX"}
    KIND = {"pwm": 0, "digital": 1}
    out = ["// AUTO-GENERATED — do not edit. Run codegen/codegen.py.",
           "#pragma once", "#include <cstdint>",
           '#include "signal_ids.h"',
           '#include "../firmware/Pipeline/OutputStages.h"', "",
           "struct OutputDef {",
           "    const char* name;",
           "    bool        enabled;",
           "    uint8_t     pin;            // board output resource (resolved at the edge)",
           "    uint8_t     kind;           // 0=pwm 1=digital",
           "    uint16_t    pwm_freq_hz;",
           "    bool        active_high;    // digital",
           "    float       threshold;      // digital",
           "    const pipe::RoledCandidate* cand;",
           "    uint8_t     n_cand;",
           "    uint8_t     primary_policy;",
           "    float       failsafe;",
           "    float       scale, offset, clamp_lo, clamp_hi;",
           "};", ""]
    for i, o in enumerate(outputs):
        cands = o.get("candidates", []) or []
        out.append(f"static const pipe::RoledCandidate OUT_{i}_CAND[] = {{")
        if cands:
            for cand in cands:
                out.append(f'    {{ SIG_{cand["signal"].upper()}, '
                           f'{ROLE[str(cand.get("role", "primary")).lower()]} }},')
        else:
            out.append("    { SIG_NONE, pipe::ROLE_PRIMARY },")   # placeholder; n_cand=0 ignores it
        out.append("};")
    out.append("static const OutputDef OUTPUTS[] = {")
    for i, o in enumerate(outputs):
        cands = o.get("candidates", []) or []
        pwm = o.get("pwm", {}) or {}
        dig = o.get("digital", {}) or {}
        enc = o.get("encode", {}) or {}
        out.append(
            f'    {{ "{o["name"]}", {"true" if o.get("enabled") else "false"}, {int(o.get("pin", 0))}, '
            f'{KIND.get(str(o.get("kind", "pwm")).lower(), 0)}, {int(pwm.get("freq_hz", 0))}, '
            f'{"true" if dig.get("active_high") else "false"}, {float(dig.get("threshold", 0.5))}f, '
            f'OUT_{i}_CAND, {len(cands)}, {POL.get(str(o.get("primary_policy", "first")).lower(), "pipe::PRIM_FIRST")}, '
            f'{float(o.get("failsafe", 0.0))}f, {float(enc.get("scale", 1.0))}f, {float(enc.get("offset", 0.0))}f, '
            f'{float(enc.get("clamp_lo", 0.0))}f, {float(enc.get("clamp_hi", 100.0))}f }},')
    out.append("};")
    out.append(f"static constexpr uint8_t OUTPUT_COUNT = {len(outputs)};")
    return "\n".join(out) + "\n"


# -----------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------

def main():
    schema     = yaml.safe_load(SCHEMA_FILE.read_text())
    prim_types = schema["primitive_types"]
    modules    = schema["modules"]
    # Replicate per-cylinder sensors (wideband/EGT) to _1.._N (N = the board's MAX_CYLINDERS) BEFORE the
    # producer/consumer validation, so lambda_1..N / egt_1..N are real catalog + signal ids.
    _vs, _gh, _brd = _read_version_h()
    _by = None
    if "--board" in sys.argv:
        _bi = sys.argv.index("--board"); _by = sys.argv[_bi + 1] if _bi + 1 < len(sys.argv) else None
    expand_per_cylinder(schema, _board_max_cylinders(_by or _brd))
    expand_per_slot(schema, _by or _brd)          # out_1..out_N, one per output slot the board has
    if not schema.get("signals"):
        sys.exit("ERROR: ecu.schema.yaml has no `signals:` catalog — the value bus is the "
                 "first-class source of truth; producers (sensors/modules/CAN/Lua) reference it.")
    # Validate producers/consumers reference real catalog signals — a sensor's `provides`
    # (default [id]) and every module `provides`/`requires` must name a catalog entry.
    catalog_ids = {c["id"] for c in schema["signals"]}
    prob = []
    for s in schema.get("sensors", []) or []:
        for ch in (s.get("provides") or [s["id"]]):
            if ch not in catalog_ids:
                prob.append(f"sensor '{s['id']}' produces unknown signal '{ch}'")
    for mn, m in (modules or {}).items():
        for p in (m or {}).get("provides", []) or []:
            pid = p if isinstance(p, str) else p.get("id")
            if pid not in catalog_ids:
                prob.append(f"module '{mn}' provides unknown signal '{pid}'")
        for r in (m or {}).get("requires", []) or []:
            rid = r if isinstance(r, str) else r.get("id")
            if rid not in catalog_ids:
                prob.append(f"module '{mn}' requires unknown signal '{rid}'")
    for o in schema.get("outputs", []) or []:
        for cand in (o.get("candidates", []) or []):
            sig = cand.get("signal")
            if sig not in catalog_ids:
                prob.append(f"output '{o.get('name')}' candidate references unknown signal '{sig}'")
    if prob:
        sys.exit("ERROR: producer/consumer references not in the signals: catalog:\n  "
                 + "\n  ".join(prob))

    # A sensor-backed signal takes its units, precision, range and wire from its sensor's type.
    resolve_sensor_signals(schema)

    # Resolve the board early — the analog-pool layout drives the raw ADC-count telemetry
    # channels (and the board ADC full-scale), so it must be known before synthesize.
    version_str, git_hash, board = _read_version_h()
    board_yaml = None
    if "--board" in sys.argv:
        i = sys.argv.index("--board")
        board_yaml = sys.argv[i + 1] if i + 1 < len(sys.argv) else None
    active_board = board_yaml or board

    # Promote each raw hardware analog input to a first-class bus signal (uint32 ADC counts), APPENDED
    # at the catalog tail so every existing SignalId keeps its value. HardwareInput publishes them each
    # frame; telemetry (and, incrementally, the sensor pipeline) consume them from the bus — which is
    # what collapses the old telem-only raw_* special case. Must run before telemetry_fields/signal ids.
    schema["signals"].extend(_hw_input_signals(active_board))
    # Digital raw inputs (per DIG pin × capture mode) — same treatment; these push the catalog past 255,
    # so gen_signal_ids_h reserves id 255 (SIG_NONE) and skips it. `hw_raw` keeps them out of pickers.
    schema["signals"].extend(_hw_digital_signals(active_board))

    # THE CATALOG IS NOW COMPLETE — pin the ids before ANYTHING resolves a channel name to an index.
    #
    # resolve_channel_name_defaults (and the meta's signal map) turn a name into its position in this
    # list, while the firmware's SignalId comes from the lock. Those two agree only if the list is
    # already in lock order, so the lock has to be applied here rather than at emit time. It was applied
    # late once: four signals declared mid-schema appended to ids 367-370 as intended, but every
    # channel-name default after the insertion point resolved four positions high — which silently
    # re-pointed table axis sources across the whole schema and turned up as a transient-throttle test
    # that stopped enriching.
    schema["signals"] = apply_signal_id_lock(schema["signals"], board)

    # SIGNAL RANGE GUARD — a declared range must be representable by BOTH types it is given.
    #
    # A signal declares two independent types: telem_type (how it is packed on the wire) and bus_type
    # (which SignalBus cell it occupies in the ECU). Nothing checked that either could hold the range the
    # same line declares, and the failures are silent in both directions:
    #
    #   * wire too narrow  -> counts wrap; the studio shows a value that never existed
    #   * bus float, wide integer range -> a float cell is exact only to 2^24, so a counter goes coarse and
    #     a bitmask has bits rounded away. frame_count and per_cycle_count sat like this for a long time:
    #     declared to 4e9, held in a float, and a 1 kHz counter passes 2^24 in about 4.7 hours.
    #
    # Every input is already in the schema (min/max/scale/telem_type/bus_type), so this is arithmetic, not
    # new bookkeeping. It fails the build, because a range nobody can represent is a defect in the
    # declaration, not a caveat to note.
    _FLOAT_EXACT = 16777216.0            # 2^24 — where a float32 stops counting in ones
    _bad_range = []
    for _s in schema.get("signals", []):
        _tt, _bt = _s.get("telem_type"), _s.get("bus_type")
        _sc = float(_s.get("scale", 1.0) or 1.0)
        _lo, _hi = float(_s.get("min", 0.0)), float(_s.get("max", 0.0))
        if _hi <= _lo:                    # no declared range (0..0) — nothing to check
            continue
        if _tt in WIRE_MAX and _sc:      # the wire carries COUNTS of scale units
            _cl, _ch = _lo / _sc, _hi / _sc
            if _ch > WIRE_MAX[_tt] or _cl < WIRE_MIN[_tt]:
                _bad_range.append(f"{_s['id']}: {_lo}..{_hi} / scale {_sc} = {_cl:.0f}..{_ch:.0f} counts, "
                                  f"which {_tt} cannot hold")
        if _bt == "float" and _sc == 1.0 and _hi > _FLOAT_EXACT:
            _bad_range.append(f"{_s['id']}: integer range up to {_hi:.0f} in a FLOAT bus cell "
                              f"(exact only to 2^24) — declare bus_type: uint32 for a counter/mask")
    if _bad_range:
        raise SystemExit("codegen: signal range(s) not representable by their declared type(s):\n  "
                         + "\n  ".join(_bad_range))

    # HARD BOUNDARY GUARD — every SignalId holder must span the whole catalog.
    # SignalId is uint16, config selectors are int16, sensor primary_channel / SensorAuxOutput.signal are
    # uint16, and the +1-encoded `options_from: signals` selectors (precond) are uint16 too. The catalog long
    # ago outgrew 254 (per-cylinder wideband/EGT, the per-scope trims), and while the precond selector was a
    # uint8 that silently made 52 signals unpickable as preconditions — no error, just a NOTE nobody could
    # act on. So this now FAILS the build instead: an 8-bit selector cannot address the catalog, and
    # reintroducing one must not be a warning.
    _narrow = [_p for _m, _mod in (schema.get("modules") or {}).items()
               for _p, _f in _iter_signal_selector_fields(_mod, f"{_m}.")
               if _f.get("type") == "uint8"]
    if _narrow:
        raise SystemExit("codegen: 8-bit options_from:signals selector(s) cannot address the "
                         f"{len(schema['signals'])}-signal catalog: {', '.join(_narrow)} — use uint16")

    normalize_module_config(schema)  # accept nested `config: {scalars,tables,arrays}`
    learned_blocks_as_tables(schema, modules)   # a learned block is a TABLE — same pipeline, different segment
    expand_inline_axes(modules)      # inline `x_axis: {signal,max,values}` -> owned <table>_<ax>_axis
    auto_table_channels(modules)     # synthesize per-axis <table>_<ax>_src/_en from each table's axes
    telem = telemetry_fields(schema, active_board)   # one OutputChannel per sensor channel (+raw)
    resolve_channel_name_defaults(schema)  # `default: clt` -> channel index (incl. the auto _src)
    sanitize_names(modules)        # coerce non-identifier names (spaces -> _) + warn
    resolve_array_counts(schema, active_board)   # count_from + count_from_board -> count
    expand_element_primitives(modules)   # element `type: table`/`type: array` -> flat fields + emit plan
    resolve_channel_name_defaults(schema)  # again: resolve any `signal:` defaults on element-table _src fields
    # Per-sensor defaults (interface/enabled/source/cal) MUST be applied before the default-image
    # generators (gen_default_config_cpp / gen_default_tune_cpp) — else the flashed image keeps the
    # generic scalar defaults while the .ini ships the per-sensor ones (mismatch). Idempotent, so
    # gen_ini's own call is a no-op afterward.
    synthesize_sensor_defaults(schema, modules, active_board)
    bake_raw_adc_fields(modules, active_board)               # `raw_adc` fields: authored mV -> stored counts
    # Resolve axis pairing + synthesize <axis>_n length scalars + annotate
    # MAX storage / INI dims. Mutates `modules`; must run before any generation.
    annotate_resizable(modules, int(schema.get("table_defaults", {}).get("axis_max", 32)))
    # Pad float tables to 4-byte offsets (Cortex-M7 VLDR faults on unaligned float loads in the
    # byte-packed EcuConfig), then assert it held — fail at codegen, not as a hard fault on hardware.
    align_float_config(prim_types, modules)
    assert_config_alignment(prim_types, modules)

    GEN_DIR.mkdir(parents=True, exist_ok=True)
    (GEN_DIR / "modules").mkdir(exist_ok=True)
    SHARED_DIR.mkdir(exist_ok=True)

    # Signal IDs — the channel namespace IS the first-class `signals:` catalog (the value
    # bus). Producers (sensors/modules/CAN/Lua) reference it; it is not derived.
    signals = schema["signals"]   # already in LOCK order — see apply_signal_id_lock() above
    sig_text = gen_signal_ids_h(signals)
    write_if_changed(GEN_DIR / "signal_ids.h", sig_text)
    print("  generated/signal_ids.h")
    write_if_changed(GEN_DIR / "hw_input_publish.inc", gen_hw_input_publish(active_board, schema))
    write_if_changed(GEN_DIR / "hw_input_keygate.inc", gen_hw_input_keygate(active_board, schema))
    print("  generated/hw_input_publish.inc")
    write_if_changed(GEN_DIR / "hw_input_map.h", gen_hw_input_map(active_board))
    print("  generated/hw_input_map.h")
    validate_firmware_signal_refs(signals, schema.get("well_known_signals", {}),
                                  schema.get("sensors", []))   # firmware↔schema contract
    validate_lua_api(schema)                                                       # lua_api ↔ ECU_API contract
    validate_trigger_wheels(schema)                                                # gap 0 == the sync tooth

    write_if_changed(GEN_DIR / "signal_enums.h", gen_signal_enums_h(schema))
    print("  generated/signal_enums.h")

    # Well-known signal ROLES — stable firmware vocabulary bound to signals by the schema, so a
    # signal rename never touches a .cpp (firmware uses wk::map, not the renameable SIG_MAP).
    wk_text = gen_well_known_signals_h(schema.get("well_known_signals", {}), signals)
    write_if_changed(GEN_DIR / "well_known_signals.h", wk_text)
    print("  generated/well_known_signals.h")


    # Sensor catalog (Tier 2) — read-only descriptor table.
    sensors_catalog = schema.get("sensors", [])
    if sensors_catalog:
        channel_ids = {c["id"] for c in signals}
        sensor_arr = next((a for a in modules.get("Sensors", {}).get("config_arrays", [])
                           if a.get("name") == "sensor"), {})
        cal_pts = sum(1 for f in sensor_arr.get("element", [])
                      if f.get("name", "").startswith("cal_raw_"))
        cat_text = gen_sensors_catalog_h(schema.get("sensor_types", []),
                                         sensors_catalog, channel_ids,
                                         enum_ids(schema, "sensor_interface"), cal_pts, schema)
        write_if_changed(GEN_DIR / "sensors_catalog.h", cat_text)
        doc = REPO_ROOT / "docs" / "dtc-codes.md"
        write_if_changed(doc, gen_dtc_reference_md(schema))
        print(f"  {doc}")
        print("  generated/sensors_catalog.h")

    # CAN device library (Tier 2 for CAN) — frame decoders a CAN-interface sensor binds to.

    # CAN protocol TEMPLATES — whole vehicle interfaces this ECU can speak, as studio data rather
    # than firmware tables. Validated against the signal catalog here so a stale signal name is a
    # build error rather than a field that silently transmits zero for ever.
    for tid, nf, ns in install_can_templates({c["id"] for c in signals}, SHARED_DIR):
        print(f"  shared/can_templates/{tid}.json  ({nf} frames, {ns} fields)")

    # Output bindings — the static OUTPUTS[] table the OutputManager builds pipelines from.
    outputs = schema.get("outputs", [])
    if outputs:
        write_if_changed(GEN_DIR / "outputs.h", gen_outputs_h(outputs))
        print("  generated/outputs.h")

    # DTC category matcher + a build-time audit that every sensor catalog P-code
    # falls in a category (the "use the right DTCs" guarantee — uncategorised codes
    # still surface via dtc_worst/OBD/LED, they just won't light a category indicator).
    dtc_cats = _dtc_categories(schema)
    write_if_changed(GEN_DIR / "dtc_categories.h", gen_dtc_categories_h(dtc_cats))
    print("  generated/dtc_categories.h")

    # Control-module signal-validity DTC constants (schema module_dtc) — firmware authority.
    write_if_changed(GEN_DIR / "module_dtc.h", gen_module_dtc_h(_module_dtc_entries(schema)))
    print("  generated/module_dtc.h")

    # The SD datalogger's MLG v2 field table — the format MegaLogViewer reads, built from the SAME
    # telemetry field list the frame itself comes from, so a column can never describe a value that is
    # not there. Only `datalog:` channels: each costs 89 header bytes in every file.
    # `telem` is the one telemetry field list (built above); collect_offsets assigns each field the
    # SAME byte offset the packed frame uses, which is what lets a record be gathered from that frame.
    _mlg_prim = schema["primitive_types"]
    _mlg_telem, _a, _b, _c, _d, _e = collect_offsets(_mlg_prim, schema["modules"], telem)
    write_if_changed(GEN_DIR / "mlg_log.h", gen_mlg_log_h(_mlg_telem, _mlg_prim))
    print("  generated/mlg_log.h")

    # Per-module scheduler cadence (firmware constant, NOT config) — see gen_module_cadence_h.
    write_if_changed(GEN_DIR / "module_cadence.h", gen_module_cadence_h(schema))
    print("  generated/module_cadence.h")
    if dtc_cats:
        # Check the codes the firmware will ACTUALLY raise (post-allocation), not the schema's declared
        # map: a declared code that lost a tie is not raised by that fault at all, so warning about it
        # sends you looking for a problem that isn't there — while the code truly in use goes unchecked.
        # ONE allocation, then the check. Calling it inside the loop re-derived every sensor's codes
        # for every sensor — a hundred passes over the same work to ask a hundred questions of it.
        _diag_codes = sensor_diag_dtcs(schema, schema.get("sensor_types", []), sensors_catalog)
        for s in sensors_catalog:
            for chk, code in _diag_codes[s["id"]].items():
                if code and _code_category_bit(code, dtc_cats) < 0:
                    print(f"  WARN: sensor '{s['id']}' dtc {chk}=P{code:04X} "
                          f"matches no dtc_indicators category",
                          file=sys.stderr)

    # Per-module config headers. (Telemetry is no longer per-module: EcuTelemetry is a
    # flat frame packed from the bus — see gen_ecu_telemetry_h.)
    for mod_name, mod in modules.items():
        if mod_name == "System":
            continue
        ms = module_snake(mod_name)

        cfg    = mod.get("config", [])
        tbls   = config_tables(mod)
        arrays = mod.get("config_arrays", [])
        if cfg or tbls or arrays:
            out = gen_module_config_h(prim_types, mod_name, cfg, tbls, arrays)
            write_if_changed(GEN_DIR / "modules" / f"{ms}_config.h", out)
            print(f"  generated/modules/{ms}_config.h")

    # Table descriptors — one tbl::TableDesc builder per configurable table (after the module config
    # headers it references). Modules call tbl::table_eval(<table>_desc(cfg_), bus) for every lookup.
    write_if_changed(GEN_DIR / "table_descs.h", gen_table_descs_h(modules))
    write_if_changed(GEN_DIR / "script_lookups.h", gen_script_lookups_h(modules))
    # …and the id-indexed view of the same tables, which is what an expression can name.
    write_if_changed(GEN_DIR / "table_registry.h", gen_table_registry_h(modules))
    print("  generated/table_descs.h")

    # Combined headers
    write_if_changed(GEN_DIR / "ecu_telemetry.h", gen_ecu_telemetry_h(prim_types, telem))
    print("  generated/ecu_telemetry.h")

    # Well-known telemetry accessors (wkt::<role>(t)) — rename-safe member access for OBD/comms.
    telem_members = {f["name"] for f in telem}
    write_if_changed(GEN_DIR / "well_known_telem.h", 
        gen_well_known_telem_h(schema.get("well_known_signals", {}), telem_members))
    print("  generated/well_known_telem.h")

    write_if_changed(GEN_DIR / "ecu_config.h", gen_ecu_config_h(prim_types, modules))
    print("  generated/ecu_config.h")

    write_if_changed(GEN_DIR / "learned_layout.h", gen_learned_layout_h(schema))
    print("  generated/learned_layout.h")

    write_if_changed(GEN_DIR / "sensor_telem_pack.inc", gen_sensor_telem_pack(schema, active_board))
    print("  generated/sensor_telem_pack.inc")

    total_telem = sum(field_size(prim_types, f) for f in telem)
    total_cfg = 4 + sum(
        struct_size(prim_types, mod.get("config", []), config_tables(mod),
                    mod.get("config_arrays", []))
        for mod_name, mod in modules.items()
        if mod_name != "System"
    )  # layout_hash (field 0) + modules; no trailing CRC field

    # The on-flash config identity: a content hash of the byte layout. One value feeds the
    # firmware boot gate (schema_meta.h), the default-tune image, and the meta-match key.
    layout_hash = compute_layout_hash(prim_types, modules, schema.get("signals"), telem)

    write_if_changed(GEN_DIR / "schema_meta.h", 
        gen_schema_meta_h(schema, total_telem, total_cfg, layout_hash))
    print("  generated/schema_meta.h")

    write_if_changed(GEN_DIR / "protocol.h", gen_protocol_h())
    print("  generated/protocol.h")

    write_if_changed(GEN_DIR / "shadow_meta.h", 
        gen_shadow_meta_h(prim_types, compute_shadow_regions(prim_types, modules)))
    print("  generated/shadow_meta.h")

    write_if_changed(GEN_DIR / "default_config.cpp", gen_default_config_cpp(prim_types, modules))
    print("  generated/default_config.cpp")

    write_if_changed(GEN_DIR / "default_tune.cpp", 
        gen_default_tune_cpp(prim_types, modules, layout_hash))
    print("  generated/default_tune.cpp")

    # version_str/git_hash/board + board_yaml resolved at the top of main() (the raw telemetry
    # needs the board before synthesize). --board overrides the pin-dropdown board; the meta
    # identity's board still names the version.h profile.
    # Resolved Data Dictionary — the single app-facing contract: fully-resolved config/telemetry
    # layout (same byte offsets as the C++ structs), hierarchical for the nav tree. The `meta`
    # block carries the device identity: layout_hash (exact match) + product/board/fw_version
    # (GUI family tag) + fw_build (display).
    meta_text = gen_tuneit_meta(schema, active_board, product="jayecu", board_profile=board,
                                fw_version=version_str, fw_build=git_hash, layout_hash=layout_hash,
                                min_studio=_read_min_studio())
    # THE FORMAT NUMBER, checked against the structure it names (tools/meta_format.py). A meta whose
    # structure moved without a new format would be read by an older studio as the format it knows —
    # so the build stops and says what moved, and the studio refuses any format newer than its own.
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
    import meta_format
    _doc = json.loads(meta_text)
    _fmt, _moved = meta_format.check(_doc)
    if _moved:
        sys.exit("ERROR: " + _moved)
    _doc["meta"] = {**{k: v for k, v in _doc["meta"].items() if k != "meta_format"}, "meta_format": _fmt}
    meta_text = json.dumps(_doc, indent=2)
    meta_bytes = meta_text.encode()
    meta_crc   = zlib.crc32(meta_bytes) & 0xFFFFFFFF
    meta_path  = SHARED_DIR / "tuneit-meta.json"
    meta_path.write_bytes(meta_bytes + struct.pack("<I", meta_crc))
    print("  shared/tuneit-meta.json")

    print(f"\nTelemetry buffer : {total_telem} bytes")
    print(f"Config struct    : {total_cfg} bytes")

    # --- Table storage cost report ("compute all tables up front to check total cost") ---
    # Every table reserves its MAX allocation (resizable axes reserve max_size); this tallies the
    # worst-case storage across the whole config so the table budget is visible as tables are added.
    FLASH_BANK_BYTES = 256 * 1024                       # one bank (A/B flip); collapse → 512 KB
    TABLE_BUDGET_BYTES = int(0.80 * FLASH_BANK_BYTES)    # warn past 80% of a single bank
    rows = []
    total_table = 0
    for mod_name, mod in modules.items():
        if mod_name == "System":
            continue
        for t in mod.get("tables", []):
            b = table_size(prim_types, t)
            total_table += b
            cell = type_info(prim_types, t["type"])["size"]
            rows.append((f"{mod_name}.{t['name']}", t.get("_alloc_elems", t.get("size", 0)), cell, b))
    rows.sort(key=lambda r: -r[3])
    print(f"\n=== Table storage (worst-case max allocation) ===")
    for name, cells, cell, b in rows:
        print(f"  {name:46s} {cells:6d} cells x {cell}B = {b:7d} B")
    pct = 100.0 * total_table / FLASH_BANK_BYTES
    print(f"  {'-'*46} {'':14s} {'-'*9}")
    print(f"  {'TOTAL table storage':46s} {'':21s}{total_table:7d} B  ({pct:.1f}% of 256 KB bank)")
    print(f"  {'config struct (incl. tables)':46s} {'':21s}{total_cfg:7d} B  "
          f"({100.0*total_cfg/FLASH_BANK_BYTES:.1f}% of 256 KB bank)")
    if total_table > TABLE_BUDGET_BYTES:
        print(f"  WARNING: table storage {total_table} B exceeds 80% of a 256 KB bank — "
              f"shrink axes/depths or collapse to a 512 KB config.")

if __name__ == "__main__":
    print("JayECU codegen:")
    main()
    print("Done.")
