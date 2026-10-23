#!/usr/bin/env python3
"""Make a tune the schema's defaults — so a fresh ECU ships with it.

    python3 tools/default_tune_to_schema.py <default.tune> <source.tune> [--write]

(run `make codegen` first: element order of catalog-sized arrays is read from shared/tuneit-meta.json)

<default.tune> is the CURRENT default tune as a dictionary (tune_migrate <meta> <default.img> --from <meta>
--dict-out <default.tune>, the image being the meta's `default_tune`); <source.tune> is the tune to adopt
(a Studio .tune of the same layout). Every path that differs is written into definition/ecu.schema.yaml as
that field's default — EXCEPT the wiring, which belongs to the car and not to the firmware:

  * the output slot table (outputs.output[]: each slot IS a pin) and its expressions
  * which pin/interface a sensor reads and whether it is fitted (sensor source/interface/enabled/flags),
    the thermistor bias mode, which capture channel a trigger stream uses and whether it is enabled
  * whether drive-by-wire hardware is fitted (ETB, H-bridge, pedal): with the no-start-unproven-throttle
    interlock, a default that enables a throttle the car does not have is an engine that never starts
  * the car's identity (vehicle.*)
  * a signal selector pointing at an OUTPUT SLOT's channel (out_N — a pin, and board-specific)

THE YAML IS EDITED IN PLACE, NOT REWRITTEN. PyYAML's composer gives every node its source span, so only the
value text of each changed key is replaced (or a key inserted) and every comment, alignment and line break
around it survives. Run without --write to see the plan.

Synthesised companions (<table>_<ax>_axis_n, _src, _en) are not edited: codegen derives them from the
table/axis declarations, so they follow when the declaration changes. Verify by regenerating, exporting the
new default tune the same way, and diffing it against <source.tune>: only the excluded wiring may remain.
"""
import json
import re
import sys
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parent.parent
SCHEMA = REPO / "definition" / "ecu.schema.yaml"
sys.path.insert(0, str(REPO / "codegen"))
from codegen import module_snake  # noqa: E402  (one naming rule, not a copy of it)

EXCLUDE = [
    r"^outputs\.",
    r"^sensors\.sensor\[[^\]]+\]\.(source|interface|enabled|flags)$",
    r"^sensors\.thermistor\[[^\]]+\]\.mode$",
    r"^trigger\.streams\[[^\]]+\]\.(capture_index|enabled)$",
    r"^electronic_throttle\.etb\[[^\]]+\]\.enabled$",
    r"^h_bridge\.half\[[^\]]+\]\.enabled$",
    r"^app\.enabled$",
    r"^vehicle\.",
]


def excluded(path):
    return any(re.search(p, path) for p in EXCLUDE)


