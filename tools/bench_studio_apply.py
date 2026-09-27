#!/usr/bin/env python3
"""Does the ECU decode what the STUDIO writes?

The library, the model tests and the bench sweep each check a different link. None of them checked
the one that matters to a user: the studio takes a library wheel through the designer, emits config
pairs, and those bytes go to the ECU. This applies exactly those pairs — produced by the studio's own
code via tests/wheel_emit — and asks a spinning ECU whether it syncs.

    python3 -m tools.bench_studio_apply
"""
import struct
import subprocess
import sys
import time
from pathlib import Path

from tools.ts_bench import TsLink
from tools.gen_rebench import open_stim, select, STREAM0, STREAM_STRIDE, F, CELL_REL, CELL_STRIDE
from tools.bench_fuel import set_fixed_rpm

ROOT = Path(__file__).resolve().parents[1]
EMIT = ROOT / "apps/studio-jf/build/wheel_emit"
META = ROOT / "shared/tuneit-meta.json"

# wheel -> (stim index, the sync level it must reach). These are the three whose angular period is
# not the slot default, i.e. the ones that carry `repeats` and would silently decode as a different
# wheel if the studio dropped it.
CASES = [
    ("Renix 4cyl symmetrical",   58, 2),
    ("Renix 6cyl symmetrical",   59, 2),
    ("Daihatsu 3+1 distributor", 46, 2),
]

# Bench wiring: the rig drives crank-rate teeth on pool index 2 and the cam on DIG3 (index 4). The
# library says nothing about pins, so the harness supplies them — and a distributor's single pickup
# comes out of the rig's crank output whichever slot it occupies.
CRANK_CAP, CAM_CAP = 2, 4
fails = []


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}: {detail}")
    if not cond:
        fails.append(name)


def studio_pairs(name):
    out = subprocess.run([str(EMIT), str(META), name], capture_output=True, text=True, check=True)
    pairs = {}
    for line in out.stdout.splitlines():
        k, _, v = line.partition("=")
        pairs[k] = float(v)
    return pairs


def apply_pairs(l, pairs):
    """Write the studio's pairs into the ECU's config image."""
    sizes = {"slots": ("<H", 2), "nominal_angle": ("<h", 2),
             "width_min": ("<h", 2), "width_max": ("<h", 2), "width_target": ("<h", 2)}
    # Clear `repeats` as well as `enabled`, or a slot keeps the value the LAST wheel left there and
    # the apply is judged against config it did not write. Found by the negative control below: with
    # repeats stripped from the pairs, all three wheels still synced — on the previous run's leftovers.
    for s in range(6):
        l.write_raw(STREAM0 + STREAM_STRIDE*s + F["enabled"], bytes([0]))
        l.write_raw(STREAM0 + STREAM_STRIDE*s + F["repeats"], bytes([0]))
    for key, val in pairs.items():
        if not key.startswith("trigger.streams["):
            continue
        i = int(key.split("[")[1].split("]")[0])
        # "streams[i].cell[k].v" — the stream's OWN cell, addressed inside its element. The studio used
        # to emit "trigger.cell_pool[j].v" against one shared pool; a private array means the wheel's
        # cells travel with the slot they describe and no cursor has to agree across streams.
        if ".cell[" in key:
            k = int(key.split(".cell[")[1].split("]")[0])
            l.write_raw(STREAM0 + STREAM_STRIDE*i + CELL_REL + CELL_STRIDE*k,
                        struct.pack("<h", int(round(val))))
            continue
        fld = key.rsplit(".", 1)[1]
        if fld not in F:
            continue
        off = STREAM0 + STREAM_STRIDE*i + F[fld]
        fmt, _n = sizes.get(fld, (None, 1))
        l.write_raw(off, struct.pack(fmt, int(round(val))) if fmt else bytes([int(round(val)) & 0xFF]))
    # Pins are bench wiring, not wheel geometry — see above.
    for s in range(6):
        b = STREAM0 + STREAM_STRIDE*s
        if pairs.get(f"trigger.streams[{s}].enabled", 0) < 1:
            continue
        crank_like = any(pairs.get(f"trigger.streams[{k}].enabled", 0) >= 1 for k in (0, 1))
        cap = CRANK_CAP if (s < 2 or not crank_like) else CAM_CAP
        l.write_raw(b + F["capture_index"], bytes([cap]))
    l.cmd(b"E", b"reconfig")


def main():
    if not EMIT.exists():
        print(f"  build it first: cmake --build apps/studio-jf/build --target wheel_emit")
        return 2
    print("=== the ECU decodes what the studio writes ===")
    sp = open_stim()
    for name, stim, want in CASES:
        pairs = studio_pairs(name)
        rep = {k: v for k, v in pairs.items() if k.endswith(".repeats") and v}
        set_fixed_rpm(sp, 900)
        select(sp, stim, reps=4)
        l = TsLink(); apply_pairs(l, pairs); time.sleep(4.5)
        lvl = l.telem_all()["sync_level"]
        l.close()
        check(f"{name}", lvl >= want, f"sync={lvl} (want {want})  repeats={rep or 'slot default'}")
    # NEGATIVE CONTROL — and it FAILED, which is the useful part.
    #
    # With `repeats` stripped these wheels STILL reach PHASE. GapMatcher identifies a gap by the
    # RATIO between intervals, which is scale-invariant, so it locks just as readily on a 40 deg
    # assumed pitch when the teeth are 20 deg apart. It syncs, confidently, to the wrong angle.
    #
    # So sync_level cannot judge this. The oracle that can is the one bench_library_sweep uses:
    # capture a cycle and count the crank edges against what the rig's geometry predicts — that is
    # what caught the same fault before (crank=8 where 16 was due). This check therefore proves the
    # studio's bytes REACH the ECU and are accepted; it does NOT prove the angle. Reported rather
    # than asserted, because a red line here would be measuring the wrong thing loudly.
    print("\n  --- sync-level is a WEAK oracle: these still 'pass' with repeats stripped ---")
    for name, stim, want in CASES:
        stripped = {k: v for k, v in studio_pairs(name).items() if not k.endswith(".repeats")}
        if not any(k.endswith(".repeats") for k in studio_pairs(name)):
            continue
        set_fixed_rpm(sp2 := open_stim(), 900); select(sp2, stim, reps=4)
        l = TsLink(); apply_pairs(l, stripped); time.sleep(4.5)
        lvl = l.telem_all()["sync_level"]; l.close(); sp2.close()
        print(f"      {name:28s} sync={lvl} without repeats "
              f"({'unchanged — sync cannot see the error' if lvl >= want else 'differs'})")

    print(f"\n  {'FAILED: ' + ', '.join(fails) if fails else 'the studio\'s bytes reach the ECU and are accepted (angle: see bench_library_sweep)'}")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
