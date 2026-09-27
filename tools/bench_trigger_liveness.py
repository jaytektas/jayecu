#!/usr/bin/env python3
"""Does the ECU notice when the trigger STOPS? On the real rig, against real silicon.

The failure this exists to prevent was measured, not imagined: a bench ECU held sync_level=PHASE,
rpm=1348 and engine_state=RUNNING for forty minutes with no trigger signal at all, because the only
stall detector in the firmware lived inside the firing clock and every sync transition reset that
clock. The decoder now owns a deadline of its own (TIM5 CCR4), armed from its own prediction of when
the next tooth is due.

The host test (tests/test_trigger_liveness.cpp) proves the logic with a fake alarm. This proves the
part a fake alarm cannot: that the capture stamp and the alarm really do read the same counter on
real hardware, that a running engine does not false-trip, and that the timing is what we claim.

    python3 -m tools.bench_trigger_liveness [rpm]

Needs the stim on /dev/ttyUSB0 wired to the configured crank pin, and the ECU already configured for
the wheel the rig is spinning (this does not reconfigure the ECU — it measures what is there).
"""
import sys
import time

from tools.ts_bench import TsLink
from tools.gen_rebench import open_stim, stim_halt, stim_resume, select, configure, WHEELS
from tools.bench_fuel import set_fixed_rpm

DEFAULT_RPM = 1200
# 60-2 + cam. Both ends are set by this script: open_stim() DTR-resets the Uno, which puts it back
# on its default wheel, so a script that only configured the ECU would be testing one wheel against
# another and reporting "no sync" as if the ECU were at fault.
WHEEL_IDX = 4
# One crank revolution at the 30 rpm floor is 2 s, and that is the longest silence the decoder ever
# tolerates whatever the wheel. Allow a little over it for the poll interval and the USB round trip.
ABSENT_DEADLINE_S = 3.0

fails = []


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}: {detail}")
    if not cond:
        fails.append(name)


def wait_for(l, pred, timeout_s, poll=0.02):
    """Poll telemetry until pred(frame) or timeout. Returns (frame, elapsed, ok)."""
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        e = l.telem_all()
        if pred(e):
            return e, time.time() - t0, True
        time.sleep(poll)
    return l.telem_all(), time.time() - t0, False


def synced(e):   return e.get("sync_level", 0) >= 1 and e.get("rpm", 0) > 0
def unsynced(e): return e.get("sync_level", 0) == 0


