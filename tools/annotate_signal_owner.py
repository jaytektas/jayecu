#!/usr/bin/env python3
"""Add `owner: firmware` to every signal the firmware references by identity — raw `SIG_<id>` OR a
`wk::<role>` target. Default (no `owner:`) = config-owned (user-editable). Line-preserving so the
compact flow style + comments survive. Idempotent. The studio locks ids where owner==firmware;
codegen validates this flag against real firmware usage so it can never silently drift.
"""
import re, yaml
from pathlib import Path

PATH = "definition/ecu.schema.yaml"
sch = yaml.safe_load(open(PATH))
sig_ids = [s["id"] for s in sch["signals"]]

# firmware-owned = referenced raw as SIG_<id>, or a well_known_signals role target.
blk = re.compile(r"/\*.*?\*/", re.S); tok = re.compile(r"\bSIG_[A-Z][A-Z0-9_]*")
used = set()
for p in Path("firmware").rglob("*"):
    if p.suffix not in (".h", ".hpp", ".cpp", ".cc", ".inc"):
        continue
    s = blk.sub(lambda m: "\n" * m.group(0).count("\n"), p.read_text(errors="replace"))
    for line in s.splitlines():
        for m in tok.finditer(line.split("//", 1)[0]):
            used.add(m.group(0))
owned = {i for i in sig_ids if f"SIG_{i.upper()}" in used}
owned |= set((sch.get("well_known_signals") or {}).values())

lines = open(PATH).readlines()
start = next(i for i, l in enumerate(lines) if l.rstrip("\n") == "signals:") + 1
end = next(i for i, l in enumerate(lines) if l.startswith("can_devices:"))

FLOW = re.compile(r"^-\s*\{\s*id:\s*([A-Za-z0-9_]+)")
BLOCK = re.compile(r"^-\s*id:\s*([A-Za-z0-9_]+)\s*$")
out, n = [], 0
for idx, line in enumerate(lines):
    if start <= idx < end:
        mf = FLOW.match(line)
        mb = BLOCK.match(line)
        if mf and mf.group(1) in owned and "owner:" not in line:
            depth = 0; close = None        # find the OUTER '}' (skip nested gauge/dtc braces, keep trailing # comment)
            for i, ch in enumerate(line):
                if ch == "{":
                    depth += 1
                elif ch == "}":
                    depth -= 1
                    if depth == 0:
                        close = i; break
            assert close is not None, line
            line = line[:close].rstrip().rstrip(",") + ", owner: firmware" + line[close:]
            n += 1
        elif mb and mb.group(1) in owned:
            out.append(line)
            out.append("  owner: firmware\n")
            n += 1
            continue
    out.append(line)

open(PATH, "w").writelines(out)
print(f"annotated {n} firmware-owned signals (of {len(owned)} owned, {len(sig_ids)} total)")
