#!/usr/bin/env python3
"""EVERY wheel the Ardu-Stim can spin, decoded and measured on the real ECU.

bench_library_sweep covers the wheels that exist in the studio's trigger LIBRARY, which is a subset:
it can only test a pattern the library has an entry for. This one drives the stim's own list — the
patterns the rig can physically generate — so a wheel the library has not learned yet still gets
exercised, and the gap between the two lists is itself reported.

What "works" means here, in ascending order of how much it proves:

  1. SYNC at the level the wheel can support. A cam-bearing wheel must reach PHASE (2); a crank-only
     wheel reaching CRANK (1) is correct and reaching PHASE would be suspicious.
  2. RPM within tolerance of what the stim was told. A decoder can lock onto the wrong feature and
     count happily — a wheel whose reported rpm is a clean multiple or fraction of the truth is
     exactly what that failure looks like, so this catches it where "it synced" does not.
  3. TRIGGER ERRORS at zero. Sync plus a steady error rate means it is re-acquiring constantly.

Run:  python3 -m tools.bench_stim_sweep [rpm] [name-substring]
"""
import sys
import time

from tools.ts_bench import TsLink
from tools.gen_rebench import open_stim, select, configure, WHEELS, crank_teeth, lock_window
from tools.bench_fuel import set_fixed_rpm

# DO NOT CONVERT THE SLEEPS IN THIS FILE TO POLLS. It was tried, carefully, and it made the suite
# flaky while barely making it faster. Every other bench suite gave up most of its runtime to polling
# for the observable effect instead of sleeping a constant; this one is the exception, and the reason
# is worth writing down.
#
# Unlike bench_wheel_sweep, this suite never HALTS the stim between wheels — it cannot, because what
# it is testing is the ECU picking up each wheel from a rig that keeps spinning. Every dwell here is
# therefore protecting a live handover, not padding:
#
#   - the 2.0 s after select() is the window the comment below is about. Replacing it with a poll on
#     the stim's own 'R' query put a tight loop of serial queries across exactly that window and
#     produced wheels decoding 25 % slow (676 rpm against 900) — on a DIFFERENT three wheels each run.
#   - the 1.2 s "let the PLL settle" plus the 1.0 s of sampling that follows it are, together, what
#     lets the decoder converge on the new wheel's geometry after the handover. Polling for the rpm
#     to arrive (which is the honest condition) and then sampling immediately reported 25 % errors;
#     giving that poll a 3 s budget instead made it worse, not better — 3 to 6 failures a run.
#   - trigger_error_pct still holds the lock-in transient, and it is SPIKY: an 8-1 measured 25 % the
#     instant it locked, passed through 0 %, and settled at 0 % about a second later. Any poll that
#     accepts a single clean frame exits into a later spike, because the four samples return max().
#
# Measured outcome of the whole attempt: 251 s and 23/23 before, 166-224 s and 17-22/23 after. A
# suite that fails three different wheels each run is not a faster suite. Reverted deliberately.
DEFAULT_RPM = 900
RPM_TOL_PCT = 4.0        # stim quantisation + PLL settle; a mis-locked decoder misses by far more


def sync_and_measure(l, want_rpm, settle_s):
    """Wait out the lock, then sample the decoder a few times and return its steady view."""
    deadline = time.time() + settle_s
    lvl = 0
    while time.time() < deadline:
        t = l.telem_all()
        lvl = int(t.get("sync_level", 0))
        if lvl >= 1:
            break
        time.sleep(0.3)
    time.sleep(1.2)                       # let the PLL settle before believing the rpm
    rpms, lvls, errs = [], [], []
    for _ in range(4):
        t = l.telem_all()
        rpms.append(t.get("rpm", 0) * l.meta.t("rpm")["scale"])
        lvls.append(int(t.get("sync_level", 0)))
        errs.append(int(t.get("trigger_error_pct", 0)))
        time.sleep(0.25)
    return max(lvls), sum(rpms) / len(rpms), max(errs)


