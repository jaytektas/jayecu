#!/usr/bin/env python3
"""Where the knock window actually sits, measured on the bench against the signal generator.

The window is armed from each cylinder's own spark: it opens Window Start (knock.window_before_spark_deg)
before that spark and listens for Window Duration (knock.window_length_deg), never opening earlier than
85 deg BTDC. The host tests prove the arithmetic; this proves the firmware stamps its real samples there.

With the generator's tone on KNOCK1 every window is loud, so every window is classified, and `knk`
reports the last one's spark angle and the crank span its samples were taken over: btdc[start..end]
(+ = BTDC, as spark advance). Each case fixes the timing (ignition.fixed_timing) and the two window
settings, then checks the span:  start = min(spark + before, 85),  end = start - length.

Needs the DSO2D15 generator into KNOCK1, the Ardu-Stim, and the studio disconnected (it holds the port).
Everything it changes on the ECU is in RAM: it ends with a reset, which reloads the burned tune, and
compares the whole tune image before and after to prove it. The generator is put back as it was found.

    python3 -m tools.bench_knock_window [rpm]
"""
import re
import sys
import time

from tools.dso2d15 import Dso2d15
from tools.ts_bench import TsLink
from tools.gen_rebench import open_stim, select, configure, WHEELS
from tools.bench_fuel import set_fixed_rpm, wait_for_sync, INJECT, TTL_MS
from tools.bench_cycle import configure_firing

WHEEL_IDX = 4            # "60-2 crank and cam" — knock windows need phase sync
DEFAULT_RPM = 1500
FREQ = 7000
CLAMP_BTDC = 85.0
TOL_DEG = 2.0            # one crank tooth is 6 deg on 60-2, but the window is stamped from the
                         # interpolated angle; 2 deg is generous for that and tight against any wrong anchor

# (spark advance, Window Start before spark, Window Duration)
CASES = [
    (20.0, 10, 80),      # the defaults: 30 BTDC .. 50 ATDC
    (35.0, 10, 80),      # more advance: the window moves with the spark
    (5.0, 10, 80),       # less advance
    (20.0, 25, 40),      # both settings changed
    (40.0, 60, 80),      # 100 BTDC asked for: clamped to 85
]

fails = []


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}: {detail}")
    if not cond:
        fails.append(name)


def script():
    lines = [f'  signalWrite("{n}", {v}, {TTL_MS})' for n, v in INJECT.items()]
    return "function onTick()\n" + "\n".join(lines) + "\nend\nsetTickRate(200)\n"


def last_shot(l):
    """(seq, cyl, spark, start_btdc, end_btdc) of the last classified window, or None."""
    o = l.execute("knk")
    m = re.search(r"last#(\d+) cyl=(\d+) .*? spark=(-?\d+)", o)
    p = re.search(r"btdc\[(-?\d+)\.\.(-?\d+)\]", o)
    if not (m and p):
        return None
    return int(m.group(1)), int(m.group(2)), int(m.group(3)), int(p.group(1)), int(p.group(2))


def window_db(l, n=8):
    """Mean level (dB) of the next n classified windows."""
    out, seen, t0 = [], set(), time.time()
    while len(out) < n and time.time() - t0 < 8.0:
        o = l.execute("knk")
        m = re.search(r"last#(\d+) .*? db_x10=(-?\d+)", o)
        if m and m.group(1) not in seen:
            seen.add(m.group(1)); out.append(int(m.group(2)) / 10.0)
        time.sleep(0.15)
    return sum(out) / len(out) if out else None


def shots(l, n=6, timeout=8.0):
    """n distinct classified windows (by seq), newest last."""
    out, seen, t0 = [], set(), time.time()
    while len(out) < n and time.time() - t0 < timeout:
        s = last_shot(l)
        if s and s[0] not in seen:
            seen.add(s[0]); out.append(s)
        time.sleep(0.15)
    return out


