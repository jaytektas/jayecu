#!/usr/bin/env python3
"""The ODD-CYLINDER DISTRIBUTOR: a cam-period stream, no crank, and it must reach PHASE.

Kept out of the library sweep because that harness cannot judge this wheel: its expectation trusts
the stim registry's declared span, which for the Daihatsu says 360 where the array's own comments
and entry count say 720, and its lane accounting assumes a crank stream where a distributor has
none. This checks the thing that actually matters.

    python3 -m tools.bench_distributor
"""
import struct
import sys
import time

from tools.ts_bench import TsLink
from tools.gen_rebench import open_stim, select, STREAM0, STREAM_STRIDE, F, CELL_REL, CELL_STRIDE
from tools.bench_fuel import set_fixed_rpm

fails = []


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}: {detail}")
    if not cond:
        fails.append(name)


def configure(l, repeats, cell):
    for s in range(6):
        l.write_raw(STREAM0 + STREAM_STRIDE*s + F['enabled'], bytes([0]))
    b = STREAM0 + STREAM_STRIDE*2          # a cam-period slot: slots 0 and 1 stay EMPTY
    for f, v in (('enabled', 1), ('capture_index', 2), ('edge', 0), ('primitive', 1),
                 ('cell_len', len(cell)), ('window_pct', 25),
                 ('repeats', repeats)):
        l.write_raw(b + F[f], bytes([v]))
    for i, c in enumerate(cell):                    # this stream's OWN cells, from 0
        l.write_raw(b + CELL_REL + CELL_STRIDE*i, struct.pack('<h', c))
    l.cmd(b"E", b"reconfig")


def main():
    print("=== Daihatsu 3+1: 3 cylinders, no crank sensor ===")
    sp = open_stim()
    set_fixed_rpm(sp, 900)
    select(sp, 46, reps=4)

    # Three primaries 240 deg apart over the 720 cycle plus a "+1" index 30 deg after the first:
    # spans 30/210/240/240. Read off the rig's own table (144 entries at 5 deg).
    l = TsLink(); configure(l, 1, [300, 2100, 2400, 2400])
    ok, took, _ = l.wait_until(lambda f: f["sync_level"] == 2, timeout=4.5)
    lvl = l.telem_all()["sync_level"]
    print(f"    phase reached in {took:.2f} s" if ok else f"    no phase after {took:.2f} s")
    check("an odd-cylinder distributor reaches PHASE with NO crank stream", lvl == 2,
          f"sync_level={lvl}")
    l.close()

    # And the span really is 720: squeezing the same shape into 360 must NOT reach phase, which is
    # what says the array is right and the stim registry's 360 is the thing that is wrong.
    # NOT CONVERTIBLE. The claim is that phase is NOT reached, and you cannot poll for something
    # failing to happen — the wait itself is the evidence, and it is only worth anything if it is
    # comfortably longer than the time the SAME rig took to reach phase when it could (printed above,
    # well under a second: measured at 0.58 s, about four engine cycles). 2.0 s at 900 rpm is fifteen
    # cycles — more than three times what the working case needed — so a silence here means refusal
    # and not impatience.
    l = TsLink(); configure(l, 2, [150, 1050, 1200, 1200]); time.sleep(2.0)
    lvl2 = l.telem_all()["sync_level"]
    check("the same shape squeezed into 360 does NOT reach phase", lvl2 < 2, f"sync_level={lvl2}")
    l.close()
    sp.close()

    print(f"\n  {'FAILED: ' + ', '.join(fails) if fails else 'the distributor decodes on its own'}")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
