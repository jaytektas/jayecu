#!/usr/bin/env python3
"""Knock levels against the signal generator, on the bench AS IT IS — nothing on the ECU is changed.

bench_knock_siggen.py sets up its own world (wheel, firing, four cylinders, knock frequency) and leaves
most of it behind; this one only drives the generator and reads `knk`, so it can run against whatever
tune is on the bench. It needs the engine turning with spark (the knock windows are armed from the spark
schedule), the DSO2D15 generator on the knock input, and the studio disconnected (it holds the port).

    python3 -m tools.bench_knock_level [freq_hz]

  1. QUIET      generator off: the floor every cylinder learned, and the level of a quiet window
  2. SWEEP      a continuous tone, 10 mV..5 V: the level must rise 20 dB per decade of amplitude until
                the input clips, and must stop at the ADC's ceiling (a full-scale sine at the pin is
                +1.3 dB re 1 V rms; one clipped flat at both rails about +3.4)
  3. KNOCK      a tone switched on at falling amplitude, each window's verdict tallied: clean under the
                threshold, KNOCK up to the Pre-Ignition Extreme level, PRE-IGNITION above it
  4. NOT LEARNED a sustained tone must not become the floor
The generator is put back the way it was found.
"""
import math
import re
import sys
import time

from tools.dso2d15 import Dso2d15
from tools.ts_bench import TsLink


def knk(l):
    o = l.execute("knk")
    floors = [(int(a) / 10.0, int(b)) for a, b, _ in re.findall(r"(-?\d+)/(\d+)/(\d+)", o)]
    m = re.search(r"last#(\d+) cyl=(\d+) (\S+) db_x10=(-?\d+) floor_x10=(-?\d+) over_x10=(-?\d+) thr_x10=(-?\d+)", o)
    last = None
    if m:
        last = dict(seq=int(m.group(1)), cyl=int(m.group(2)), verdict=m.group(3), db=int(m.group(4)) / 10.0,
                    floor=int(m.group(5)) / 10.0, over=int(m.group(6)) / 10.0, thr=int(m.group(7)) / 10.0)
    return dict(knocks=int(re.search(r"knocks=(\d+)", o).group(1)), floors=floors, last=last, raw=o)


def sample(l, n=8, gap=0.12):
    """n distinct classified windows: their levels, floors and verdicts."""
    got, seen, t_end = [], set(), time.time() + n * gap * 6
    while len(got) < n and time.time() < t_end:
        s = knk(l)["last"]
        if s and s["seq"] not in seen:
            seen.add(s["seq"]); got.append(s)
        time.sleep(gap)
    return got


def mean(xs):
    return sum(xs) / len(xs) if xs else float("nan")