# --------------------------------------------------------------------------------------------- YAML spans
class Doc:
    """The schema text plus its composed node tree; edits are queued as (start, end, text) and applied
    back-to-front so earlier offsets stay valid."""

    def __init__(self, text):
        self.text = text
        self.root = yaml.compose(text)
        self.edits = []

    @staticmethod
    def get(node, key):
        if not isinstance(node, yaml.MappingNode):
            return None
        for k, v in node.value:
            if k.value == key:
                return v
        return None

    def replace(self, node, text):
        self.edits.append((node.start_mark.index, node.end_mark.index, text))

    def value_span(self, k, v):
        """Where the value text of `k: v` really lies. A block value (`key:` then the value on the following
        lines) is replaced from just after the colon, so the flow text that replaces it sits on the key's own
        line; and it ends at its LAST ITEM (plus a comment on that line), because a block node's end mark is
        the start of the next key's line — replacing up to it eats that key's indentation."""
        s, e = v.start_mark.index, v.end_mark.index
        if v.start_mark.line != k.start_mark.line:
            s = self.text.index(":", k.end_mark.index) + 1
            last = v
            while isinstance(last, (yaml.SequenceNode, yaml.MappingNode)) and not last.flow_style and last.value:
                last = last.value[-1] if isinstance(last, yaml.SequenceNode) else last.value[-1][1]
            e = last.end_mark.index
            nl = self.text.find("\n", e)
            tail = self.text[e:nl if nl != -1 else len(self.text)]
            if tail.strip().startswith("#") or not tail.strip():
                e = nl if nl != -1 else len(self.text)
            return s, e, " "
        return s, e, ""

    def set_key(self, mapping, key, text):
        """Set mapping[key] = text (raw YAML), inserting the key if absent."""
        for k, v in mapping.value:
            if k.value == key:
                s, e, lead = self.value_span(k, v)
                self.edits.append((s, e, lead + text))
                return
        if mapping.flow_style:
            end = mapping.end_mark.index - 1                       # the closing brace
            self.edits.append((end, end, f", {key}: {text}"))
        else:
            # After the last key's value — where it REALLY ends (value_span: a nested block's end mark is the
            # next line's start) — on a new line at the mapping's own indent.
            indent = mapping.value[0][0].start_mark.column
            last_k, last_v = mapping.value[-1]
            _, e, _ = self.value_span(last_k, last_v)
            nl = self.text.find("\n", e - 1 if e > 0 and self.text[e - 1] == "\n" else e)
            at = e - 1 if e > 0 and self.text[e - 1] == "\n" else (nl if nl != -1 else len(self.text))
            self.edits.append((at, at, "\n" + " " * indent + f"{key}: {text}"))

    def drop_key(self, mapping, key):
        """Remove `key: value` from a block mapping (whole line(s)); flow mappings are rewritten by set_key."""
        for i, (k, v) in enumerate(mapping.value):
            if k.value == key:
                if mapping.flow_style:
                    if i > 0:                                     # ", key: value" — from the comma before it
                        s = self.text.rfind(",", 0, k.start_mark.index)
                        self.edits.append((s, v.end_mark.index, ""))
                    else:                                         # "{key: value, " — through the comma after it
                        e = self.text.find(",", v.end_mark.index) + 1
                        while self.text[e] == " ":
                            e += 1
                        self.edits.append((k.start_mark.index, e, ""))
                    return
                _, e, _ = self.value_span(k, v)
                if v.start_mark.line == k.start_mark.line:        # value on the key's line: to that line's end
                    nl = self.text.find("\n", e)
                    e = nl if nl != -1 else len(self.text)
                s = self.text.rfind("\n", 0, k.start_mark.index)  # from the newline BEFORE the key's line
                self.edits.append((s, e, ""))
                return

    def apply(self):
        out = self.text
        ed = sorted(self.edits, key=lambda x: (x[0], x[1]))
        for (s1, e1, t1), (s2, e2, t2) in zip(ed, ed[1:]):
            if s2 < e1:                                           # two edits on one span: refuse, never guess
                line = self.text.count("\n", 0, s1) + 1
                raise ValueError(f"overlapping edits near line {line}: [{s1},{e1}) {t1[:40]!r} / [{s2},{e2}) {t2[:40]!r}")
        for s, e, t in reversed(ed):
            out = out[:s] + t + out[e:]
        return out


def set_block_string(doc, mapping, key, value):
    """Write a multi-line string as a YAML block literal, indented under its key. `|-` when the value has
    no trailing newline, so it loads back byte for byte."""
    for k, v in mapping.value:
        if k.value == key:
            indent = " " * (k.start_mark.column + 2)
            body = "\n".join((indent + ln) if ln else "" for ln in value.rstrip("\n").split("\n"))
            chomp = "|" if value.endswith("\n") else "|-"
            s = doc.text.index(":", k.end_mark.index) + 1
            e = v.end_mark.index                                   # a block scalar ends at the next line's start
            if v.style in ("|", ">"):
                doc.edits.append((s, e, f" {chomp}\n{body}\n"))
            else:
                doc.edits.append((s, e, " " + json.dumps(value)))
            return
    raise KeyError(key)


