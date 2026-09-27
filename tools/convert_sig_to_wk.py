#!/usr/bin/env python3
"""Convert every raw `SIG_<id>` signal reference in firmware to the rename-safe `wk::<role>`.
Reuses an existing semantic role when one binds the id (SIG_TPS_1 -> wk::tps); otherwise adds an
`id: id` role to well_known_signals:. Sentinels (SIG_NONE/SIG_COUNT/SIG_T_*) are left raw. Ensures
each touched firmware file includes well_known_signals.h. Idempotent.
"""
import re
from pathlib import Path
import yaml

SENTINELS = {"SIG_NONE", "SIG_COUNT", "SIG_T_FLOAT", "SIG_T_U32", "SIG_T_I32"}
SCHEMA = "definition/ecu.schema.yaml"
BLK = re.compile(r"/\*.*?\*/", re.S)
TOK = re.compile(r"\bSIG_[A-Z][A-Z0-9_]*\b")

sch = yaml.safe_load(open(SCHEMA))
wk = sch["well_known_signals"]                       # role -> id
rev = {}                                             # id -> existing role (first wins)
for role, sid in wk.items():
    rev.setdefault(sid, role)
sig_ids = {s["id"] for s in sch["signals"]}

fw_files = sorted(p for p in Path("firmware").rglob("*")
                  if p.suffix in (".h", ".hpp", ".cpp", ".cc", ".inc"))

# 1) discover SIG_<id> tokens actually used in firmware code (comment-stripped)
used = set()
for p in fw_files:
    s = BLK.sub(lambda m: "\n" * m.group(0).count("\n"), p.read_text())
    for line in s.splitlines():
        for m in TOK.finditer(line.split("//", 1)[0]):
            if m.group(0) not in SENTINELS:
                used.add(m.group(0))

# 2) SIG_<ID> -> wk::<role>, adding id:id roles as needed
sym_role, new_roles = {}, {}
for sym in sorted(used):
    sid = sym[4:].lower()
    if sid not in sig_ids:
        continue                                     # validator already guarantees none
    role = rev.get(sid, sid)
    sym_role[sym] = role
    if sid not in rev:
        new_roles[role] = sid

# 3) append new roles to well_known_signals: (line-preserving, after the last existing role)
if new_roles:
    lines = open(SCHEMA).readlines()
    start = next(i for i, l in enumerate(lines) if l.rstrip("\n") == "well_known_signals:")
    end = start + 1
    while end < len(lines) and (lines[end].startswith("  ") or lines[end].strip() == ""):
        end += 1
    block_end = max(i for i in range(start + 1, end) if lines[i].strip()) + 1
    add = [f"  {r}: {sid}\n" for r, sid in new_roles.items()]
    lines[block_end:block_end] = add
    open(SCHEMA, "w").writelines(lines)

# 4) rewrite firmware refs + ensure include
INC = '#include "well_known_signals.h"\n'
changed = 0
for p in fw_files:
    text = p.read_text()
    new = TOK.sub(lambda m: f"wk::{sym_role[m.group(0)]}" if m.group(0) in sym_role else m.group(0), text)
    if new == text:
        continue
    if "well_known_signals.h" not in new:            # inject after the first #include
        out, done = [], False
        for ln in new.splitlines(keepends=True):
            out.append(ln)
            if not done and ln.startswith("#include"):
                out.append('#include "well_known_signals.h"   // wk:: rename-safe signal roles\n')
                done = True
        new = "".join(out)
    p.write_text(new)
    changed += 1

print(f"added {len(new_roles)} roles, rewrote {len(sym_role)} distinct SIG_* in {changed} files")
print("new roles:", ", ".join(sorted(new_roles)) or "(none)")
