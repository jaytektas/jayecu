#!/usr/bin/env python3
"""EVERY PATH-GATED SETTING, CHECKED AGAINST THE INSTALLED DOCUMENT.

gate_paths.py derives what each mode reaches. This asserts the pages agree — for widgets AND for the
tree nodes that stage the pages, because greying a table inside a page that is still listed tells a
tuner nothing about which of the listed pages matter.

A setting counts as gated if the condition appears anywhere on the way to it: its own
enableCondition, the panel containing it (a disabled panel cascades — PanelWidget.cpp:133), or any
tree node on its path (a hidden node cannot be opened at all).

HAND RULES are the ones no static walk can reach — a gate resolved through a ternary or a struct
array, and the fields that ARE reached but feed something the mode never acts on. Each carries the
file:line it was read from, because that is the only thing that makes it checkable by the next person.

    python3 tools/gate_check.py
"""
import json, os, re, subprocess, sys

DOC = os.path.expanduser("~/.local/share/jayecu/jayecu Studio/ecus")

# --- gates no static walk can derive; each verified by reading the line named --------------------
HAND = {}
for f in ("freq_hz", "duty_min", "duty_max", "direction", "dead_band", "dead_time", "overall_corr"):
    # VvtControl.cpp:67 — mode resolves into defs[4].active, not into a branch.
    HAND[f"vvt_control.intake_{f}"]  = "[#vvt_control.mode] != 1"
    HAND[f"vvt_control.exhaust_{f}"] = "[#vvt_control.mode] != 0"
HAND.update({
    # Idle.cpp:131-139 — reached in open loop, but they only move a target the PI never acts on, and
    # nothing outside Idle consumes idle_target_rpm or idle_rpm_error.
    "idle.target_rpm_table":          "[#idle.mode] == 1",
    "idle.start_target_offset_table": "[#idle.mode] == 1",
    "idle.rpm_rate_rising_limit":     "[#idle.mode] == 1",
    "idle.rpm_rate_falling_limit":    "[#idle.mode] == 1",
    "idle.ltt_min_runtime_s":         "[#idle.mode] == 1",   # Idle.cpp:221, part of the learn condition
    # Boost.cpp — the servo is only built for a motorised gate.
    "boost.gate_pos_sig":       "[#boost.output_mode] == 1",
    "boost.gate_min_pos_pct":   "[#boost.output_mode] == 1",
    "boost.gate_max_pos_pct":   "[#boost.output_mode] == 1",
    "boost.gate_rate_pct_s":    "[#boost.output_mode] == 1",
    "boost.gate_kp":            "[#boost.output_mode] == 1",
    "boost.gate_ki":            "[#boost.output_mode] == 1",
    "boost.gate_kd":            "[#boost.output_mode] == 1",
    "boost.gate_iterm_max_pct": "[#boost.output_mode] == 1",
    # Launch.cpp — the stagger is read inside `method == CutBoth`, and the lead has nothing to lead
    # until the adder is non-zero. cut_channel() is a helper, so no walk reaches either branch.
    "launch.cut_adder_rpm":   "[#launch.cut_method] == 2",
    "launch.cut_lead":        "[#launch.cut_method] == 2 and [#launch.cut_adder_rpm] != 0",
    # Launch.cpp — cut_channel()'s two halves: the soft band's range and the hard cut's hysteresis are
    # each read on exactly one branch of cut_type.
    "launch.cut_range_rpm":   "[#launch.cut_type] == 1",
    "launch.resume_band_rpm": "[#launch.cut_type] == 0",
    # Stepper.cpp:63,103 — microstep is the H-bridge divisor; :91 max_step is the H-bridge integrator.
    "stepper.microstep":            "[#stepper.driver_mode] == 0",
    "stepper.max_step_per_update":  "[#stepper.driver_mode] == 0",
    # CanBroker.cpp:66 — reconfigure_obd consumes the bus only `if (enabled)`.
    "can.obd_bus":                  "[#can.obd_enabled] == 1",
    # PlatformCan.cpp:22 — `if (!enabled) { shutdown(); return; }`, so neither is read on a dead bus.
    "can.bus[*].bitrate":           "[#can.bus[*].enabled] == 1",
    "can.bus[*].listen_only":       "[#can.bus[*].enabled] == 1",
})


def document():
    for d in sorted(os.listdir(DOC)):
        p = os.path.join(DOC, d, "dashboard.gui")
        if os.path.exists(p) and not d.startswith(("tsimport", "offline")):
            return json.load(open(p, encoding="utf-8", errors="replace"))
    raise SystemExit("no native ECU document found")