def fmt(v):
    """A value as one line of flow YAML."""
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, float):
        return repr(int(v)) if v == int(v) and abs(v) < 1e15 else repr(v)
    if isinstance(v, (int,)):
        return str(v)
    if isinstance(v, str):
        return json.dumps(v)                                      # JSON strings are valid YAML scalars
    if isinstance(v, list):
        return "[" + ", ".join(fmt(x) for x in v) + "]"
    raise TypeError(v)


# --------------------------------------------------------------------------------------------- the schema
class Schema:
    def __init__(self, doc):
        self.doc = doc
        self.data = yaml.safe_load(doc.text)
        root = doc.root
        self.enums = self.data.get("enums", {})
        mods = Doc.get(root, "modules")
        self.mods = {}                                            # snake -> (yaml MappingNode, dict)
        for k, v in mods.value:
            self.mods[module_snake(k.value)] = (v, self.data["modules"][k.value])

    def section(self, mod, sec):
        node, _ = self.mods[mod]
        cfg = Doc.get(node, "config")
        return Doc.get(cfg, sec) if cfg is not None else None

    def item(self, mod, sec, name):
        seq = self.section(mod, sec)
        if seq is None:
            return None
        for it in seq.value:
            n = Doc.get(it, "name")
            if n is not None and n.value == name:
                return it
        return None

    def enum_index(self, field_node, label):
        """A tune stores an enum by its label; the schema default is the index."""
        opts = None
        o = Doc.get(field_node, "options")
        if o is not None:
            opts = [x.value for x in o.value]
        e = Doc.get(field_node, "enum")
        if opts is None and e is not None:
            en = self.enums[e.value]
            opts = list(en) if isinstance(en, list) else [x if isinstance(x, str) else x.get("label") for x in en.get("values", en)]
        if opts is None:
            raise KeyError("no options")
        labels = [x if isinstance(x, str) else str(x) for x in opts]
        if label in labels:
            return labels.index(label)
        raise KeyError(f"label {label!r} not in {labels}")


    def elem_field(self, mod, arr, field):
        """(yaml node, dict) of a config-array element field."""
        seq = self.section(mod, "arrays")
        _, mdict = self.mods[mod]
        for it, d in zip(seq.value if seq is not None else [], mdict["config"].get("arrays", [])):
            if d.get("name") != arr:
                continue
            for fn, fd in zip(Doc.get(it, "element").value, d["element"]):
                if fd.get("name") == field:
                    return fn, fd, d
        return None, None, None

    def elem_keys(self, arr_dict):
        """The tune's index for each element: the sensor catalog's ids for sensor arrays, else 0..n-1."""
        # A catalog/board-sized array is EXPANDED by codegen (one wideband/EGT per cylinder…), so its element
        # order is the generated meta's, not the schema's source list.
        if arr_dict.get("count_from"):
            ids = META_IDS.get((self.mod_of(arr_dict), arr_dict["name"]))
            return ids
        return list(range(int(arr_dict["count"])))

    def mod_of(self, arr_dict):
        for snake, (_, md) in self.mods.items():
            if any(a is arr_dict for a in md.get("config", {}).get("arrays", [])):
                return snake
        return None


def load_meta_ids(path):
    """element_ids of every config array, from the generated meta (shared/tuneit-meta.json)."""
    raw = Path(path).read_bytes()
    meta = json.loads(raw[:raw.rfind(b"}") + 1].decode())
    out = {}
    for mod, arrs in meta["config"].items():
        if isinstance(arrs, dict):
            for name, a in arrs.items():
                if isinstance(a, dict) and isinstance(a.get("element_ids"), list):
                    out[(mod, name)] = a["element_ids"]
    return out


