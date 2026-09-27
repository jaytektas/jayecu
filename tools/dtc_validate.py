#!/usr/bin/env python3
"""Live bench validation of the unified DTC stack over the TS link.

Reads the DTC telemetry the firmware packs each frame (dtc_active/dtc_stored/
dtc_worst_sev/dtc_worst_code/dtc_indicators) and decodes the per-category indicator
bitmask against the schema's dtc_indicators definitions. No engine/stim required —
on a bare bench the enabled-but-unfed sensors time out, which is exactly what should
light their category bits.
"""
import sys, pathlib, yaml
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from ts_bench import TsLink

ROOT = pathlib.Path(__file__).resolve().parent.parent
CATS = {int(c["bit"]): (c["id"], c.get("label", c["id"]))
        for c in (yaml.safe_load((ROOT / "definition" / "ecu.schema.yaml").read_text())
                  or {}).get("dtc_indicators", [])}

link = TsLink(verbose="-v" in sys.argv)
print("signature:", link.hello())

t = link.telem_all()
present = [k for k in ("dtc_active", "dtc_stored", "dtc_worst_sev", "dtc_worst_code",
                       "dtc_indicators", "monitor_flags", "prot_status") if k in t]
print("\nDTC telemetry fields present:", ", ".join(present))
for k in present:
    print(f"  {k:16} = {t[k]}")

mask = int(t.get("dtc_indicators", 0))
print(f"\ndtc_indicators = 0x{mask:08X}")
active = [(b, CATS.get(b, ('?', '?'))) for b in range(32) if mask & (1 << b)]
if not active:
    print("  (no category indicators lit)")
for bit, (cid, label) in active:
    print(f"  bit {bit:2}  {cid:14} {label}")

worst = int(t.get("dtc_worst_sev", 0))
print(f"\nworst severity = {worst}  ({'none' if worst==0 else ['','level 1','level 2','level 3'][worst]})"
      f"   active={t.get('dtc_active')}  stored={t.get('dtc_stored')}"
      f"   worst_code=P{int(t.get('dtc_worst_code',0)):04X}")
link.close()