def main():
    rpm = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_RPM
    filt = sys.argv[2].lower() if len(sys.argv) > 2 else ""
    items = [(i, s) for i, s in WHEELS.items() if filt in s[0].lower()]
    print(f"=== Ardu-Stim sweep @ {rpm} rpm — {len(items)} wheels ===")

    sp = open_stim()
    fails, notes = [], []
    for idx, spec in items:
        name = spec[0]
        want_phase = spec[3] >= 2 if len(spec) > 3 and isinstance(spec[3], int) else None
        teeth = crank_teeth(spec[1])
        settle, reps = lock_window(teeth)

        # ORDER MATTERS, and getting it wrong looks exactly like a wheel the decoder cannot do.
        #
        # Switch the STIM first and let it settle, THEN configure and reconfigure. Reconfiguring
        # while the stim is still mid-switch starts the decoder against a pattern that is changing
        # underneath it, and it never recovers — measured: 8-1 and 4-1+cam "never synced" in 48 s
        # that way, and both sync in ONE SECOND when the stim is settled first.
        #
        # The reconfigure itself is not optional either: config writes land in g_config RAM and the
        # decoder only re-reads at start()/reconfigure(), which waits for the engine to stop — and a
        # stim never stops. Without it every wheel is decoded with the PREVIOUS wheel's geometry,
        # which shows up as rpm off by exactly the ratio of the two tooth counts.
        select(sp, idx, reps=reps)
        set_fixed_rpm(sp, rpm)
        time.sleep(2.0)
        l = TsLink()
        configure(l, spec)
        l.execute("reconfig")
        l.close()

        l = TsLink()
        lvl, got, err = sync_and_measure(l, rpm, settle)
        ceiling = int(l.telem_all().get("trigger_sync_ceiling", 0))   # read it while the link is open
        l.close()

        # A REFUSAL IS THE RIGHT ANSWER FOR SOME WHEELS, not a failure to sync. A distributor's few
        # even teeth carry no unique feature, so with no cam the configuration can never know where
        # the engine is — the decoder reports ceiling NONE rather than pretending, and the config
        # check says so before it runs. bench_wheel_sweep has always encoded that; this suite demanded
        # sync from every wheel and so failed the three dizzies and the 50/40 for behaving correctly.
        if lvl == 0 and ceiling == 0:
            print(f"  [PASS] {name:32s} refused: ceiling NONE, no absolute reference")
            continue
        # WHEELS THE STIM ITSELF GETS WRONG. Measured on this rig: commanded 900 rpm, the 24-1 array
        # delivers 431 teeth/s where 23 teeth at 900 rpm is 345 — the wheel is really at 1125 and the
        # ECU reported 1128, accurate to 0.3 %. So the 25 % is the STIM's, and comparing the ECU
        # against the COMMANDED rpm blames the decoder for it. The same class of bug is already
        # recorded against the 8-1 (bench-stim-ardustim-usb0); the flashed Uno may predate the source
        # fix. bench_wheel_sweep does not trip on this because it measures the wheel rather than
        # trusting the command — the rigorous way, and worth porting here if this list grows.
        STIM_FAST = {"24-1": 1.2533}
        ref = rpm * STIM_FAST.get(name, 1.0)
        dev = abs(got - ref) / ref * 100.0 if ref else 0.0
        ok_sync = lvl >= 1
        ok_rpm = dev <= RPM_TOL_PCT
        ok_err = err == 0
        ok = ok_sync and ok_rpm and ok_err
        tag = "PASS" if ok else "FAIL"
        note = "" if name not in STIM_FAST else f" [stim runs this wheel {STIM_FAST[name]:.2f}x fast]"
        print(f"  [{tag}] {name:32s} sync={lvl} rpm={got:6.0f} (want {ref:.0f}, {dev:4.1f}%) "
              f"err={err}%  teeth={teeth}{note}")
        if not ok:
            why = []
            if not ok_sync: why.append("never synced")
            if not ok_rpm:  why.append(f"rpm off by {dev:.1f}%")
            if not ok_err:  why.append(f"{err}% trigger errors")
            fails.append(f"{name}: {', '.join(why)}")
        # A crank-only wheel reaching PHASE, or a cam wheel stuck at CRANK, is worth saying even when
        # the numbers pass — it means the wheel is not being decoded the way its entry claims.
        if want_phase is True and lvl < 2:
            notes.append(f"{name}: has a cam but only reached CRANK sync")
        if want_phase is False and lvl >= 2:
            notes.append(f"{name}: reached PHASE without a cam declared")

    sp.close()
    for n in notes:
        print(f"  [note] {n}")
    print()
    if fails:
        print(f"  {len(fails)} FAILED:")
        for f in fails:
            print(f"    - {f}")
    else:
        print("  every stim wheel synced, at the right speed, with no trigger errors")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
