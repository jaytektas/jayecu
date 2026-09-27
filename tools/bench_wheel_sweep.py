#!/usr/bin/env python3
"""Every wheel the rig can spin, decoded and held across an RPM RAMP.

The wheel table is the rig's own (tools/gen_rebench.py WHEELS) — not transcribed into a host test,
because a transcribed wheel is a wheel nobody has measured. What this adds over gen_rebench's
existing sweep is the thing a steady RPM cannot show: the decoder is asked to hold sync while the
speed is CHANGING, continuously, up and down, which is where a per-edge prediction window either
tracks or falls over.

The stim ramps for us. Ardu-Stim already has LINEAR_SWEPT_RPM — 'r' sets low/high/interval and it
walks 1 rpm per interval, turning at each end — so this drives that rather than stepping 'F', which
is what every other bench tool here does and which cannot exercise a transient at all.

    python3 -m tools.bench_wheel_sweep [wheel_idx ...]

Needs the stim on /dev/ttyUSB0 wired to the configured crank pin (capture idx 2 = DIG1) and, for the
PHASE wheels, a cam on DIG3.
"""
import struct
import sys
import time

from tools.ts_bench import TsLink
from tools.gen_rebench import (open_stim, select, configure, crank_teeth,
                               stim_resume, stim_halt, WHEELS, STREAM0, STREAM_STRIDE, F)

LVL = {0: "NONE", 1: "CRANK", 2: "PHASE"}
MAX_EDGE_HZ = 8000          # keep the capture ISR honest on the densest wheels
# THE RAMP USED TO NEVER FINISH. It was a flat 6 s watch over a ramp of 1 rpm per 3000 us — 333
# rpm/s — across a span that is 3000 rpm wide on every wheel but the Nissan. A full traverse needs
# 9.0 s, so the watch stopped two thirds of the way up and the "ramp lo-hi" it printed was 1000-3000
# on a wheel that was supposed to be swept to 4000. Twenty-three wheels x 6 s of that is 138 s of the
# suite's 341, spent on a sweep that never reached the top.
#
# So size the RATE to the span instead of the duration: cover the whole range in RAMP_TRAVERSE_S, and
# stop as soon as both ends have actually been seen. That is a full sweep in less than half the time,
# and it is also a slightly harder test — 1000 rpm/s where it used to be 333, still gentler than a
# real engine's snap. The cap is two traverses because the stim BOUNCES between the ends and may be
# starting from anywhere in the range, so covering both can take one and a half passes.
RAMP_TRAVERSE_S = 3.0

fails, skipped = [], []


def crank_present_teeth(streams):
    """Teeth that actually ARRIVE in one crank revolution — slots minus the missing ones."""
    for st in streams:
        if st["rate"] != 0:
            continue
        if st["prim"] == 1:
            return len(st["cell"])
        missing = (st["ratio"] - 1) * len(st["cell"]) if st["ratio"] else 0
        return max(1, st["slots"] - missing)
    return 1


def ask_rpm(sp):
    """The STIM's own idea of its speed, to check the ECU's decode against."""
    sp.reset_input_buffer()
    sp.write(b"R")
    time.sleep(0.12)
    try:
        return int(sp.readline().decode(errors="replace").strip())
    except Exception:
        return 0


def set_ramp(sp, lo, hi, interval_us):
    """LINEAR_SWEPT_RPM: 1 rpm per interval_us, bouncing between lo and hi."""
    sp.reset_input_buffer()
    sp.write(b"r" + struct.pack("<HHH", int(lo), int(hi), int(interval_us)))
    time.sleep(0.3)


# HOW LONG A RECONFIGURE ACTUALLY TAKES, measured on this rig rather than guessed: the 'reconfig'
# command only sets a flag ("reconfig queued", 0 ms) and the rebuild happens on the next pass of the
# main loop. Watching trigger_sync_ceiling change as the wheel changes puts it at 275-465 ms, with a
# telemetry round-trip of 4 ms and configure()'s own writes at 35 ms. The sleeps in this file were
# 1.5-2.0 s against that.
#
# There is no telemetry that says "the rebuild is done", and the ceiling only helps when the new
# wheel's ceiling DIFFERS from the old one — consecutive PHASE wheels leave it unchanged. So these
# waits poll for the expected ceiling where that is informative and keep a floor of 0.6 s (above the
# measured 465 ms worst case) where it is not. Never a bare "it changed", which would sail straight
# through on a wheel whose ceiling happens to match.
RECONFIG_FLOOR = 0.6

LVLN = {"NONE": 0, "CRANK": 1, "PHASE": 2}


def wait_reconfig(l, want_ceiling=None, timeout=1.5, floor=RECONFIG_FLOOR):
    t0 = time.time()
    while time.time() - t0 < timeout:
        f = l.telem_all()
        done = want_ceiling is None or f["trigger_sync_ceiling"] == want_ceiling
        if done and time.time() - t0 >= floor:
            return True
        time.sleep(0.01)
    return False


