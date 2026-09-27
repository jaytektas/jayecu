#!/usr/bin/env python3
"""Sort the `sensors:` block in definition/ecu.schema.yaml alphabetically by id WITHIN each
section. Section dividers (`# --- ... ---` comment lines) and their order are preserved;
entry-attached prose comments travel with their entry; block-style entries are kept intact.
"""
import re

PATH = "definition/ecu.schema.yaml"

with open(PATH) as f:
    lines = f.readlines()

start = next(i for i, l in enumerate(lines) if l.rstrip("\n") == "sensors:") + 1
end = next(i for i, l in enumerate(lines) if l.rstrip("\n") == "signals:")

head, block, tail = lines[:start], lines[start:end], lines[end:]

ID_RE = re.compile(r"^-\s*(?:\{\s*)?id:\s*([A-Za-z0-9_]+)")
DIVIDER_RE = re.compile(r"^\s*#\s*---")   # section header lines

# 1) Tokenise the block into flat entries, each carrying its leading comment/blank preamble.
entries = []
pending = []
cur = None
for line in block:
    if ID_RE.match(line):
        cur = {"id": ID_RE.match(line).group(1), "preamble": pending, "body": [line]}
        entries.append(cur)
        pending = []
    elif line.strip() == "" or line.lstrip().startswith("#"):
        pending.append(line)          # comment/blank -> preamble of the NEXT entry
    else:
        cur["body"].append(line)      # indented continuation of a block-style entry
trailing = pending                    # comments/blanks after the last entry (none expected)

# 2) Group entries into sections. A divider run in an entry's preamble starts a new section;
#    the header (up to & including the last divider line) stays fixed, the rest stays attached.
sections = []
cur_sec = None
for e in entries:
    pre = e["preamble"]
    div_idx = max((i for i, l in enumerate(pre) if DIVIDER_RE.match(l)), default=None)
    if div_idx is not None:
        header = pre[: div_idx + 1]
        e["preamble"] = pre[div_idx + 1 :]
        cur_sec = {"header": header, "entries": []}
        sections.append(cur_sec)
    elif cur_sec is None:
        cur_sec = {"header": [], "entries": []}
        sections.append(cur_sec)
    cur_sec["entries"].append(e)

# 3) Sort each section's entries by id (stable) and reassemble.
out = list(head)
total = 0
for sec in sections:
    sec["entries"].sort(key=lambda e: e["id"])
    out.extend(sec["header"])
    for e in sec["entries"]:
        out.extend(e["preamble"])
        out.extend(e["body"])
        total += 1
out.extend(trailing)
out.extend(tail)

with open(PATH, "w") as f:
    f.writelines(out)

print(f"{len(sections)} sections, {total} sensor entries sorted within sections")
for sec in sections:
    hdr = next((l.strip() for l in sec["header"] if DIVIDER_RE.match(l)), "(no header)")
    print(f"  {hdr:50s} -> {[e['id'] for e in sec['entries']]}")
