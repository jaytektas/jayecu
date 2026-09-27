#!/usr/bin/env python3
"""WHICH SETTINGS A MODE ACTUALLY TOUCHES — by walking the code path, not by matching guards.

gate_audit.py asks "what condition sits above this read". That is the question backwards, and it is
why it needed a rule for pure conditions, another for `else`, another for local bools, another for a
field read in both branches — each one a heuristic, each one wrong somewhere.

This asks the question the tuner asks. Bind the gate (idle.mode = Open Loop), walk update() with that
binding, and collect every config field the walk can reach. A field reached on that path is live. A
field reached on NO path for that value is dead, and the page should say so.

THE WALK IS CONSERVATIVE, which is the only safe direction. A condition it cannot decide — anything
involving rpm, a bus channel, a timer — is taken BOTH ways, so everything under it counts as reached.
The tool therefore under-reports (it will call a dead field live) and never over-reports (it will not
call a live field dead). Greying a working control is the fault that actually hurts a tuner, so the
uncertainty is spent in the other direction.

    python3 tools/gate_paths.py                      # every module with a mode-like gate
    python3 tools/gate_paths.py idle                 # one module
    python3 tools/gate_paths.py idle --gate mode     # one gate of it
"""
import argparse, itertools, json, os, re, sys

FW   = "firmware/Engine/Modules"
META = "shared/tuneit-meta.json"

# MODULES WHOSE BRANCHES LIVE SOMEWHERE ELSE, and under a different name for the config.
#
# Engine/Modules is a convention, not a rule: the output slots are realised by OutputManager and the
# sensor pipelines by Sensors, both outside it, and neither says `cfg_->`. The outputs loop aliases one
# element as `o` and reads `o.value_source`, so a walker looking only for `cfg_->` in one directory saw
# a module with 25 fields per slot across 42 slots and nothing to say about any of them.
EXTRA_SOURCES = {
    "outputs": (["firmware/Integration/OutputManager.cpp"], r"\bo\.(\w+)"),
    "sensors": (["firmware/Sensors/Sensors.cpp"],           r"\bs\.(\w+)"),
    # …and the modules whose config is reached by NAME from wherever needs it, rather than through a
    # module class. There is no `cfg_->` for these — the engine definition is read by the composer, the
    # ignition and fuel modules and the position HAL, all as `g_config.engine.<field>`.
    # The engine definition is read under THREE names — g_config.engine.x by the composer and the
    # modules, and cyl_cfg_.x by the scheduler, which is where ign_mode, injector_timing_method and
    # num_inj_stages actually branch. One pattern would have seen two thirds of it.
    "engine":  (["firmware/Engine/SystemComposer.cpp", "firmware/Engine/Modules/Ignition.cpp",
                 "firmware/Engine/Modules/FuelCalculator.cpp", "firmware/Scheduler/EnginePositionHal.cpp",
                 "firmware/Scheduler/EventScheduler.cpp", "firmware/Scheduler/EventScheduler.h",
                 "firmware/Comms/CommsManager.cpp", "firmware/main.cpp"],
                r"(?:g_config\.engine|cyl_cfg_)\.(\w+)"),
    "datalog": (["firmware/Comms/CommsManager.cpp", "firmware/Comms/SdProtocol.cpp"],
                r"g_config\.datalog\.(\w+)"),
    "can":     (["firmware/main.cpp"], r"g_config\.can\.(\w+)"),
}


def snake(n):
    return re.sub(r"(?<!^)(?=[A-Z])", "_", n).lower()


def meta_config():
    return json.loads(open(META, "rb").read()[:-4])["config"]