def sweep_one(l, sp, idx):
    spec = WHEELS[idx]
    name, streams, _cam_edge, expect = spec
    # STOP THE WHEEL BEFORE TOUCHING THE ECU. The previous wheel left the stim mid-RAMP, up to 4000
    # rpm, and that kept running while this one's config was written and the decoder rebuilt. For most
    # wheels the leftover speed is survivable; on the densest it is not. A 180-slit CAS at the 4000 rpm
    # the PREVIOUS wheel had reached is 24 kHz of edges, which starved the comms task hard enough that
    # the ECU stopped answering — the suite then reported "no matching response" against the Nissan CAS
    # and it read as that wheel failing. It does not: run it alone and it passes, PHASE, 0.3 % rpm
    # error, zero trigger errors. What failed was the handover.
    #
    # Halting also puts the reconfigure in the right order — a decoder still being fed the old pattern
    # never drops below the rpm gate, so the new config would not apply cleanly anyway.
    stim_halt(sp)
    configure(l, spec)
    l.execute("reconfig")
    wait_reconfig(l, LVLN.get(expect), timeout=1.5)

    # Keep the edge rate sane on a dense wheel: a 180-slit crank track at 4000 rpm is 12 kHz.
    teeth = max(1, crank_teeth(streams))
    hi = int(min(4000, MAX_EDGE_HZ * 60 / teeth))
    lo = max(400, hi // 4)
    if hi <= lo + 200:
        skipped.append(f"{idx} {name} (too dense to ramp: {teeth} teeth)")
        return

    stim_resume(sp)
    hold = (lo + hi) // 2
    sp.write(b"F" + struct.pack("<H", hold))
    # WAIT FOR LOCK **AND** FOR THE COMMANDED SPEED, and do not try to be clever about the refusal.
    #
    # Two things went wrong writing this the obvious way. First, exiting on "ceiling == NONE" to catch
    # the refusal case early looked free and is not: the ceiling reads 0 TRANSIENTLY while a
    # reconfigure is in flight, so the wait fell out of the loop mid-rebuild with sync still 0 and the
    # wheel was reported as never syncing. 60-2+cam and the Nissan CAS both "failed" that way and both
    # are fine. A genuine refusal now simply costs the full timeout, which is what it cost before.
    #
    # Second, breaking on sync alone samples rpm part-way up the spin-up from a halted stim: 36-1 read
    # 1410 rpm against a true 2503 and was reported as a 43.7 % decode error. What the section needs
    # is the rig AT SPEED and locked, so wait for both, and require it to hold for a few frames rather
    # than trusting one crossing.
    t0, run = time.time(), 0
    while time.time() - t0 < 3.0:
        e = l.telem_all()
        at_speed = e["rpm"] and abs(e["rpm"] - hold) < hold * 0.05
        run = run + 1 if (e["sync_level"] != 0 and at_speed) else 0
        if run >= 3:
            break
        time.sleep(0.01)

    e = l.telem_all()
    ceiling = LVL.get(e["trigger_sync_ceiling"], "?")
    got = LVL.get(e["sync_level"], "?")
    if got == "NONE":
        if ceiling == "NONE":
            # NOT a failure to sync — a REFUSAL to pretend. A distributor's few even teeth carry no
            # unique feature, so with no cam this configuration can never know where the engine is,
            # and the config check says so before it is asked to run. Reaching NONE here is the
            # designed answer.
            print(f"  [PASS]  {idx:3d} {name:22s} refused: ceiling NONE, no absolute reference")
            return
        skipped.append(f"{idx} {name}: no sync (expected {expect}, ceiling {ceiling}) "
                       f"— PHASE wheels need the cam on DIG3")
        return

    # DOES THE ECU AGREE WITH THE STIM ABOUT HOW FAST IT IS TURNING? Syncing only says the pattern
    # matched; the SPEED says the geometry did. A wheel described with the wrong tooth count can still
    # lock and still hold, and report an rpm scaled by the ratio of the counts — which is a decode
    # that looks perfectly healthy and is wrong by tens of percent.
    # GROUND TRUTH IS THE EDGE RATE, not the stim's claim — which on at least one wheel was wrong:
    # the 24-1's rpm_scaler was 0.5 where every other wheel's is (edges per 360)/120, so it spun 25%
    # faster than it reported and anything calibrated against it was calibrated against a lie.
    #
    # Measured with the CAM STREAMS OFF. trigger_teeth counts every enabled stream, so leaving a cam
    # live adds its edges to a count that is then divided by the CRANK's tooth count — inflating the
    # answer in proportion to how coarse the crank is. Measured: 24.8% out on a 4-1, 3.7% on a 36-1,
    # 1.3% on the Nissan's 180-slit track. That reads like six decoder faults and is one test bug.
    stim_rpm = ask_rpm(sp)
    ecu_rpm = e["rpm"]
    for slot in range(6):
        if slot >= 2:
            l.write_raw(STREAM0 + STREAM_STRIDE * slot + F["enabled"], b"\x00")
    # THE FLOOR MATTERS HERE more than anywhere. If this returns before the rebuild lands, the cam
    # streams are still enabled and their edges are still counted into trigger_teeth — which is the
    # exact ~25 % overestimate the comment above describes. On a PHASE wheel the ceiling dropping to
    # CRANK proves it applied; on a crank-only wheel there is nothing to see and the floor is all
    # there is.
    l.execute("reconfig")
    wait_reconfig(l, 1 if LVLN.get(expect) == 2 else None, timeout=1.8)
    # Sample the count FRESH at t0. Reusing an earlier frame counts teeth from before the window
    # opened against a window that does not include them — a consistent ~7% overestimate, which reads
    # exactly like a real decode error until you notice every wheel is wrong by the same amount.
    # THE WINDOW IS SIZED BY EDGES, NOT BY SECONDS. What this measurement needs is enough teeth for
    # the rate to be precise; a fixed 1.5 s gave a 180-slit CAS 7500 edges (absurd precision) and a
    # 4-1 only 250. Stop once 400 edges are in — 0.25 % resolution — but never before 0.4 s, so the
    # window is always long against the 4 ms telemetry round-trip. Sparse wheels still take the full
    # 1.5 s and are no worse off than before.
    n0 = l.telem_all()["trigger_teeth"]; t0 = time.time()
    while True:
        dt = time.time() - t0
        n1 = l.telem_all()["trigger_teeth"]
        if dt >= 1.5 or (dt >= 0.4 and n1 - n0 >= 400):
            break
        time.sleep(0.01)
    eps = (n1 - n0) / (time.time() - t0)
    present = max(1, crank_present_teeth(streams))
    true_rpm = eps * 60.0 / present
    rpm_err = abs(ecu_rpm - true_rpm) / true_rpm * 100.0 if true_rpm > 0 else 999.0
    configure(l, spec); l.execute("reconfig")                    # cam back for the level + ramp test
    wait_reconfig(l, LVLN.get(expect), timeout=2.0)
    e = l.telem_all()
    got = LVL.get(e["sync_level"], "?")

    base = (e["trigger_noise_edges"], e["trigger_missed_teeth"], e["trigger_phase_lost"])
    lo_seen, hi_seen, lvl_min = 99999, 0, 9

    # Now RAMP, and watch it the whole way rather than sampling the ends.
    interval_us = max(1, int(RAMP_TRAVERSE_S * 1e6 / max(1, hi - lo)))
    set_ramp(sp, lo, hi, interval_us)
    t0 = time.time()
    while time.time() - t0 < RAMP_TRAVERSE_S * 2:
        f = l.telem_all()
        r = f["rpm"]
        if r:
            lo_seen, hi_seen = min(lo_seen, r), max(hi_seen, r)
        lvl_min = min(lvl_min, f["sync_level"])
        # both ends reached: the sweep is done, and the decoder held all the way across it
        if hi_seen >= hi * 0.97 and lo_seen <= lo * 1.03:
            break
    f = l.telem_all()
    d = (f["trigger_noise_edges"] - base[0],
         f["trigger_missed_teeth"] - base[1],
         f["trigger_phase_lost"] - base[2])

    held = LVL.get(lvl_min, "?")
    clean = (d == (0, 0, 0))
    ok = (held == got) and clean and rpm_err < 3.0
    # A PHASE wheel that only reached CRANK has not failed the decoder — the cam is not on the wire.
    # Say so rather than printing PASS beside an expectation that was not met.
    short = (expect == "PHASE" and got == "CRANK")
    tag = ("PASS" if ok else "FAIL") if not short else ("CRANK" if ok else "FAIL")
    if short and ok:
        skipped.append(f"{idx} {name}: reached CRANK, not PHASE — cam not wired on DIG3")
    if not ok:
        fails.append(f"{idx} {name}")
    print(f"  [{tag}] {idx:3d} {name:22s} expect={expect:5s} got={got:5s} ceiling={ceiling:5s} "
          f"held={held:5s} rpm ecu={ecu_rpm} true={true_rpm:.0f} ({rpm_err:.1f}%) stim={stim_rpm} "
          f"ramp {lo_seen}-{hi_seen}  noise+{d[0]} missed+{d[1]} phase_lost+{d[2]}")


def main():
    want = [int(a) for a in sys.argv[1:]] or sorted(WHEELS)
    sp = open_stim()
    l = TsLink()
    print(f"=== wheel sweep: {len(want)} wheels, ramped ===")
    print("  ecu:", l.hello().strip()[:70])
    for idx in want:
        if idx not in WHEELS:
            continue
        select(sp, idx)
        try:
            sweep_one(l, sp, idx)
        except Exception as exc:                      # a wheel that wedges must not lose the run
            fails.append(f"{idx} ({exc})")
            print(f"  [FAIL] {idx:3d} {WHEELS[idx][0]:22s} {exc}")
    stim_halt(sp)
    l.close(); sp.close()
    if skipped:
        print("\nnot exercised:")
        for s in skipped:
            print(f"    {s}")
    print(f"\n{'ALL PASS' if not fails else 'FAILED: ' + ', '.join(fails)}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
