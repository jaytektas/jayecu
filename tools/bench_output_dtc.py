#!/usr/bin/env python3
"""P1653 / P1654 on the rig: a firing cylinder with no coil row, or no stage-1 injector row.

The studio lays the coil and injector rows out and the firmware fills nothing in, so a row removed by hand
is a cylinder that gets no spark or no fuel. This clears one coil row and one injector row on a known
4-cylinder wasted-spark map, applies it, and reads the DTCs back; then restores the rows and checks both
heal. No wheel is needed: the check is on the APPLIED map, which a stopped reconfigure applies.

  python3 -m tools.bench_output_dtc
"""
import struct, sys, time
from tools.ts_bench import TsLink
from tools.bench_cycle import configure_firing
from tools.output_rows import IGN_BASE, LS_BASE, FN_NONE

fails = []
def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}: {detail}")
    if not cond:
        fails.append(name)


def active_dtcs(l):
    _, img = l.cmd(b"G")
    n = struct.unpack_from("<H", img, 8)[0]
    out = set()
    for i in range(n):
        off = 10 + 36 * i
        if off + 36 > len(img):
            break
        code = struct.unpack_from("<H", img, off)[0]
        if code and img[off + 4] & 1:
            out.add(f"P{code:04X}")
    return out


def row_put(l, row, field, value):
    M = l.meta
    f = M.array("outputs", "output")["fields"][field]
    l.write_raw(M.array_offset("outputs", "output", row, field), value.to_bytes(f["size"], "little"))


def apply(l):
    l.execute("reconfig")
    time.sleep(1.5)


def main():
    print("=== Bench output-row DTCs (P1653 no coil, P1654 no injector) ===")
    l = TsLink()
    configure_firing(l)                       # 4-cyl 1-3-4-2, wasted-spark coils IGN1/IGN2, LS1-4
    apply(l)
    d = active_dtcs(l)
    check("full map: no P1653", "P1653" not in d, str(sorted(d)))
    check("full map: no P1654", "P1654" not in d, str(sorted(d)))

    row_put(l, IGN_BASE + 1, "function", FN_NONE)     # IGN2: cylinders 2 + 3 lose their coil
    apply(l)
    d = active_dtcs(l)
    check("IGN2 removed: P1653 active", "P1653" in d, str(sorted(d)))
    check("IGN2 removed: no P1654", "P1654" not in d, str(sorted(d)))

    row_put(l, LS_BASE + 3, "function", FN_NONE)      # LS4: cylinder 4 loses its injector
    apply(l)
    d = active_dtcs(l)
    check("LS4 removed too: P1654 active", "P1654" in d, str(sorted(d)))

    configure_firing(l)                                # the rows back
    apply(l)
    time.sleep(1.0)
    d = active_dtcs(l)
    check("restored: P1653 healed", "P1653" not in d, str(sorted(d)))
    check("restored: P1654 healed", "P1654" not in d, str(sorted(d)))
    l.close()
    print(f"\n=== {'FAILED: ' + ', '.join(fails) if fails else 'ALL PASS'} ===")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