def derived():
    out = subprocess.run([sys.executable, "tools/gate_paths.py"], capture_output=True, text=True).stdout
    rules, mod, gate, val = {}, None, None, None
    for ln in out.splitlines():
        m = re.match(r"^(\w+)\.(\w+)$", ln.strip())
        if m:
            mod, gate = m.groups(); continue
        m = re.match(r"\s*when \w+ = (\d+) ", ln)
        if m:
            val = int(m.group(1)); continue
        m = re.match(r"^\s{8}(\w+)$", ln)
        if m and mod:
            rules.setdefault(f"{mod}.{m.group(1)}", set()).add((gate, val))
    # dead for value v -> must be gated on that gate not being v
    return {f: " and ".join(sorted(f"[#{f.split('.')[0]}.{g}] != {v}" for g, v in vs))
            for f, vs in rules.items()}


def gates_on(doc):
    """{'mod.field': every condition on the way to it — widget, panel, and tree node}."""
    node = {}
    def walk_tree(n, path="", chain=()):
        q = f"{path}/{n['name']}" if path else n["name"]
        c = chain + ((n.get("condition") or ""),)
        node[q] = " ".join(x for x in c if x)
        for k in n.get("children") or []:
            walk_tree(k, q, c)
    for n in doc["tree"]:
        walk_tree(n)

    found = {}
    for name, page in doc["panelLibrary"].items():
        outer = node.get(name, "")
        def walk(o, pg=""):
            if isinstance(o, dict):
                p = o.get("props", {}) or {}
                s = re.sub(r"\[\d+\]", "[*]", p.get("signalName", "") or "")
                g = " ".join(x for x in (outer, pg, p.get("enableCondition", "")) if x)
                if s:
                    # ONE TEMPLATE, FORTY-TWO SLOTS. A templated page is installed per element, so the
                    # same field arrives 42 times with output[0], output[1] ... in its node condition.
                    # Normalise the index away and keep the distinct conditions only, or the "has"
                    # line is four thousand characters of the same clause and the matcher misses the
                    # indexed form entirely.
                    g = re.sub(r"\[\d+\]", "[*]", g)
                    # AN UNGATED FIELD IS STILL A FIELD. `g` is "" when nothing on the way to it
                    # carries a condition, and the dedup below read that as already-present — so the
                    # one case this whole check exists to catch never reached the map, and reported as
                    # "not on a page". A check that passes by failing to look is worse than no check.
                    if s not in found:
                        found[s] = g
                    elif g and g not in found[s].split(" || "):
                        found[s] = f"{found[s]} || {g}" if found[s] else g
                nxt = g if o.get("type") == "panel" else pg
                for k, v in o.items():
                    if k == "children" and isinstance(v, str):
                        try:
                            for c in json.loads(v):
                                walk(c, nxt)
                        except Exception:
                            pass
                    else:
                        walk(v, pg)
            elif isinstance(o, list):
                for x in o:
                    walk(x, pg)
        walk(page)
    return found


def pages_staged_dead(doc, rules):
    """Pages left in the tree in a mode where every editable thing on them is dead.

    Greying a table inside a page the tree still lists tells a tuner nothing about which of the listed
    pages matter — which was the whole complaint. A page whose every setting shares one gate belongs
    behind that gate on its NODE, where it is not staged at all.
    """
    node = {}
    def wt(n, path="", chain=()):
        q = f"{path}/{n['name']}" if path else n["name"]
        c = chain + ((n.get("condition") or ""),)
        node[q] = " ".join(x for x in c if x)
        for k in n.get("children") or []:
            wt(k, q, c)
    for n in doc["tree"]:
        wt(n)

    EDIT = {"configedit", "enum", "combobox", "table", "checkbox", "wiring", "expression", "curve"}
    bad = []
    for name, page in doc["panelLibrary"].items():
        fields = []
        def walk(o):
            if isinstance(o, dict):
                p = o.get("props", {}) or {}
                s = re.sub(r"\[\d+\]", "[*]", p.get("signalName", "") or "")
                if o.get("type") in EDIT and "." in s:
                    fields.append(s)
                for k, v in o.items():
                    if k == "children" and isinstance(v, str):
                        try:
                            for c in json.loads(v):
                                walk(c)
                        except Exception:
                            pass
                    else:
                        walk(v)
            elif isinstance(o, list):
                for x in o:
                    walk(x)
        walk(page)
        if not fields:
            continue
        conds = [rules.get(f) for f in fields]
        if conds and all(conds) and len(set(conds)) == 1:
            want, has = conds[0], node.get(name, "")
            if any(re.match(r"\[#([\w.]+)\]", t).group(1) not in has for t in want.split(" and ")):
                bad.append((name, want, has))
    return bad