def sensor_cal_default(doc, sch, sid, tbl):
    """A sensor's default calibration lives in the CATALOG (top-level `sensors:`), authored as
    [[raw, value], ...]: raw in mV for an analog input (baked to counts by codegen), native units otherwise
    (Hz, µs); value in engineering units, stored by codegen at the sensor type's decimals."""
    cat = Doc.get(doc.root, "sensors")
    entry = None
    for it in cat.value:
        i = Doc.get(it, "id")
        if i is not None and i.value == sid:
            entry = it
    if entry is None:                                             # an expanded instance (lambda_1 <- lambda)
        base = re.sub(r"_\d+$", "", sid)
        for it in cat.value:
            i = Doc.get(it, "id")
            if i is not None and i.value == base:
                entry = it
    if entry is None:
        return False
    d = yaml.safe_load(doc.text[entry.start_mark.index:entry.end_mark.index]) if entry.flow_style else None
    ifaces = [x.value for x in Doc.get(entry, "interfaces").value] if Doc.get(entry, "interfaces") is not None else []
    stype = Doc.get(entry, "type").value
    dec = next((int(t.get("decimals", 2)) for t in sch.data.get("sensor_types", []) if t["id"] == stype), 2)
    analog = any(x in ifaces for x in ("analog_voltage", "engine_sync_voltage", "analog_temp"))
    n = tbl["n"][0]
    raws = tbl["a"][0][:n]
    vals = tbl["c"][:n]
    pts = []
    for r, c in zip(raws, vals):
        raw = counts_to_mv(r) if analog else r
        val = round(c * 10 ** -dec, dec)
        pts.append([raw, int(val) if val == int(val) else val])
    doc.set_key(entry, "default_cal", fmt(pts))
    return True


ADC_FULL_SCALE, ADC_AV_MV = 4095, 5000      # jaytek_v1 and proteus_f7 alike (board yaml: analog_av_fullscale_mv)


def counts_to_mv(c):
    mv = round(c * ADC_AV_MV / ADC_FULL_SCALE)
    for cand in (mv, mv - 1, mv + 1):
        if round(cand * ADC_FULL_SCALE / ADC_AV_MV) == c:
            return cand
    raise ValueError(f"no whole mV bakes to {c} counts")


META_IDS = {}
SIGNAL_NAMES = ("sig", "signal")


def is_signal_field(fd):
    n = fd.get("name", "")
    return fd.get("control") == "signal" or n.endswith("_src") or n.endswith("_sig") or n in SIGNAL_NAMES


