#!/usr/bin/env python3
"""What is different between the ECU's LIVE config and a tune image — by FIELD NAME.

"The studio says there's a tune change" is a true statement that names nothing. The studio compares
its image against the ECU's and reports a count; it cannot tell you that what moved was one ETB's
relax_pct, because the answer lives in the meta and not in the diff. This does that join.

  python3 tools/tune_diff.py                          # vs the newest studio restore snapshot
  python3 tools/tune_diff.py <image.tune>             # vs a specific RAW image (config_size bytes)
  python3 tools/tune_diff.py --watch [secs]           # re-read live and report writes as they happen

--watch is the one that answers "it went dirty on its own": it holds a baseline and names whatever
changes it, so a module quietly rewriting the tune is caught in the act rather than inferred.
"""
import sys, glob, os, time
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from tools.ts_bench import TsLink

RESTORE = os.path.expanduser(
    "~/.local/share/jayecu/jayecu Studio/ecus/004200313138510939323531/restore")


def spans(M):
    """(offset, size, name) for every addressable thing in the meta, so a byte can be named."""
    out = []
    for mod, fields in M.config.items():
        for name, d in fields.items():
            if not isinstance(d, dict):
                continue
            t = d.get("type")
            if t == "scalar":
                out.append((d["offset"], d["size"], f"{mod}.{name}"))
            elif t == "table":
                out.append((d["offset"], d["size"], f"{mod}.{name}"))
            elif t == "struct_array":
                # Name the ELEMENT, not just the array: "sensors.sensor[18].enabled" is actionable
                # where "sensors.sensor[] (99 bytes)" is not.
                for i in range(d["count"]):
                    base = d["base_offset"] + i * d["stride"]
                    eid = (d.get("element_ids") or [None] * d["count"])[i] or str(i)
                    for fn, fd in d["fields"].items():
                        out.append((base + fd["rel_offset"], fd.get("size", 1),
                                    f"{mod}.{name}[{eid}].{fn}"))
                    for tn, td in (d.get("tables") or {}).items():
                        if "base_offset" in td:
                            out.append((td["base_offset"] + i * td["stride"], td.get("size", 0),
                                        f"{mod}.{name}[{eid}].{tn}"))
            for tn, td in (d.get("tables") or {}).items():
                if "base_offset" in td and t != "struct_array":
                    out.append((td["base_offset"], td.get("size", 0), f"{mod}.{name}.{tn}"))
    out.sort()
    return out


def namer(sp):
    def who(off):
        hit = [n for (o, s, n) in sp if o <= off < o + max(s, 1)]
        return hit[-1] if hit else f"<unmapped @{off}>"
    return who


def report(a, b, who, label_a, label_b):
    d = [i for i in range(min(len(a), len(b))) if a[i] != b[i]]
    if not d:
        print(f"  identical ({label_a} vs {label_b})")
        return 0
    named = {}
    for i in d:
        n = who(i)
        named.setdefault(n, []).append(i)
    print(f"  {len(d)} bytes differ across {len(named)} field(s)   [{label_a} -> {label_b}]")
    for n, idxs in sorted(named.items(), key=lambda kv: -len(kv[1]))[:30]:
        print(f"    {len(idxs):5} B  {n}")
    if len(named) > 30:
        print(f"    … and {len(named)-30} more")
    return len(d)


def main():
    args = [a for a in sys.argv[1:]]
    L = TsLink(verbose=False)
    M = L.meta
    who = namer(spans(M))
    size = M.config_size

    def live():
        return L.read_config_chunked(0, size, chunk=200)

    if args and args[0] == "--watch":
        secs = float(args[1]) if len(args) > 1 else 120.0
        base = live()
        print(f"baseline read ({len(base)} B). Watching {secs:.0f}s — do the thing that dirties it.\n")
        t0 = time.time()
        while time.time() - t0 < secs:
            time.sleep(3.0)
            cur = live()
            if cur != base:
                print(f"[{time.strftime('%H:%M:%S')}] CONFIG CHANGED:")
                report(base, cur, who, "baseline", "now")
                base = cur
        print("\nwatch ended")
        return 0

    if args:
        path = args[0]
    else:
        fs = sorted(glob.glob(RESTORE + "/*.tune"), key=os.path.getmtime)
        if not fs:
            print("no restore snapshots found; pass an image explicitly")
            return 2
        path = fs[-1]
    img = open(path, "rb").read()
    if len(img) != size:
        print(f"{os.path.basename(path)} is {len(img)} B; this layout is {size} B — not a raw image "
              f"for this firmware (a .tune from tunes/ is a DICTIONARY, not an image)")
        return 2
    print(f"ECU live  vs  {os.path.basename(path)}")
    report(img, live(), who, os.path.basename(path), "ECU live")
    return 0


if __name__ == "__main__":
    sys.exit(main())