def main():
    rpm = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_RPM
    print(f"=== knock window placement @ {rpm} rpm ===")

    d = Dso2d15()
    gen0 = d.state()
    print(f"  gen: {d.idn()}  found {gen0}")
    d.output(False); d.burst_off(); d.wave("SINE"); d.freq(FREQ); d.offset(0); d.amp(1.0)

    l = TsLink()
    size = l.meta.config_size
    image0 = l.read_config_chunked(0, size)
    sp = open_stim()
    configure(l, WHEELS[WHEEL_IDX]); configure_firing(l)
    l.set_config("knock_enabled", 1)
    l.set_config("knock_knock_frequency", FREQ)
    l.set_config("knock_preign_enabled", 0)           # a continuous tone is loud before the spark too
    cs = l.meta.config["knock"]["cyl_sensor"]
    for c in range(4):                                # every cylinder on KNOCK1
        l.write_raw(cs["base_offset"] + c * cs["stride"], bytes([0]))
    l.set_config("ignition_fixed_timing_enable", 1)
    l.close()
    select(sp, WHEEL_IDX, reps=4); set_fixed_rpm(sp, rpm)

    l = TsLink()
    try:
        synced = wait_for_sync(l) is not None
        check("crank sync", synced, "" if synced else "no teeth reached the ECU; no case was run")
        if synced:
            l.execute("key on"); l.set_script(script())
            time.sleep(3)
            quiet = window_db(l)
            d.output(True)
            time.sleep(2)
            loud = window_db(l)
            # Not a gate: placement is stamped from the crank whatever the level. It says whether the
            # tone is reaching KNOCK1 at all, which detection (bench_knock_siggen) depends on.
            print(f"  window level: generator off {quiet} dB, 1 Vpp on {loud} dB")

        for spark, before, length in (CASES if synced else []):
            l.set_config("ignition_fixed_timing_deg", int(round(spark * 10)))   # x0.1 deg
            l.set_config("knock_window_before_spark_deg", before)
            l.set_config("knock_window_length_deg", length)
            time.sleep(1.5)                               # let windows armed under the old settings drain
            got = shots(l)
            want_start = min(spark + before, CLAMP_BTDC)
            want_end = want_start - length
            label = f"spark {spark:g} BTDC, start {before} before, length {length}"
            if not got:
                check(label, False, "no window was classified"); continue
            sparks = sorted({s[2] for s in got})
            spans = [(s[3], s[4]) for s in got]
            ok_spark = all(abs(s[2] - spark) <= 1 for s in got)
            ok_span = all(abs(a - want_start) <= TOL_DEG and abs(b - want_end) <= TOL_DEG for a, b in spans)
            check(label, ok_spark and ok_span,
                  f"want btdc[{want_start:g}..{want_end:g}]  got {sorted(set(spans))}  spark {sparks}  "
                  f"cyls {sorted({s[1] for s in got})}")
    finally:
        d.output(False)
        try:
            l.restore_script(); l.execute("key auto")
        except Exception as ex:
            print(f"  cleanup: {ex}")
        try:
            l.execute("reset")                          # reload the burned tune: every change above was RAM
        except Exception:
            pass
        l.close(); sp.close()
        # the generator as it was found
        d.wave(gen0["type"]); d.freq(gen0["freq"]); d.amp(gen0["amp"]); d.offset(gen0["offset"])
        if gen0["burst"].upper().startswith("ON"):
            d.write(":DDS:BURSt:SWITch ON"); d.write(f":DDS:BURSt:CNT {gen0['cnt']}")
        d.output(gen0["out"].upper().startswith("ON"))
        print(f"  gen put back: {d.state()}")
        d.close()

    time.sleep(6)
    l = TsLink()
    check("burned tune unchanged after the reset", l.read_config_chunked(0, size) == image0)
    l.close()

    print(f"\n=== {'PASS' if not fails else 'FAIL: ' + ', '.join(fails)} ===")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