def main():
    rpm = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_RPM
    print(f"=== Trigger liveness on the rig @ {rpm} rpm ===")

    sp = open_stim()          # NB: opening DTR-resets the Uno; open_stim waits out the bootloader
    select(sp, WHEEL_IDX)
    stim_resume(sp)           # a previous run may have left it halted
    set_fixed_rpm(sp, rpm)
    l = TsLink()
    print("  ecu:", l.hello().strip()[:70])
    print(f"  wheel: {WHEELS[WHEEL_IDX][0]}")
    configure(l, WHEELS[WHEEL_IDX])
    l.execute("reconfig")
    time.sleep(2.0)

    e, t, ok = wait_for(l, synced, 20.0)
    check("acquires sync", ok, f"sync={e.get('sync_level')} rpm={e.get('rpm')} after {t:.1f}s")
    if not ok:
        print("  cannot test absence without sync first — is the stim wired to the configured pin?")
        return 1

    # ---- A running engine must not false-trip -------------------------------------------------
    base_noise  = e.get("trigger_noise_edges", 0)
    base_missed = e.get("trigger_missed_teeth", 0)
    t0 = time.time()
    while time.time() - t0 < 5.0:
        f = l.telem_all()
        if f.get("trigger_absent"):
            break
        time.sleep(0.05)
    f = l.telem_all()
    check("no false absence while running", not f.get("trigger_absent"),
          "5 s at speed, trigger_absent stayed 0")
    check("no phantom missed teeth", f.get("trigger_missed_teeth", 0) == base_missed,
          f"missed={f.get('trigger_missed_teeth')} (unchanged)")
    check("no phantom noise edges", f.get("trigger_noise_edges", 0) == base_noise,
          f"noise={f.get('trigger_noise_edges')} (unchanged)")

    # ---- THE HEADLINE: teeth stop, and the ECU notices ----------------------------------------
    teeth_at_halt = l.telem_all().get("trigger_teeth", 0)
    stim_halt(sp)
    e, t, ok = wait_for(l, unsynced, ABSENT_DEADLINE_S + 2.0)
    check("sync drops when the teeth stop", ok, f"sync_level -> {e.get('sync_level')} after {t:.2f}s")
    check("...within the floor bound", ok and t <= ABSENT_DEADLINE_S,
          f"{t:.2f}s (bound {ABSENT_DEADLINE_S}s)")
    check("rpm goes to zero", e.get("rpm", -1) == 0, f"rpm={e.get('rpm')}")
    check("engine state is STOPPED", e.get("engine_state", -1) == 0, f"state={e.get('engine_state')}")
    # At the instant of the cut a missing tooth and a dead wire are indistinguishable — only time
    # separates them, so the diagnosis upgrades afterwards. The CUT does not wait for it.
    check("the cut is prompt, not floor-bound", t <= 0.25,
          f"{t*1000:.0f} ms (a fixed 2 s floor was 142 revolutions at 5000 rpm)")
    e2, t2, ok2 = wait_for(l, lambda f: f.get("trigger_absent") == 1, ABSENT_DEADLINE_S + 2.0)
    check("...and the diagnosis upgrades to ABSENT", ok2, f"trigger_absent=1 after {t+t2:.2f}s")
    # The teeth must have stopped ADVANCING — not "emitted nothing after the halt command". A stim
    # halting mid-pattern lets a few edges out, so asserting zero would be asserting something
    # about the stim rather than about the ECU. (An earlier version of this comment claimed a DTR
    # reset throws "1730 spurious edges"; it does not. That number was a free-running LIFETIME
    # total read as if it were a per-event delta, and it was the missed counter, not the noise one.
    # Measured properly — explicit DTR pulse, telemetry polled at 50 ms — a reset costs missed +1
    # and noise +0..1 over two runs: sync drops within ~56 ms and re-acquires 1.7 s later. The rig
    # is not the noisy environment that number was quoted for. tools/bench_dtr_noise.py.)
    s1 = l.telem_all().get("trigger_teeth", 0)
    time.sleep(0.5)
    s2 = l.telem_all().get("trigger_teeth", 0)
    check("and the teeth really had stopped", s1 == s2,
          f"teeth static at {s2} (was {teeth_at_halt} when halted)")

    # ---- It recovers, and it works a SECOND time ----------------------------------------------
    stim_resume(sp)
    set_fixed_rpm(sp, rpm)
    e, t, ok = wait_for(l, synced, 20.0)
    check("re-acquires from the next teeth", ok, f"sync={e.get('sync_level')} after {t:.1f}s")
    check("the absence flag heals", e.get("trigger_absent") == 0,
          f"trigger_absent={e.get('trigger_absent')}")

    stim_halt(sp)
    e, t, ok = wait_for(l, unsynced, ABSENT_DEADLINE_S + 2.0)
    check("catches it a SECOND time (the deadline re-armed)", ok and t <= ABSENT_DEADLINE_S,
          f"sync_level -> {e.get('sync_level')} after {t*1000:.0f} ms")
    stim_resume(sp)
    set_fixed_rpm(sp, rpm)
    wait_for(l, synced, 20.0)

    # ---- A hard decel must not read as an absence ---------------------------------------------
    # The deadline is 1.5x the decoder's own prediction, so a stretching tooth gap has to stretch by
    # half again before it counts as overdue. This is the case that would make the fix worse than
    # the bug: an engine that cuts on every throttle lift.
    before = l.telem_all()
    tripped = False
    for r in (2400, 1800, 1400, 1000, 700, 500, 400):
        set_fixed_rpm(sp, r)
        time.sleep(0.25)
        f = l.telem_all()
        if f.get("trigger_absent") or f.get("sync_level", 0) == 0:
            tripped = True
            print(f"    tripped at {r} rpm: sync={f.get('sync_level')} absent={f.get('trigger_absent')}")
            break
    check("a hard decel sweep recovers sync at every step", not tripped, "2400 -> 400 rpm in steps")
    f = l.telem_all()
    # NOT asserted to be zero, because on this rig it is not, and pretending otherwise would hide
    # the one number that decides whether the strict window is tuned right. Each step here is an
    # INSTANT rpm command — a bigger per-tooth change than any engine makes — and a step larger than
    # window_pct trips the window by design. Recorded so a real engine's number can be compared
    # against it; window_pct is per stream, and cranking is where it may need opening up.
    print(f"    decel-sweep missed teeth: "
          f"+{f.get('trigger_missed_teeth',0) - before.get('trigger_missed_teeth',0)} "
          f"(instant steps of 25-40% per tooth; window_pct=25)")

    set_fixed_rpm(sp, rpm)
    l.close(); sp.close()
    print(f"\n{'ALL PASS' if not fails else 'FAILED: ' + ', '.join(fails)}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
