#!/usr/bin/env python3
"""The decoder against faults that arrive as WIRING and CONFIGURATION, on real hardware.

The wheel sweep proves healthy wheels and the host suite injects faults into a synthesised edge
stream. This is the third kind: the mistakes and failures that happen at the loom and in the tune —
the wrong wheel named, a cam that never arrives, the two sensors swapped, a trigger that stops at
speed. None of them can be simulated honestly, because what is being tested is whether the ECU's
belief matches a physical rig it cannot see.

    python3 -m tools.bench_trigger_faults
"""
import struct
import sys
import time

from tools.ts_bench import TsLink
from tools.gen_rebench import (open_stim, select, configure, stim_resume, stim_halt,
                               WHEELS, STREAM0, STREAM_STRIDE, F)

LVL = {0: "NONE", 1: "CRANK", 2: "PHASE"}
fails = []


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}: {detail}")
    if not cond:
        fails.append(name)


def w(l, slot, field, val, fmt="<B"):
    l.write_raw(STREAM0 + STREAM_STRIDE * slot + F[field], struct.pack(fmt, val))


def settle(l, s=3.0, until=None):
    """Reconfigure, then wait.

    WITH `until`, poll for it and return as soon as it holds — `s` becomes the timeout, so a working
    ECU costs a fraction of it and a broken one still waits the full time and still fails.

    WITHOUT it, sleep the whole `s`. That is not laziness: most claims in this file are NEGATIVE
    ("never syncs", "refuses to sync"), and you cannot poll for something failing to happen. There
    the elapsed time IS the evidence, and it is only worth anything because the positive cases here
    show the same rig reaching sync well inside it.
    """
    l.execute("reconfig")
    if until is None:
        time.sleep(s)
    else:
        l.wait_until(until, timeout=s)
    return l.telem_all()


def rpm_of(l, f):
    return f["rpm"] * l.meta.telem["rpm"]["scale"]


def wait_hold(l, pred, timeout=3.0, n=3):
    """Wait for `pred` to hold on n CONSECUTIVE frames.

    The stim RAMPS to a commanded speed, it does not step. A plain first-crossing poll on
    "rpm > 2500" therefore returns part-way up the ramp, where the decoder may still be re-locking —
    and the very next read, the one the check uses, can catch sync at 0. That failed one run in two
    while nothing was wrong with the ECU. Requiring the condition to survive a few frames is the
    difference between "it happened once" and "it is true".
    """
    t0, run = time.time(), 0
    while time.time() - t0 < timeout:
        run = run + 1 if pred(l.telem_all()) else 0
        if run >= n:
            return True
        time.sleep(0.02)
    return False


