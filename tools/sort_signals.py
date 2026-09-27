#!/usr/bin/env python3
"""Sort the top-level `signals:` block in definition/ecu.schema.yaml alphabetically by id.

Line-preserving: keeps each entry's attached leading comments with it, and pins the
trailer comment block (the "Pure-virtual channels" / "CAN device library" run that
physically precedes can_devices:) in place at the end of the block.
"""
import re
import sys

PATH = "definition/ecu.schema.yaml"

with open(PATH) as f:
    lines = f.readlines()

# Locate block boundaries: from the line after `^signals:` up to (excluding) `^can_devices:`.
start = next(i for i, l in enumerate(lines) if l.rstrip("\n") == "signals:") + 1
end = next(i for i, l in enumerate(lines) if l.startswith("can_devices:"))

head = lines[:start]
block = lines[start:end]
tail = lines[end:]

ID_RE = re.compile(r"^-\s*(?:\{\s*)?id:\s*([A-Za-z0-9_]+)")

items = []          # list of dicts: {id, preamble:[...], body:[...]}
pending = []        # comment/blank lines awaiting the next entry
cur = None

for line in block:
    if ID_RE.match(line):
        cur = {"id": ID_RE.match(line).group(1), "preamble": pending, "body": [line]}
        items.append(cur)
        pending = []
    elif line.strip() == "" or line.lstrip().startswith("#"):
        # blank or comment -> preamble of the NEXT entry (or trailer if none follows)
        pending.append(line)
    else:
        # indented continuation line of the current (block-style) entry
        cur["body"].append(line)

trailer = pending  # leftover comments after the last entry (none expected here)

# The misplaced trailer comment run (describes can_devices / lua gauges) currently sits
# as the preamble of whichever entry follows it. Detach it so it stays put before
# can_devices: instead of travelling with that entry when sorted.
for it in items:
    if any("CAN device library" in c for c in it["preamble"]):
        trailer = it["preamble"] + trailer
        it["preamble"] = []
        break

before = len(items)
items.sort(key=lambda it: it["id"])
assert len(items) == before, "lost an item"

# Reassemble.
out = list(head)
for it in items:
    out.extend(it["preamble"])
    out.extend(it["body"])
out.extend(trailer)
out.extend(tail)

with open(PATH, "w") as f:
    f.writelines(out)

print(f"sorted {before} signal entries; trailer lines pinned: {len(trailer)}")
print("first 5:", [it["id"] for it in items[:5]])
print("last 5: ", [it["id"] for it in items[-5:]])
