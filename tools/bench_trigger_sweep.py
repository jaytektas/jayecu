#!/usr/bin/env python3
"""Capture every stimulator wheel as a RAW trigger log, for the fitter to be judged against.

THE ECU IS NEVER TOLD WHAT IT IS LOOKING AT. One generic capture config is written once — both
capture channels armed on BOTH edges — and every wheel is then spun past it unchanged. That is the
case the fitter exists for, and configuring the decoder per wheel would quietly answer the question
the fitter is supposed to answer: a log taken with the right wheel already programmed proves only
that the geometry round-trips.

Both edges, always. A stream armed Rising logs one record per tooth and the mark/space widths are
simply not in the data, so a wheel whose identity is in its SLOT WIDTHS — an optical CAS, a
variable-width cam window — is unfittable before the fitter ever sees it.

Writes one CSV per wheel (t_us, levels, flags) next to the studio's other fixtures, so a capture
taken today is still there to re-run the fitter against next time it changes.

    python3 -m tools.bench_trigger_sweep [--rpm N] [--only IDX[,IDX...]]
"""
import argparse
import struct
import sys
import time
from pathlib import Path

from tools.ts_bench import TsLink
from tools.gen_rebench import (open_stim, select, stim_halt, stim_resume,
                               STREAM0, STREAM_STRIDE, F, CRANK_CAP, CAM_CAP, WHEELS)

OUT = Path(__file__).resolve().parent.parent / "apps/studio-jf/tests/fixtures/stim"
CMD = b"\x27"
ARM, READ, STOP = 0x00, 0x01, 0x02
HDR = "<IBBIIIH"
HDR_N = struct.calcsize(HDR)


def generic_capture(l):
    """Arm dig1 and dig3, both edges, and disable every other slot. No wheel geometry at all."""
    for s in range(6):
        base = STREAM0 + STREAM_STRIDE * s
        if s in (0, 2):
            l.write_raw(base + F['enabled'],       bytes([1]))
            l.write_raw(base + F['capture_index'], bytes([CRANK_CAP if s == 0 else CAM_CAP]))
            l.write_raw(base + F['edge'],          bytes([2]))          # Both
        else:
            l.write_raw(base + F['enabled'],       bytes([0]))


def read_log(l, want=4096):
    recs, first, hdr0 = [], 0, None
    while len(recs) < want:
        _rt, data = l.cmd(CMD, struct.pack("<BI", READ, first))
        if len(data) < HDR_N:
            break
        magic, ver, state, total, base, pfirst, count = struct.unpack_from(HDR, data, 0)
        if hdr0 is None:
            hdr0 = dict(version=ver, state=state, total=total, base=base)
        if magic != 0x474F4C54:
            break
        for i in range(count):
            recs.append(struct.unpack_from("<IBB", data, HDR_N + i * 6))
        first += count
        if count == 0 or first >= total:
            break
    return hdr0, recs


def capture(sp, idx, rpm, dwell):
    """Halt, arm, resume — in that order. Arming is refused while the engine runs, and a refused arm
    is SILENT: the ring keeps whatever it held, so the capture reads as a different wheel entirely."""
    if not select(sp, idx, 6):
        return None, "the stim would not take the wheel"
    # SET THE SPEED BEFORE STOPPING, not after starting. A halted wheel keeps the timer value it had,
    # so resuming and THEN commanding the rpm leaves a stretch of capture running at the old rate —
    # around fifty teeth of it, dead steady, in the middle of a supposedly cranking signal. The
    # fitter copes with that now, because a real engine spins up from rest too, but there is no
    # reason to put it in a fixture on purpose.
    sp.write(b"F" + struct.pack("<H", int(rpm)))
    time.sleep(0.3)
    stim_halt(sp); time.sleep(1.0)
    l = TsLink()
    l.cmd(CMD, struct.pack("<BI", ARM, 0))
    stim_resume(sp)
    time.sleep(dwell)
    hdr, recs = read_log(l)
    l.cmd(CMD, struct.pack("<B", STOP))
    l.close()
    return (hdr, recs), ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rpm", type=int, default=300)
    ap.add_argument("--dwell", type=float, default=3.0)
    ap.add_argument("--only", default="")
    # THE CRANK CURVE, in the stim's firmware. A capture of an unknown wheel is always taken on a
    # starter, and a starter does not turn the engine at a speed — it stalls against each compression
    # and runs away between them. Sweeping at a dead steady rpm tests the fitter against a signal no
    # engine produces.
    ap.add_argument("--amp", type=int, default=0, help="curve amplitude, percent of mean, 0 = steady")
    ap.add_argument("--halves", type=int, default=4, help="compressions per revolution x2: 4 a four, 6 a six")
    ap.add_argument("--tag", default="", help="suffix for the fixture filenames")
    a = ap.parse_args()

    OUT.mkdir(parents=True, exist_ok=True)
    sp = open_stim()
    print(f"stim on {sp.port}")
    sp.write(b"V" + bytes([max(0, min(90, a.amp)), a.halves]) + struct.pack("<H", 3000))
    time.sleep(0.4)
    sp.reset_input_buffer(); sp.write(b"v"); time.sleep(0.3)
    # ASK THE STIM WHAT IT TOOK. It is one dropped byte from having read the amplitude as the wheel
    # number, and a curve that silently stayed at zero would look exactly like a fitter that copes.
    got = sp.read(64).decode(errors="replace").strip()
    print(f"crank curve: {got}  (amplitude%, compressions per rev x2, rpm ceiling)")
    if got.split(",")[0] != str(max(0, min(90, a.amp))):
        raise SystemExit(f"the stim did not take the curve — it reports {got}")
    l = TsLink(); generic_capture(l); l.cmd(b"E", b"reconfig"); l.close()
    print("ECU armed: dig1 + dig3, both edges, no wheel configured\n")

    want = [int(x) for x in a.only.split(",")] if a.only else sorted(WHEELS)
    for idx in want:
        name = WHEELS[idx][0]
        slug = "".join(c if c.isalnum() else "_" for c in name).strip("_").lower()
        got, why = capture(sp, idx, a.rpm, a.dwell)
        if not got:
            print(f"  {idx:>2} {name:<20} SKIP  {why}")
            continue
        hdr, recs = got
        path = OUT / f"{idx:02d}_{slug}_{a.rpm}rpm{a.tag}.csv"
        with path.open("w") as f:
            for t, lv, fl in recs:
                f.write(f"{t},{lv},{fl}\n")
        streams = sorted({(fl >> 5) & 7 for _, _, fl in recs})
        print(f"  {idx:>2} {name:<20} {len(recs):>5} records  streams {streams}  -> {path.name}")
    sp.write(b"F" + struct.pack("<H", 0))


if __name__ == "__main__":
    sys.exit(main())