def main():
    sp = open_stim()
    l = TsLink()
    print("=== decoder vs wiring and configuration faults ===")
    print("  ecu:", l.hello().strip()[:70])

    # ---- A. the wrong wheel named in the tune ------------------------------------------------
    select(sp, 4); stim_resume(sp)                       # rig spins 60-2 + cam
    sp.write(b"F" + struct.pack("<H", 1500)); time.sleep(1.0)
    configure(l, WHEELS[6])                              # tune says 36-1
    e = settle(l)
    check("wrong wheel named: never syncs", e["sync_level"] == 0,
          f"sync={LVL[e['sync_level']]} (rig is 60-2, tune says 36-1)")
    check("wrong wheel named: rpm stays zero", e["rpm"] == 0, f"rpm={e['rpm']}")

    # ---- B. a cam the tune expects and the loom does not provide ------------------------------
    select(sp, 3)                                        # 60-2, CRANK ONLY: no cam on the wire
    time.sleep(0.5)
    configure(l, WHEELS[4])                              # tune expects 60-2 + cam
    e = settle(l, until=lambda f: f["sync_level"] == 1)
    check("cam expected, cam absent: reaches CRANK", e["sync_level"] == 1,
          f"sync={LVL[e['sync_level']]}, ceiling={LVL[e['trigger_sync_ceiling']]}")
    a = l.telem_all(); time.sleep(3.0); b = l.telem_all()   # measurement window: KEEP the dwell
    # ASSERTED AFTER THE DWELL, not before it. It used to be re-read immediately, which asked only
    # "has it claimed PHASE yet"; three seconds of crank later it asks "did it ever", which is the
    # claim that was meant. Free, because the noise window below already pays for the time.
    check("cam expected, cam absent: does NOT claim PHASE", b["sync_level"] != 2,
          "a revolution nothing is measuring is the state to avoid")
    check("cam expected, cam absent: crank keeps running clean",
          b["trigger_noise_edges"] == a["trigger_noise_edges"] and
          b["trigger_missed_teeth"] == a["trigger_missed_teeth"],
          f"noise+{b['trigger_noise_edges']-a['trigger_noise_edges']} "
          f"missed+{b['trigger_missed_teeth']-a['trigger_missed_teeth']}")

    # ---- C. the two sensors swapped at the connector -------------------------------------------
    select(sp, 4); time.sleep(0.5)
    configure(l, WHEELS[4])
    w(l, 0, "capture_index", 4)                          # crank stream told to look at DIG3
    w(l, 2, "capture_index", 2)                          # cam stream told to look at DIG1
    e = settle(l)
    check("sensors swapped: refuses to sync", e["sync_level"] == 0,
          f"sync={LVL[e['sync_level']]} — the crank pattern is not on the pin named")

    # ---- D. the trigger stops at speed ---------------------------------------------------------
    configure(l, WHEELS[4]); settle(l, until=lambda f: f["sync_level"] >= 1)
    sp.write(b"F" + struct.pack("<H", 3000))
    # "at speed AND locked", held — see wait_hold for why a first crossing is not enough here.
    wait_hold(l, lambda f: rpm_of(l, f) > 2900 and f["sync_level"] >= 1, timeout=2.5)
    e = l.telem_all()
    if e["sync_level"] == 0:
        check("stop at speed: precondition", False, "did not sync before the test")
    else:
        stim_halt(sp)
        t0 = time.time()
        while time.time() - t0 < 5.0:
            if l.telem_all()["sync_level"] == 0:
                break
        dt = time.time() - t0
        check("stop at speed: sync drops promptly", dt < 0.5, f"{dt*1000:.0f} ms at 3000 rpm")
        # SYNC DROPPING AND RPM CLEARING ARE TWO EVENTS, and this used to read rpm in the same breath
        # as the sync it had just watched drop — a race it lost about one run in four, in the ORIGINAL
        # file as well as this one. rpm reaching zero is a positive claim, so wait for it and report
        # how long it took; a decoder that never clears it still fails, on the value.
        zok, zdt, zf = l.wait_until(lambda fr: fr["rpm"] == 0, timeout=1.0)
        check("stop at speed: rpm goes to zero", zok,
              f"rpm={zf['rpm']} after {(dt + zdt)*1000:.0f} ms")
        # ---- E. and it comes back when cranked again -------------------------------------------
        stim_resume(sp)
        sp.write(b"F" + struct.pack("<H", 1200))
        # SUSTAINED, not a single sample. Right after a resume the decoder is re-acquiring and a lone
        # read can catch a transient either way; requiring two consecutive frames is the difference
        # between "it synced" and "one sample happened to say so".
        t0, run = time.time(), 0
        while time.time() - t0 < 10.0:
            run = run + 1 if l.telem_all()["sync_level"] != 0 else 0
            if run >= 2:
                break
        e = l.telem_all()
        check("restart: re-acquires with no reconfigure", e["sync_level"] != 0,
              f"sync={LVL[e['sync_level']]} after {time.time()-t0:.1f}s")

    # ---- F. the edge-rate envelope -------------------------------------------------------------
    # Not a fault so much as the wall behind them: a wheel dense enough at rpm enough will outrun the
    # capture path, and it matters which way it fails. The Nissan CAS's 180-slit fine ring is the
    # densest thing the rig owns and reaches 21k edges/s at 7000 rpm; a 60-2 does not get near it
    # even at the stim's 8000 rpm ceiling.
    select(sp, 34); stim_resume(sp)
    configure(l, WHEELS[34]); settle(l, 2.0, until=lambda f: f["sync_level"] >= 1)
    sp.write(b"F" + struct.pack("<H", 7000))
    # likewise: the claim is that PHASE is HELD across the window below, so establish it first
    wait_hold(l, lambda f: rpm_of(l, f) > 6800 and f["sync_level"] == 2, timeout=2.0)
    a = l.telem_all(); t0 = time.time(); time.sleep(2.0); b = l.telem_all()
    eps = (b["trigger_teeth"] - a["trigger_teeth"]) / (time.time() - t0)
    check("densest wheel at 7000 rpm: holds PHASE clean", b["sync_level"] == 2 and
          b["trigger_noise_edges"] == a["trigger_noise_edges"],
          f"{eps:.0f} edges/s, sync={LVL[b['sync_level']]}, "
          f"noise+{b['trigger_noise_edges']-a['trigger_noise_edges']}")
    check("...and that is a useful envelope", eps > 15000, f"{eps:.0f} edges/s sustained")
    # Past it the right failure is to reject the marginal edges and KEEP position, not to lose sync:
    # measured at 8000 rpm / 24k edges/s it took 4 rejections and still held PHASE.
    # NOT CONVERTIBLE: the claim is that it holds position WHILE rejecting marginal edges, so the
    # dwell at the ceiling is the test. Polling for 8000 rpm and reading sync straight away would
    # pass before a single edge had been rejected.
    sp.write(b"F" + struct.pack("<H", 8000)); time.sleep(2.5)
    f = l.telem_all()
    check("beyond it: degrades by rejecting, not by losing position", f["sync_level"] == 2,
          f"sync={LVL[f['sync_level']]} at the stim's ceiling")
    sp.write(b"F" + struct.pack("<H", 1200))

    configure(l, WHEELS[4]); settle(l, 1.0, until=lambda f: f["sync_level"] >= 1)
    l.close(); sp.close()
    print(f"\n{'ALL PASS' if not fails else 'FAILED: ' + ', '.join(fails)}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