def main():
    dflt = json.loads(Path(sys.argv[1]).read_text())
    src = json.loads(Path(sys.argv[2]).read_text())
    write = "--write" in sys.argv
    META_IDS.update(load_meta_ids(REPO / "shared" / "tuneit-meta.json"))
    doc = Doc(SCHEMA.read_text())
    sch = Schema(doc)
    done, skipped, todo = [], [], []

    # ---- module scalars -------------------------------------------------------------------------
    for path in sorted(set(dflt["scalars"]) | set(src["scalars"])):
        a, b = dflt["scalars"].get(path), src["scalars"].get(path)
        if a == b:
            continue
        if excluded(path):
            skipped.append(path); continue
        if re.search(r"_(axis_n|src|en)$", path) and path.count(".") == 1:
            continue                                              # derived from the table/axis edits below
        mod, field = path.split(".", 1)
        it = sch.item(mod, "scalars", field) if mod in sch.mods else None
        if it is None:
            todo.append(("scalar not found", path)); continue
        t = Doc.get(it, "type").value if Doc.get(it, "type") is not None else ""
        v = b
        if t == "enum" or (isinstance(b, str) and t not in ("string",)):
            try:
                v = sch.enum_index(it, b)
            except KeyError as e:
                if t != "enum":
                    v = b                                         # a signal selector: stored by name, as the schema writes it
                else:
                    todo.append((f"enum {e}", path)); continue
        if t == "string" and "\n" in str(v):
            set_block_string(doc, it, "default", str(v))
            done.append((path, "string", "")); continue
        doc.set_key(it, "default", fmt(v))
        done.append((path, a, b))

    # ---- tables ---------------------------------------------------------------------------------
    AX = ("x", "y", "z")
    DIM = {"x": "cols", "y": "rows", "z": "depth"}
    for path in sorted(set(dflt["tables"]) | set(src["tables"])):
        a, b = dflt["tables"].get(path), src["tables"].get(path)
        if a == b:
            continue
        if excluded(path):
            skipped.append(path); continue
        if "[" in path:
            m = re.match(r"^([a-z0-9_]+)\.([a-z0-9_]+)\[([^\]]+)\]\.([a-z0-9_]+)$", path)
            mod, arr, key, tname = m.groups()
            if mod == "sensors" and arr == "sensor" and tname == "cal":
                if sensor_cal_default(doc, sch, key, b):
                    done.append((path, "sensor cal", ""))
                else:
                    todo.append(("sensor cal: catalog entry not found", path))
                continue
            fn, fd, ad = sch.elem_field(mod, arr, tname)
            if fn is not None and fd.get("type") == "table" and key in ("0",):
                doc.set_key(fn, "cells", fmt(list(b["c"])))       # seeds every element; etb[0] is the fitted one
                done.append((path, "element table", ""))
                continue
            todo.append(("element table", path)); continue
        mod, name = path.split(".", 1)
        it = sch.item(mod, "tables", name)
        if it is None:
            todo.append(("table not found", path)); continue
        n = list(b["n"])
        cells = list(b["c"])
        live = 1
        for k in n[:2]:
            live *= k
        if len(n) == 3 and len(cells) == live:                    # Z off: one plane stored -> tile it (enabling Z is a no-op)
            cells = cells * n[2]
        for i, ax in enumerate(AX[:len(n)]):
            spec = Doc.get(it, f"{ax}_axis")
            if spec is None:
                continue
            # The breakpoints come from the AXES section: a table entry's own copy can be short (a Z axis
            # whose plane is off is stored as [0] even with two planes live).
            full = src["axes"].get(f"{path}_{ax}_axis")
            vals = (full[:n[i]] if full is not None
                    else (b["a"][i][:n[i]] if i < len(b.get("a", [])) else None))
            sig = b["s"][i] if i < len(b.get("s", [])) else None
            en = b["e"][i] if i < len(b.get("e", [])) else -1
            if isinstance(spec, yaml.MappingNode):                # inline, table-owned axis
                if vals is not None:
                    doc.set_key(spec, "values", fmt(vals))
                if sig and sig != "none" and Doc.get(spec, "signal") is not None:
                    doc.set_key(spec, "signal", sig)
                cur = Doc.get(spec, "en_default")
                cur = int(cur.value) if cur is not None else 0
                if en in (0, 1) and Doc.get(spec, "optional") is not None and en != cur:
                    doc.set_key(spec, "en_default", str(en))
            else:                                                 # legacy: a named, separately declared axis
                if Doc.get(it, DIM[ax]) is not None:
                    doc.set_key(it, DIM[ax], str(n[i]))
                if sig and sig != "none" and Doc.get(it, f"{ax}_channel") is not None:
                    doc.set_key(it, f"{ax}_channel", sig)
                cur = Doc.get(it, f"{ax}_en_default")
                cur = int(cur.value) if cur is not None else 0
                if en in (0, 1) and Doc.get(it, f"{ax}_optional") is not None and en != cur:
                    doc.set_key(it, f"{ax}_en_default", str(en))
        if Doc.get(it, "default_row") is not None:
            doc.drop_key(it, "default_row")
        if Doc.get(it, "default") is not None and Doc.get(it, "default_values") is None:
            pass                                                  # uniform fill stays the fallback; default_values wins
        doc.set_key(it, "default_values", fmt(cells))
        done.append((path, "table", ""))

    # ---- shared (legacy) axes -------------------------------------------------------------------
    for path in sorted(set(dflt["axes"]) | set(src["axes"])):
        a, b = dflt["axes"].get(path), src["axes"].get(path)
        if a == b or excluded(path):
            continue
        mod, name = path.split(".", 1)
        it = sch.item(mod, "tables", name)
        if it is None or Doc.get(it, "max_size") is None:
            continue                                              # an inline axis: handled with its table
        nkey = f"{name}_n"
        live = src["scalars"].get(f"{mod}.{nkey}")
        if live is None:
            todo.append(("axis live count unknown", path)); continue
        # The axis' FLOOR is min(min_size (2), size): growing the default size would quietly raise the
        # floor too (a 1-bin SG axis could no longer be collapsed back to one cell). Pin the old floor.
        old_size = int(Doc.get(it, "size").value) if Doc.get(it, "size") is not None else 2
        if Doc.get(it, "min_size") is None and old_size < 2 and live > old_size:
            doc.set_key(it, "min_size", str(old_size))
        doc.set_key(it, "size", str(live))
        doc.set_key(it, "default_values", fmt(list(b[:live])))
        done.append((path, "axis", live))

    # ---- config arrays: one default per element ---------------------------------------------------
    groups = {}
    for path in sorted(set(dflt["arrays"]) | set(src["arrays"])):
        if dflt["arrays"].get(path) == src["arrays"].get(path):
            continue
        if excluded(path):
            skipped.append(path); continue
        m = re.match(r"^([a-z0-9_]+)\.([a-z0-9_]+)\[([^\]]+)\]\.([a-z0-9_]+)$", path)
        if not m:
            todo.append(("array path shape", path)); continue
        groups.setdefault((m.group(1), m.group(2), m.group(4)), []).append(path)
    for (mod, arr, field), paths in sorted(groups.items()):
        fn, fd, ad = sch.elem_field(mod, arr, field)
        if fn is None:
            todo.append(("element field not found", f"{mod}.{arr}[].{field}")); continue
        keys = sch.elem_keys(ad)
        if keys is None:
            todo.append(("board-derived element count", f"{mod}.{arr}[].{field}")); continue
        cur = fd.get("default", 0)
        t = fd.get("type", "")
        vals = []
        for i, key in enumerate(keys):
            v = src["arrays"].get(f"{mod}.{arr}[{key}].{field}")
            if v is None:
                v = cur[i] if isinstance(cur, list) and i < len(cur) else (cur[-1] if isinstance(cur, list) and cur else cur)
            elif t == "enum" and isinstance(v, str):
                v = sch.enum_index(fn, v)
            elif is_signal_field(fd) and isinstance(v, str) and (v in ("none", "") or re.match(r"^out_\d+$", v)):
                # An OUTPUT SLOT's channel is a pin's: board-specific (jaytek has an out_42, proteus does not)
                # and wiring, which stays with the car. Ship it unassigned.
                v = 0xFFFF if t == "uint16" else -1
            vals.append(v)
        if t == "string":
            vals = ["" if v is None else str(v) for v in vals]
        if fd.get("raw_adc"):
            # AUTHORED IN mV, STORED IN COUNTS: codegen bakes mV -> round(mV * 4095 / 5000) per board. The tune
            # holds counts, so write the mV that bakes back to exactly those counts.
            vals = [counts_to_mv(v) for v in vals]
        default = vals[0] if all(v == vals[0] for v in vals) else vals
        doc.set_key(fn, "default", fmt(default))
        done.append((f"{mod}.{arr}[].{field}", len(paths), ""))

    print(f"edits planned: {len(doc.edits)}  fields: {len(done)}  excluded: {len(skipped)}  not yet handled: {len(todo)}")
    for r, p in todo:
        print(f"   TODO {r}: {p}")
    if write:
        SCHEMA.write_text(doc.apply())
        print(f"wrote {SCHEMA}")


if __name__ == "__main__":
    main()
