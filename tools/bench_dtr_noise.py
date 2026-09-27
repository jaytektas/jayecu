#!/usr/bin/env python3
"""What does an Ardu-Stim reset actually cost the decoder? Measured, not assumed.

This exists because a bench comment once claimed "1730 spurious edges per DTR reset" and used it as
evidence that the rig is a noisy environment. It was wrong three ways: the figure was a free-running
LIFETIME counter read as a per-event delta, it was the MISSED counter rather than the noise one, and
so it said the opposite of what it was quoted for. The real cost of a reset, over two runs, is
missed +1 and noise +0..1 — sync drops within ~56 ms and re-acquires 1.7 s later.

Two traps this tool avoids, both of which produced a confidently wrong answer first time:

  * SAMPLING THE ENDPOINTS. The outage is ~1.7 s. Read the counters before and 5 s after and sync
    reads 2 at both ends, so the reset looks like it never happened.
  * MEASURING WHILE UNSYNCED. trigger_noise_edges counts intervals REJECTED by a LOCKED matcher.
    An unsynced ECU rejects nothing and reports zero noise however ugly the wire is, so a run that
    never establishes sync measures nothing at all and still prints a clean-looking zero.

    python3 -m tools.bench_dtr_noise
"""
import sys
import time

import serial

from tools.ts_bench import TsLink
from tools.gen_rebench import open_stim, select, configure, WHEELS
from tools.bench_fuel import set_fixed_rpm

PORT = '/dev/ttyUSB0'
POLL_S = 0.05
WATCH_S = 8.0
RUN_RPM = 1200
WHEEL_FALLBACK = 6       # 36-1, only if the stim will not say what it is on


def ask_wheel(sp):
    """Which wheel is the stim on right now? Single-byte 'N' query, index back on one line."""
    sp.reset_input_buffer()
    sp.write(b"N")
    time.sleep(0.2)
    try:
        return int(sp.readline().decode(errors="replace").strip())
    except Exception:
        return -1


def snap(l):
    e = l.telem_all()
    return dict(sync=e.get("sync_level", 0), noise=e.get("trigger_noise_edges", 0),
                missed=e.get("trigger_missed_teeth", 0), teeth=e.get("trigger_teeth", 0),
                rpm=e.get("rpm", 0))


def main():
    l = TsLink()

    # SET THE RIG UP RATHER THAN DEMANDING IT. This suite refuses to measure unsynced, and rightly so
    # — the docstring above explains that an unsynced ECU rejects nothing and reports a clean-looking
    # zero however ugly the wire is. But it used to require the CALLER to have left the rig on a
    # matching wheel, which no other trigger suite does, so in a full sweep it simply exited 1 every
    # time: a suite that had not run for as long as anyone could remember. Arrange the precondition
    # here, the way bench_distributor and bench_trigger_faults do.
    sp = open_stim()                      # handles the open's own DTR reset before we measure one

    # RUN ON THE WHEEL THE STIM COMES BACK AS, not one we choose. A DTR pulse reboots the Uno and it
    # powers up on its DEFAULT wheel — measured: select 6 (36-1), pulse DTR, and it returns on 4
    # (60-2 + cam) still spinning at the commanded rpm. Pick our own wheel and the ECU is configured
    # for a pattern the rig stops producing the moment we reset it, so sync drops and can NEVER come
    # back: the re-acquisition half of this measurement reads "None" and looks like a decoder that
    # cannot recover. Ask the stim what it is on and configure the ECU to match.
    idx = ask_wheel(sp)
    if idx not in WHEELS:
        print(f"  stim did not report a known wheel ({idx}); falling back to {WHEEL_FALLBACK}")
        idx = WHEEL_FALLBACK
        select(sp, idx, reps=6)
    print(f"  rig wheel {idx}: {WHEELS[idx][0]}")
    set_fixed_rpm(sp, RUN_RPM)
    configure(l, WHEELS[idx])
    l.execute("reconfig")
    ok, took, _ = l.wait_until(lambda f: f["sync_level"] >= 1, timeout=8.0)

    base = snap(l)
    print(f"settled: sync={base['sync']} rpm={base['rpm']} after {took:.2f}s "
          f"(lifetime totals noise={base['noise']} missed={base['missed']})")
    if base['sync'] == 0:
        print("  NOT SYNCED — the counters below would measure nothing. The rig did not lock onto "
              f"{WHEELS[idx][0]} at {RUN_RPM} rpm; check the trigger wiring to the ECU.")
        sp.close(); l.close(); return 1

    # An EXPLICIT pulse, not a reliance on the port open: whether an open resets the board depends
    # on hupcl and on how pyserial applies dtr, and quietly not resetting looks identical to a
    # reset that cost nothing.
    print(f"--- pulsing DTR (Uno reset), watching {WATCH_S:.0f}s at {POLL_S*1000:.0f}ms ---")
    sp.dtr = False; time.sleep(0.05); sp.dtr = True

    t0 = time.time()
    lost_at = relock_at = None
    prev = base
    while time.time() - t0 < WATCH_S:
        s = snap(l)
        if s['sync'] != prev['sync']:
            dt = time.time() - t0
            print(f"  t={dt:5.2f}s  sync {prev['sync']} -> {s['sync']}  "
                  f"noise +{s['noise']-base['noise']}  missed +{s['missed']-base['missed']}")
            if s['sync'] == 0 and lost_at is None: lost_at = dt
            if s['sync'] != 0 and lost_at is not None and relock_at is None: relock_at = dt
        prev = s
        time.sleep(POLL_S)

    end = snap(l)
    print(f"\ncost of one reset: noise +{end['noise']-base['noise']}  "
          f"missed +{end['missed']-base['missed']}")
    print(f"sync lost after {lost_at if lost_at is None else f'{lost_at*1000:.0f} ms'}, "
          f"re-acquired after {relock_at if relock_at is None else f'{relock_at:.2f} s'}")
    sp.close(); l.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
