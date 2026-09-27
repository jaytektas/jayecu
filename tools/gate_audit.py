#!/usr/bin/env python3
"""Which settings does the firmware stop reading in some MODE, and does the page say so?

A control that still takes a value nobody consults lies about having an effect. The firmware is the
only authority on that — a field is dead when the code reading it sits behind a condition on ANOTHER
field — and the installed document is the only authority on whether the control says so.

A MODE GATE, and not merely a threshold. `if (cfg_->enable_ltt)` and `if (cfg_->pulse_mode == 0)`
decide whether a setting is consulted at all; `if (over >= cfg_->multi_cyl_for_p0300)` and
`if (rpm >= cfg_->min_rpm)` compare a setting against something happening in the engine, and the field
is live either way. So a guard counts only when every operand of the condition is a config field or a
literal — which is exactly the difference between "this mode does not use that" and "that has not
happened yet".

    python3 tools/gate_audit.py            # the fields whose page gate does not match the firmware
    python3 tools/gate_audit.py --all      # every conditional read, gated or not
"""
import argparse, collections, json, os, re, sys

FW  = "firmware/Engine/Modules"
DOC = os.path.expanduser("~/.local/share/jayecu/jayecu Studio/ecus")


def snake(n):
    return re.sub(r"(?<!^)(?=[A-Z])", "_", n).lower()


def document():
    for d in sorted(os.listdir(DOC)):
        p = os.path.join(DOC, d, "dashboard.gui")
        if os.path.exists(p) and not d.startswith(("tsimport", "offline")):
            return json.load(open(p))
    raise SystemExit("no native ECU document found")


def is_pure(expr):
    """Is every term of this condition a setting or a literal? Only then can its `else` be negated:
    `!(A && runtime)` is not `!A`, so an else-if chain over a live value gates nothing."""
    return bool(expr) and "||" not in expr and all(
        ("cfg_->" in t and mode_terms(t)) for t in expr.split("&&") if t.strip())


def mode_terms(expr):
    """The parts of a condition that decide a MODE, rather than compare a setting to something live.

    Split on `&&` first: `if (ltt_ && cfg_->enable_ltt)` is a runtime term AND a mode term, and the
    mode half still means the field below is dead when the switch is off. An `||` is not split — either
    side alone can make it true, so neither is a gate on its own.
    """
    if "||" in expr:
        return []
    # NOR THE NEGATION OF A CONJUNCTION. `!(cfg_->x && engine_state == CRANKING)` is true whenever the
    # engine is not cranking, whatever x says — so splitting it on && and keeping the config half reads
    # as a gate that is not there, and greys a live control. That is the worse direction of this fault:
    # a control the page says is dead. Expanding local guards is what made these reachable at all.
    if re.search(r"!\s*\(", expr) and "&&" in expr:
        return []
    out = []
    for term in expr.split("&&"):
        if "cfg_->" not in term:
            continue
        rest = re.sub(r"cfg_->\w+", " ", term)
        rest = re.sub(r"\b\d+\b|\b0x[0-9a-fA-F]+\b", " ", rest)
        rest = re.sub(r"[()!=<>&|+\-*/%,\s]", "", rest)
        if rest == "":
            out.append(term.strip().strip("()").strip())
    return out


def resolve_locals(path):
    """{local bool name: the expression behind it}, for guards written through a variable.

    `const bool closed = (cfg_->mode != 0) && idle_active && dt_s > 0.0f;` followed by `if (closed)`
    is a mode gate, and a walker that only looks at the `if` sees a name with no `cfg_->` in it and
    calls the block ungated. That is how three idle gain TABLES came to stage in open loop, where the
    firmware does not evaluate them at all. One level of indirection is enough — a guard built from
    another guard is rare, and a chain nobody can follow is one nobody should be writing.
    """
    out = {}
    # WHOLE STATEMENTS, not lines. These guards are long — they are a list of everything that has to be
    # true — so they wrap, and a line-at-a-time reader sees `const bool closed = (cfg_->mode != 0) &&`
    # with no semicolon and gives up. Idle's closed-loop guard is exactly that shape, which is why its
    # three gain tables went on reading as ungated after the read was moved inside the block.
    src = re.sub(r"//.*", "", open(path, encoding="utf-8").read())
    for m in re.finditer(r"\b(?:const\s+)?bool\s+(\w+)\s*=\s*([^;]+);", src, re.S):
        expr = " ".join(m.group(2).split())
        if "cfg_->" in expr:
            out[m.group(1)] = expr
    return out