def strip(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return "\n".join(re.sub(r"//.*", "", ln) for ln in src.splitlines())


def elem_expand(cond, elem_read):
    """`o.value_source == 3` reads as a config test, the same as `cfg_->value_source == 3`."""
    return re.sub(elem_read, lambda m: f"cfg_->{m.group(1)}", cond) if elem_read else cond


def locals_of(src):
    """{name: expression} for a local standing in for config, statements not lines.

    Two shapes, and both are how a mode gate is actually written:
      bool  closed = (cfg_->mode != 0) && ...     a compound guard, used as `if (closed)`
      uint8 model  = cfg_->fuel_model;            a plain alias, used as `if (model == 1)`
    The second was missed for being typed rather than bool, which is why the fuel model — four
    branches deciding which load the whole calculator runs on — read as gating nothing at all.
    """
    out = {}
    for m in re.finditer(r"\b(?:const\s+)?bool\s+(\w+)\s*=\s*([^;]+);", src, re.S):
        e = " ".join(m.group(2).split())
        if "cfg_->" in e:
            out[m.group(1)] = f"({e})"
    for m in re.finditer(r"\b(?:const\s+)?(?:u?int\d*_t|unsigned|int|auto)\s+(\w+)\s*=\s*"
                         r"(?:static_cast<[^>]+>\s*\(\s*)?(cfg_->\w+)\s*\)?\s*;", src):
        out.setdefault(m.group(1), m.group(2))
    return out


def expand(cond, locs):
    for _ in range(3):                      # a guard built from a guard; three is plenty
        new = cond
        for n, e in locs.items():
            new = re.sub(rf"(?<![\w>]){re.escape(n)}(?![\w(])", e, new)
        if new == cond:
            break
        cond = new
    return cond


def evaluate(cond, binding):
    """True / False / None(unknown) for a condition under `binding` = {field: int}.

    Only the shapes a mode gate is written in are decided: a field compared to a literal, a bare
    field as a truth value, `!`, `&&`, `||`. Everything else is unknown, and unknown means BOTH
    branches are walked.
    """
    c = cond.strip()
    while c.startswith("(") and c.endswith(")") and _balanced(c[1:-1]):
        c = c[1:-1].strip()

    for op, fn in ((" || ", any), (" && ", all)):
        parts = _split_top(c, op.strip())
        if len(parts) > 1:
            vals = [evaluate(p, binding) for p in parts]
            if op.strip() == "&&":
                if any(v is False for v in vals): return False
                return True if all(v is True for v in vals) else None
            if any(v is True for v in vals): return True
            return False if all(v is False for v in vals) else None

    if c.startswith("!"):
        v = evaluate(c[1:], binding)
        return None if v is None else (not v)

    m = re.fullmatch(r"cfg_->(\w+)\s*(==|!=|<|<=|>|>=)\s*(\d+)", c)
    if m and m.group(1) in binding:
        a, b = binding[m.group(1)], int(m.group(3))
        return {"==": a == b, "!=": a != b, "<": a < b, "<=": a <= b,
                ">": a > b, ">=": a >= b}[m.group(2)]
    m = re.fullmatch(r"cfg_->(\w+)", c)
    if m and m.group(1) in binding:
        return binding[m.group(1)] != 0
    return None


def _balanced(s):
    d = 0
    for ch in s:
        d += ch == "("
        d -= ch == ")"
        if d < 0: return False
    return d == 0


def _split_top(s, op):
    parts, d, cur, i = [], 0, "", 0
    while i < len(s):
        ch = s[i]
        if ch == "(": d += 1
        elif ch == ")": d -= 1
        if d == 0 and s.startswith(op, i):
            parts.append(cur); cur = ""; i += len(op); continue
        cur += ch; i += 1
    parts.append(cur)
    return [p for p in parts if p.strip()]


def reached(src, binding, elem_read=None):
    """Every config field the code can reach under `binding`.

    A brace walker with a skip: when a condition is decidably false the whole block is stepped over,
    and its `else` is entered instead. Reads are `cfg_->name` and `<table>_desc(cfg_)` — a table never
    appears as a cfg_-> name, which is how whole pages of maps escaped the guard-matching audit.
    """
    locs = locals_of(src)
    out, stack, depth, skip_from = set(), [], 0, None

    def note(code):
        if skip_from is not None:
            return
        out.update(re.findall(r"cfg_->(\w+)", code))
        out.update(re.findall(r"\b(\w+)_desc\s*\(\s*cfg_", code))
        if elem_read:
            out.update(re.findall(elem_read, code))

    for ln in src.splitlines():
        code = ln
        els = re.match(r"\s*\}\s*else\b", code)

        if els:
            depth -= 1
            frame = stack.pop() if stack and depth <= stack[-1][0] else None
            # <=, NOT <. skip_from is the depth the skipped `if` was opened AT, so closing that block
            # brings depth back to exactly it — and a strict < left the skip latched across the `else`,
            # which is the whole branch the binding says IS taken. It reported outputs.active_high dead
            # in Digital mode when OutputManager.cpp:335 reads it right there, and greying a live
            # control is the fault that costs a tuner something.
            if skip_from is not None and depth <= skip_from:
                skip_from = None
            # THE WHOLE CHAIN, not the previous link. In `if a {} else if b {} else if c {} else {}`
            # the final else is reached only when NONE of a, b, c held — so a frame carries whether
            # anything earlier in its chain was taken. Knowing only that the immediately preceding
            # branch was false left the last else undecidable, and outputs.primary_policy (read only
            # in that else) came back live in every value_source but one.
            won = frame[2] if frame else False
            m = re.search(r"else\s+if\s*\((.*)\)\s*\{?\s*$", code)
            if won:
                verdict = False                               # an earlier branch ran; this cannot
            elif m:
                c = evaluate(elem_expand(expand(m.group(1), locs), elem_read), binding)
                verdict = c if (frame is None or frame[1] is False) else (False if c is False else None)
            elif frame is None:
                verdict = None
            else:
                verdict = True if frame[1] is False else None
            stack.append((depth, verdict, won or verdict is True))
            if verdict is False and skip_from is None:
                skip_from = depth
            depth += 1
            note(code)
            continue

        note(code)
        m = re.search(r"\b(if|while)\s*\((.*)\)\s*\{\s*$", code)
        if m:
            verdict = evaluate(elem_expand(expand(m.group(2), locs), elem_read), binding)
            stack.append((depth, verdict, verdict is True))
            if verdict is False and skip_from is None:
                skip_from = depth
            depth += 1
            continue

        depth += code.count("{") - code.count("}")
        while stack and depth <= stack[-1][0]:
            stack.pop()
        if skip_from is not None and depth <= skip_from:
            skip_from = None
    return out


def gate_fields(mod, fields, src, elem_read=None):
    """Config fields worth binding: an enum or 0/1 flag the module actually branches on.

    An ARRAY module (outputs, sensors) is one element's fields — every slot runs the same code, so the
    gate is per-slot and the page is a template. Its fields live under the array, not beside it.
    """
    # AN ELEMENT ALIAS MEANS AN ARRAY; `g_config.<mod>.` DOES NOT. Both are "not cfg_->", but one is a
    # loop over slots that all run the same code and the other is a flat module read by name from
    # wherever needs it. Treating engine as an array put the walk inside cyl[]'s fields and it found
    # none of cycle_type, ign_mode or num_inj_stages.
    if elem_read and "g_config" not in elem_read:
        arr = next((v.get("fields") for v in fields.values()
                    if isinstance(v, dict) and v.get("type") in ("array", "struct_array")
                    and v.get("fields")), None)
        fields = arr or fields
    out = {}
    for name, spec in fields.items():
        if not isinstance(spec, dict) or spec.get("type") not in ("scalar", None):
            continue
        pat = elem_read.replace(r"(\w+)", re.escape(name)) if elem_read else rf"cfg_->{re.escape(name)}\b"
        if not re.search(pat, src):
            continue
        opts = spec.get("options")
        lo, hi = spec.get("min", 0), spec.get("max", 0)
        if opts:
            out[name] = (list(range(len(opts))), opts)
        elif (lo, hi) == (0, 1) and spec.get("kind") in ("bool", None) and name != "enabled":
            out[name] = ([0, 1], ["off", "on"])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("module", nargs="?")
    ap.add_argument("--gate")
    a = ap.parse_args()

    cfg = meta_config()
    srcs = {snake(f[:-4]): os.path.join(FW, f) for f in os.listdir(FW) if f.endswith(".cpp")}
    srcs.update({m: p for m, (p, _r) in EXTRA_SOURCES.items()
                 if any(os.path.exists(x) for x in p)})
    for mod in sorted(srcs):
        if a.module and mod != a.module:
            continue
        if mod not in cfg:
            continue
        paths = srcs[mod] if isinstance(srcs[mod], list) else [srcs[mod]]
        src = strip("\n".join(open(x, encoding="utf-8", errors="replace").read()
                              for x in paths if os.path.exists(x)))
        elem_read = EXTRA_SOURCES.get(mod, (None, None))[1]
        gates = gate_fields(mod, cfg[mod], src, elem_read)
        for g, (values, labels) in sorted(gates.items()):
            if a.gate and g != a.gate:
                continue
            per = {v: reached(src, {g: v}, elem_read) for v in values}
            everything = set().union(*per.values())
            # A SWITCH NEVER GATES ITSELF. A flag read only inside its own `if` is trivially absent
            # from the path where it is off, and a page that acted on that would grey the very control
            # you turn the feature back on with.
            dead = {v: sorted(everything - per[v] - {g}) for v in values}
            if not any(dead.values()):
                continue
            print(f"\n{mod}.{g}")
            for v in values:
                if dead[v]:
                    print(f"  when {g} = {v} ({labels[v]}) — {len(dead[v])} settings are never reached:")
                    for f in dead[v]:
                        print(f"        {f}")


if __name__ == "__main__":
    sys.exit(main())