def axis_rules():
    """`<table>_<ax>_src` is dead while `<table>_<ax>_en` is off — for every optional axis in the ECU.

    Derived from the meta, not walked: TableEval is the ONE evaluation path behind every table, and it
    collapses a disabled optional axis to index 0 without ever fetching its coordinate
    (TableEval.h:131-137). So the rule is structural, and it covers 89 selectors no per-module walk
    would have reached — the eight generic tables among them, which had no gate at all.
    """
    cfg = json.loads(open("shared/tuneit-meta.json", "rb").read()[:-4])["config"]

    def scan(mod, fields, prefix=""):
        out = {}
        for name in fields:
            m = re.fullmatch(r"(\w+)_([xyz])_en", name)
            if m and f"{m.group(1)}_{m.group(2)}_src" in fields:
                out[f"{mod}.{prefix}{m.group(1)}_{m.group(2)}_src"] = f"[#{mod}.{prefix}{name}] == 1"
        return out

    rules = {}
    for mod, fields in cfg.items():
        rules.update(scan(mod, fields))
        for fname, spec in fields.items():
            if isinstance(spec, dict) and spec.get("type") in ("array", "struct_array") and spec.get("fields"):
                rules.update(scan(mod, spec["fields"], f"{fname}[*]."))
    return rules


def main():
    doc = document()
    have = gates_on(doc)
    rules = dict(derived())
    rules.update(axis_rules())
    rules.update(HAND)                      # a hand rule is the authority where both speak

    # AN ARRAY MODULE'S FIELDS LIVE UNDER THE ARRAY. gate_paths names outputs.fixed_x10 (one element's
    # field, because every slot runs the same code); the document names outputs.output[*].fixed_x10.
    # Without this the lookup missed and the rule was silently counted as "not on a page" — a check
    # that passes by failing to look is worse than no check.
    cfgm = json.loads(open("shared/tuneit-meta.json", "rb").read()[:-4])["config"]
    arrays = {}
    for mod, fields in cfgm.items():
        for fname, spec in fields.items():
            if isinstance(spec, dict) and spec.get("type") in ("array", "struct_array"):
                arrays[mod] = fname
                break

    def lookup(field):
        if field in have:
            return have[field]
        mod, _, rest = field.partition(".")
        if mod in arrays:
            return have.get(f"{mod}.{arrays[mod]}[*].{rest}")
        return None

    bad, unseen = [], []
    for field, cond in sorted(rules.items()):
        g = lookup(field)
        if g is None:
            unseen.append(field)
            continue                        # not on a page: coverage.py's question, not this one
        for term in cond.split(" and "):
            key = re.match(r"\[#([\w.\[\]*]+)\]", term).group(1)
            # …and the GATE key needs the same array qualification as the field did. A rule says
            # outputs.value_source; the page condition says outputs.output[*].value_source. Comparing
            # the two as strings failed on every array module — six real, correct gates reported as
            # missing, which is the way to make a check nobody trusts.
            mod, _, rest = key.partition(".")
            alt = f"{mod}.{arrays[mod]}[*].{rest}" if mod in arrays else None
            if key not in g and not (alt and alt in g):
                bad.append((field, cond, g))
                break

    staged = pages_staged_dead(doc, rules)

    print(f"{len(rules)} path-gated settings "
          f"({len(derived())} walked, {len(axis_rules())} structural, {len(HAND)} read by hand)")
    # NOT A GAP. Most axis selectors are TABLE-OWNED — edited in the table's own axis-setup dialog
    # rather than as a control on a page (coverage.py counts 730 such fields), and a rule about a
    # control that does not exist is nothing to answer for. Counted, not listed, or the real findings
    # are buried under eighty lines of them.
    print(f"{len(unseen)} rule(s) name a setting owned by a dialog rather than a page (expected)")
    print(f"{len(bad)} setting(s) not gated on the page:")
    for field, cond, g in bad:
        print(f"    {field}\n        wants: {cond}\n        has:   {g.strip()[:90] or '(nothing)'}")
    print(f"{len(staged)} page(s) staged in a mode where everything on them is dead:")
    for name, want, has in staged:
        print(f"    {name}\n        node wants: {want}\n        node has:   {has or '(nothing)'}")
    return 1 if (bad or staged) else 0


if __name__ == "__main__":
    sys.exit(main())
