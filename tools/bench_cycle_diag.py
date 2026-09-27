#!/usr/bin/env python3
"""Capture real engine-cycle frames and judge them with the SHIPPED studio code.

This does not re-implement the check. It writes the raw 0x26 page bytes to a file and hands them to
apps/studio-jf/build/cycle_diag_dump, which runs cyclewire::decode() and enginecycle::diagnose() —
the same two functions the studio calls when it draws a frame. A second implementation here could
agree with the first and both be wrong.

The rig covers what a user actually does to an engine: run it, stop it, start it again. The restart
is the important one — a capture can be exact for hours mid-run and still split the cycle the moment
sync is re-acquired, which is exactly the bug this exists to catch.

    python3 -m tools.bench_cycle_diag [rpm]
"""
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from ts_bench import TsLink                                    # noqa: E402
from gen_rebench import open_stim, select, configure, WHEELS, stim_halt, stim_resume   # noqa: E402
from bench_fuel import set_fixed_rpm, wait_for_sync            # noqa: E402
from bench_cycle import configure_firing                       # noqa: E402

ROOT  = Path(__file__).resolve().parent.parent
DUMP  = ROOT / "apps" / "studio-jf" / "build" / "cycle_diag_dump"
WHEEL = 4          # 60-2 + cam — reaches PHASE, so the cycle is the full 720


def raw_frame(link: TsLink) -> bytes:
    """Every page of one capture, concatenated exactly as they came off the wire."""
    pages, first = [], 0
    while True:
        _rtype, data = link.cmd(b"\x26", struct.pack("<BH", 0x01, first))
        if len(data) < 20:
            break
        pages.append(data)
        total, count = struct.unpack_from("<H", data, 8)[0], struct.unpack_from("<H", data, 12)[0]
        first += count
        if count == 0 or first >= total:
            break
    return b"".join(pages)


def collect(link, sp, out, label, n, settle=0.0):
    if settle:
        time.sleep(settle)
    got = 0
    while got < n:
        try:
            link.cycle_arm()
            b = raw_frame(link)
        except Exception:
            continue
        if len(b) < 20:
            continue
        out.write(struct.pack("<I", len(b)))
        out.write(b)
        got += 1
    print(f"  captured {got:3d} frames — {label}")


def main():
    rpm = int(sys.argv[1]) if len(sys.argv) > 1 else 3000
    if not DUMP.exists():
        print(f"  build the harness first: cmake --build apps/studio-jf/build --target cycle_diag_dump")
        return 2

    sp = open_stim()
    l = TsLink(); configure(l, WHEELS[WHEEL]); configure_firing(l); l.close()
    select(sp, WHEEL, reps=4); set_fixed_rpm(sp, rpm)
    l = TsLink(); wait_for_sync(l, 30.0)
    for _ in range(40):
        if l.telem_all()["sync_level"] >= 2:
            break
        time.sleep(0.5)
    l.execute("key on"); time.sleep(1.5)

    path = ROOT / "apps" / "studio-jf" / "build" / "cycle_frames.bin"
    with open(path, "wb") as out:
        collect(l, sp, out, f"running steadily @ {rpm} rpm", 40)
        stim_halt(sp)
        collect(l, sp, out, "engine STOPPED (frames must be stale, not live)", 10, settle=1.0)
        stim_resume(sp)
        collect(l, sp, out, "after a RESTART — sync re-acquired", 40, settle=3.0)
    l.execute("key auto"); l.close(); sp.close()

    print(f"\n  --- judged by the shipped studio decoder + diagnostics ---")
    r = subprocess.run([str(DUMP), str(path)], capture_output=True, text=True)
    # Only the frames worth reading: the summary line plus anything flagged.
    for line in r.stdout.splitlines():
        if any(m in line for m in ("[!]", "[~]", "\u00b7 ", "UNTRUSTWORTHY", "frames,")):
            print(line)
    print(f"\n  {'ALL FRAMES TRUSTWORTHY' if r.returncode == 0 else 'UNTRUSTWORTHY FRAMES PRESENT'}")
    return r.returncode


if __name__ == "__main__":
    raise SystemExit(main())