def main():
    freq = int(sys.argv[1]) if len(sys.argv) > 1 else 6662
    d = Dso2d15()
    was = {k: d.query(f":DDS:{k}?") for k in ("TYPE", "FREQ", "AMP", "OFFSet", "SWITch", "BURSt:SWITch")}
    print(f"=== knock levels vs generator, {freq} Hz ===\n  generator was: {was}")
    l = TsLink()
    try:
        s0 = knk(l)
        if not s0["last"]:
            print("  nothing classified — is the engine turning with spark and the key on?"); return 2
        seq0 = s0["last"]["seq"]; time.sleep(1.0)
        if knk(l)["last"]["seq"] == seq0:
            print("  the classifier is not running (no new windows in 1 s) — engine turning? spark on?"); return 2
        thr = s0["last"]["thr"]
        print(f"  threshold {thr:.1f} dB over floor")

        d.burst_off(); d.wave("SINE"); d.freq(freq); d.offset(0)

        # ---- 1. quiet ----------------------------------------------------------------------------
        d.output(False); time.sleep(8)
        s = knk(l); q = sample(l)
        quiet_db = mean([x["db"] for x in q])
        print(f"\n1. QUIET   window level {quiet_db:.1f} dB   floors {[f for f, _ in s['floors']]}")

        # ---- 2. continuous sweep -----------------------------------------------------------------
        print("\n2. SWEEP (continuous)   gen mVpp   gen dBV(rms)   window dB   gain dB   floor dB   over")
        rows = []
        d.output(True)
        for mv in (10, 20, 50, 100, 200, 500, 1000, 2000, 3000, 5000):
            d.amp(mv / 1000.0); time.sleep(1.5)
            w = sample(l)
            db = mean([x["db"] for x in w]); fl = mean([x["floor"] for x in w]); ov = mean([x["over"] for x in w])
            gdb = 20 * math.log10((mv / 1000.0) / (2 * math.sqrt(2)))
            rows.append((mv, gdb, db))
            print(f"                          {mv:6d}      {gdb:7.1f}      {db:7.1f}   {db - gdb:6.1f}   {fl:7.1f}  {ov:+6.1f}")
        # 20 dB per decade while linear: compare 20 -> 200 mV
        lin = {mv: db for mv, _, db in rows}
        print(f"   20->200 mV rose {lin[200] - lin[20]:.1f} dB (linear = 20.0);  top out {max(lin.values()):.1f} dB")
        # The sweep rose in steps under the threshold, so each was clean and the floor followed it up to the
        # ceiling. Wait for it to come back down to the quiet level before judging knock against it.
        d.output(False)
        for _ in range(60):
            time.sleep(2)
            fl = mean([f for f, _ in knk(l)["floors"]])
            if fl < quiet_db + 3: break
        print(f"   floor back to {fl:.1f} dB")

        # ---- 3. knock events -----------------------------------------------------------------------
        # A tone switched ON against a quiet floor: every window it covers is a sudden jump, which is what
        # knock is to the classifier. Counted by the classifier's VERDICT per window, not by the knock
        # counter (it stopped at 65535 before 2026-09-30, and a bench that had knocked that often read
        # every later knock as none) and not by the generator's N-cycle burst (its SCPI trigger gave
        # counts nobody could reproduce — bench_knock_siggen.py, "KNOWN OPEN").
        #   under the threshold -> clean;  threshold .. Pre-Ignition Extreme -> KNOCK;  above -> PRE-IGNITION
        # The generator is not accurate below ~50 mVpp (the sweep bends there), so the low rungs read low.
        print(f"\n3. KNOCK   tone switched on for 1.5 s per level   (threshold {thr:.0f} dB)")
        print("   gen mVpp   windows   mean over   verdicts")
        for mv in (300, 100, 70, 50, 40, 30, 20, 15, 12, 10):
            d.amp(mv / 1000.0); d.output(True); time.sleep(0.2)
            seen, tally, ov, t0 = set(), {}, [], time.time()
            while time.time() - t0 < 1.5:
                x = knk(l)["last"]
                if x and x["seq"] not in seen:
                    seen.add(x["seq"]); tally[x["verdict"]] = tally.get(x["verdict"], 0) + 1; ov.append(x["over"])
                time.sleep(0.05)
            d.output(False)
            print(f"   {mv:6d}     {len(seen):5d}     {mean(ov):+6.1f}    {tally}")
            time.sleep(2.5)
        print("   (PRE-IGNITION cuts the cylinder until the engine stops, with Pre-Ignition Cut Hold at 0)")

        # ---- 4. a sustained tone is not learned --------------------------------------------------
        d.amp(0.5); d.output(True); time.sleep(3)
        b = knk(l)["floors"]; time.sleep(12); a = knk(l)["floors"]
        d.output(False)
        moved = max(abs(x[0] - y[0]) for x, y in zip(a, b))
        print(f"\n4. NOT LEARNED   floors {[f for f, _ in b]} -> {[f for f, _ in a]}  (moved at most {moved:.1f} dB)")
    finally:
        # put the generator back as it was found
        d.burst_off()
        d.write(f":DDS:TYPE {was['TYPE']}"); d.write(f":DDS:FREQ {was['FREQ']}")
        d.write(f":DDS:AMP {was['AMP']}");   d.write(f":DDS:OFFSet {was['OFFSet']}")
        if was["BURSt:SWITch"].upper().startswith("ON"): d.write(":DDS:BURSt:SWITch ON")
        d.output(was["SWITch"].upper().startswith("ON"))
        d.close(); l.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