def guards_in(path):
    """{field: {conditions it is read behind}} for one module source file.

    A brace walker, not a parser. EVERY `if` block goes on the stack, mode or not — a frame with no
    terms still holds a depth, and without it an inner `} else {` pops the enclosing frame and hangs
    its negation on fields that had nothing to do with it (four stacked `!` on a cruise field was how
    that showed up). The `} else {` line nets zero braces, so it is handled explicitly: it leaves its
    if-block and re-enters as that block's negation, which is the only way `pulse_width_us` reads as
    "when pulse_mode is NOT 0" rather than the exact opposite.
    """
    out = collections.defaultdict(set)
    stack, depth = [], 0
    locals_ = resolve_locals(path)

    def reads(code):
        """Every config thing this line touches. A scalar is `cfg_->name`; a TABLE is
        `<name>_table_desc(cfg_)` — the cells never appear as a cfg_-> name at all, which is why a
        scalar-only audit reported zero gaps while whole pages of tables staged in the wrong mode."""
        return re.findall(r"cfg_->(\w+)", code) + re.findall(r"\b(\w+)_desc\s*\(\s*cfg_", code)

    def expand(cond):
        """A condition with the local bools in it replaced by what they were built from."""
        if not cond:
            return cond
        for name, expr in locals_.items():
            cond = re.sub(rf"(?<![\w>]){re.escape(name)}(?![\w(])", f"({expr})", cond)
        return cond

    def pop_to(d):
        gone = None
        while stack and d <= stack[-1][0]:
            gone = stack.pop()
        return gone

    def negate(terms):
        return [t[2:-1] if t.startswith("!(") and t.endswith(")") else f"!({t})" for t in terms]

    for ln in open(path, encoding="utf-8"):
        code = re.sub(r"//.*", "", ln)
        opens, closes = code.count("{"), code.count("}")

        # Reads on this line belong to the scope as it stands BEFORE the line's own braces move it.
        if not re.match(r"\s*\}\s*else\b", code):
            for f in reads(code):
                for _d, cs, fs, _p in stack:
                    if cs and f not in fs:
                        out[f] |= set(cs)

        if re.match(r"\s*\}\s*else\b", code):
            depth -= 1
            gone = pop_to(depth)
            m = re.search(r"else\s+if\s*\((.*?)\)\s*\{?\s*$", code)
            cond   = expand(m.group(1)) if m else None
            terms  = mode_terms(cond) if cond else []
            tested = set(re.findall(r"cfg_->(\w+)", cond)) if cond else set()
            # ONLY A PURE CONDITION CAN BE NEGATED. `} else if (cfg_->x != 0 && age > limit)` is reached
            # when EITHER half was false, so "not x" is not what got us here — and a gate written from
            # it would grey a field that is live.
            if gone and gone[3]:
                terms  = negate(gone[1]) + terms
                tested = tested | gone[2]
            stack.append((depth, terms, tested, is_pure(cond)))
            depth += 1
            # …and the reads on THIS line belong to the branch it just opened, not the one it left.
            for f in reads(code):
                for _d, cs, fs, _p in stack:
                    if cs and f not in fs:
                        out[f] |= set(cs)
            continue

        m = re.search(r"\b(if|while)\s*\((.*?)\)\s*\{?\s*$", code)
        if m and opens > closes:
            cond = expand(m.group(2))
            stack.append((depth, mode_terms(cond), set(re.findall(r"cfg_->(\w+)", cond)), is_pure(cond)))
        depth += opens - closes
        pop_to(depth)
    return out


def node_gates(doc):
    """{page path: every tree condition on it and its ancestors, joined}.

    A CONTROL IS ALSO GATED BY THE NODE THAT REACHES IT. A page whose tree node is hidden while the
    feature is off cannot be opened at all, which is a stronger gate than greying a widget — and
    reading only widget conditions reported those pages as wide open. MAP Prediction is the case in
    point: its node carries the enable and not one widget on it does.
    """
    out = {}
    def walk(n, path="", chain=()):
        q = f"{path}/{n['name']}" if path else n["name"]
        c = chain + ((n.get("condition") or ""),)
        out[q] = " ".join(x for x in c if x)
        for k in n.get("children") or []:
            walk(k, q, c)
    for n in doc["tree"]:
        walk(n)
    return out


def gates(doc):
    """{'<module>.<field>': the strongest enable/visibility condition reaching that control}."""
    nodes = node_gates(doc)
    found = {}
    for name, pg in doc["panelLibrary"].items():
        if not isinstance(pg, dict):
            continue
        outer = nodes.get(name, "")
        for w in pg.get("widgets", []):
            for c in [w] + json.loads(w.get("props", {}).get("children", "[]") or "[]"):
                cp = c.get("props", {})
                path = cp.get("signalName", "")
                if "." not in path:
                    continue
                g = " ".join(x for x in (outer,
                                         cp.get("enableCondition", ""),
                                         cp.get("condition", "")) if x).strip()
                if path not in found or len(g) > len(found[path]):
                    found[path] = g
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true", help="list every conditional read, matched or not")
    a = ap.parse_args()

    doc, g = document(), None
    g = gates(doc)
    bad = 0
    for src in sorted(os.listdir(FW)):
        if not src.endswith(".cpp"):
            continue
        mod = snake(src[:-4])
        for field, conds in sorted(guards_in(os.path.join(FW, src)).items()):
            if field == "enabled":
                continue
            # READ IN BOTH BRANCHES IS NOT GATED. stepper.dir_invert is read inside `if (stepdir)` and
            # again in the H-bridge path below it, so the union of its conditions holds both `x` and
            # `!(x)` — and demanding x in the page gate would grey a control that is live in EVERY
            # mode. That is the direction of this fault that actually hurts: a page saying a working
            # setting is dead. Found by reading Stepper.cpp, not by running this.
            norm = {c.strip() for c in conds}
            if any((f"!({c})" in norm or c.startswith("!(") and c[2:-1] in norm) for c in norm):
                continue
            path = f"{mod}.{field}"
            if path not in g:
                continue                       # not on a page at all — that is coverage.py's question
            gate = g[path]
            # Every field the firmware tests should appear in the control's own condition.
            needed = sorted({x for c in conds for x in re.findall(r"cfg_->(\w+)", c)} - {field, "enabled"})
            missing = [x for x in needed if f"{mod}.{x}" not in gate]
            if missing or a.all:
                mark = "GAP " if missing else "ok  "
                bad += 1 if missing else 0
                print(f"  {mark}{path}")
                for c in sorted(conds):
                    print(f"        read only when: {c}")
                print(f"        page gate: {gate or '(none)'}")
    print(f"\n{bad} control(s) that stay live while the firmware has stopped reading them")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
