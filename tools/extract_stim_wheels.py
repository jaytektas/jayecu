#!/usr/bin/env python3
"""Derive trigger-wheel geometry from the bench stim's own pattern tables.

The stim stores each wheel as an array of angular samples over its full span — one entry per step,
each a bitmask of the output channels (bit 0 = crank/dig1, bit 1 = cam/dig2). That is the exact
geometry, in a file we hold, and it is the SAME definition the rig will spin when we go to verify.
So a library entry derived here can be checked by measurement rather than taken on trust, which is
not true of a tooth count read off a web page.

It prints, per wheel: the sample resolution, the crank tooth angles with their pitch pattern (and
what missing-tooth wheel that implies), and the cam edge angles over the 720 cycle.

    python3 tools/extract_stim_wheels.py [name-substring]
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INO  = ROOT / "tools/Ardu-Stim/ardustim/ardustim/ardustim.ino"
DEFS = ROOT / "tools/Ardu-Stim/ardustim/ardustim/wheel_defs.h"


def load_arrays():
    """symbol -> [int samples]. The tables are C arrays of small ints with /* */ comments."""
    text = DEFS.read_text(errors="replace")
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)          # strip block comments
    # ...and LINE comments, which is not tidiness: these tables annotate every row with its angle
    # range ("//540-600"), and a bare digit scan reads that as the samples 540 and -600. It inflated
    # the Daihatsu array from 144 entries to 175 and put its edges at angles the wheel does not have.
    text = re.sub(r"//[^\n]*", " ", text)
    out = {}
    for m in re.finditer(r"(\w+)\s*\[\s*\]\s*PROGMEM\s*=\s*\{(.*?)\}\s*;", text, re.S):
        vals = [int(v) for v in re.findall(r"-?\d+", m.group(2))]
        if vals:
            out[m.group(1)] = vals
    return out


def load_table():
    """The registry: (friendly name, array symbol, sample count, span degrees), in stim index order."""
    text = INO.read_text(errors="replace")
    body = text.split("Wheels[MAX_WHEELS] = {", 1)[1]
    rows = []
    for line in body.splitlines():
        # FOUR FIELDS: name, array, sample count, span. The row used to carry a hand-typed RPM scaler
        # between the array and the count — fourteen of them disagreed with their own inputs, so it is
        # derived at the point of use now and the row no longer states it. This regex still wanted the
        # five-field shape, matched nothing, and returned an EMPTY registry: every tool built on it
        # reported no wheels at all rather than failing.
        m = re.match(r"\s*\{\s*(\w+)_friendly_name\s*,\s*(\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}", line)
        if m:
            rows.append({"sym": m.group(2), "count": int(m.group(3)), "deg": int(m.group(4))})
        if line.strip().startswith("};"):
            break
    names = dict(re.findall(r"(\w+)_friendly_name\[\]\s*PROGMEM\s*=\s*\"([^\"]*)\"", DEFS.read_text(errors="replace")))
    for r in rows:
        r["name"] = names.get(r["sym"], r["sym"])
    return rows


def edges(samples, bit, deg_per):
    """Rising and falling edge angles for one channel, walking the ring (wrap included)."""
    rise, fall = [], []
    n = len(samples)
    for i in range(n):
        a = (samples[i - 1] >> bit) & 1
        b = (samples[i] >> bit) & 1
        if not a and b: rise.append(i * deg_per)
        elif a and not b: fall.append(i * deg_per)
    return rise, fall


def describe_gap_wheel(rise, period):
    """Given rising-edge angles within one period, work out slots / missing / gap positions."""
    if len(rise) < 3:
        return None
    within = sorted(a for a in rise if a < period - 1e-9)
    if len(within) < 3:
        return None
    gaps = [round(b - a, 3) for a, b in zip(within, within[1:])]
    gaps.append(round(within[0] + period - within[-1], 3))
    pitch = min(gaps)
    if pitch <= 0:
        return None
    slots = round(period / pitch)
    # Every spacing must be a whole number of pitches for this to be a missing-tooth wheel.
    mult = [g / pitch for g in gaps]
    if any(abs(m - round(m)) > 0.02 for m in mult):
        return None
    ratios = [round(m) for m in mult]
    missing_at = [i for i, r in enumerate(ratios) if r > 1]
    return {"slots": slots, "pitch": pitch, "present": len(within),
            "ratios": ratios, "missing_at": missing_at,
            "ratio": max(ratios) if missing_at else 0, "first": within[0]}


def main():
    want = sys.argv[1].lower() if len(sys.argv) > 1 else ""
    arrays, table = load_arrays(), load_table()
    for idx, r in enumerate(table):
        if want and want not in r["name"].lower():
            continue
        s = arrays.get(r["sym"])
        if not s:
            print(f"[{idx:3d}] {r['name']}  -- array {r['sym']} not found"); continue
        if len(s) != r["count"]:
            print(f"[{idx:3d}] {r['name']}  -- table says {r['count']} samples, array has {len(s)}")
        deg_per = r["deg"] / len(s)
        chans = max(s).bit_length()
        print(f"\n[{idx:3d}] {r['name']}")
        print(f"      span {r['deg']}deg  {len(s)} samples  ({deg_per:.4g} deg/sample)  channels={chans}")
        for bit in range(max(2, chans)):
            rise, fall = edges(s, bit, deg_per)
            if not rise and not fall:
                continue
            kind = "crank" if bit == 0 else f"cam{bit}"
            print(f"      {kind}: {len(rise)} rising, {len(fall)} falling")
            # A crank pattern on a 720 span usually repeats each revolution; report it per 360.
            period = 360.0 if (bit == 0 and r["deg"] == 720) else float(r["deg"])
            g = describe_gap_wheel(rise, period)
            if g:
                miss = f"{g['slots']}-{sum(x - 1 for x in g['ratios'] if x > 1)}" if g["missing_at"] else f"{g['slots']} even"
                print(f"        -> {miss}: slots={g['slots']} pitch={g['pitch']}deg present={g['present']}"
                      f" gaps_at={g['missing_at']} ratio={g['ratio']} first_tooth@{g['first']}deg")
            else:
                spans = [round(b - a, 1) for a, b in zip(rise, rise[1:])]
                if rise:
                    spans.append(round(rise[0] + float(r["deg"]) - rise[-1], 1))
                print(f"        -> not a uniform wheel; rising at {[round(a,1) for a in rise][:12]}")
                print(f"           inter-edge spans {spans[:12]}")


if __name__ == "__main__":
    main()
