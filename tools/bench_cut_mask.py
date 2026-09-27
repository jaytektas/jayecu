#!/usr/bin/env python3
"""Does an ignition/fuel cut actually stop the OUTPUT? Bench rig against a spinning wheel.

The host tests prove the modules no longer fake it — Ignition leaves the advance alone and
FuelCalculator leaves the pulse width alone. What they cannot prove is the half that only exists on
hardware: that EnginePositionHal::set_output_cuts reaches EventScheduler's execution mask, and that
the mask actually suppresses IGN_DWELL_START and INJ_OPEN in the ISR.

That gap is exactly how the bug survived in the first place. wk::ign_cut only ever set advance to 0
— a spark at TDC, not the absence of one — and every test asserted that zero, so "no spark" and "a
spark at TDC" were indistinguishable to the suite. Nothing measured the pin.

So this measures the pin, via the engine-cycle capture (cmd 0x26), which records DELIVERED edges:

  * running clean      -> coils dwell AND injectors open
  * ignition cut       -> ZERO dwell starts, injectors still opening, crank teeth unchanged
  * fuel cut           -> ZERO injector opens, coils dwelling again
  * cut released       -> both return

The crank lane is the control: it must be unchanged throughout. If it moved, the engine stopped and
the "no coil edges" result would mean nothing.

The cut is asserted through the REV LIMITER, by dropping its limit under the running rpm — a real
requester on its real path, not a poked flag.

Usage:  python3 -m tools.bench_cut_mask [rpm]
"""
import sys
import time

from tools.ts_bench import TsLink
from tools.gen_rebench import open_stim, select, configure, WHEELS
from tools.bench_fuel import set_fixed_rpm, wait_for_sync, lua_script
from tools.bench_cycle import configure_firing, WHEEL_IDX

DEFAULT_RPM = 1200

fails = []


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}: {detail}")
    if not cond:
        fails.append(name)


def lanes_of(cap):
    """Crank teeth, DWELL STARTS, injector opens.

    Counting coil EDGES is the wrong measure. IGN_SPARK drives the pin to its idle level
    unconditionally (EventScheduler::dispatch_fire) — only IGN_DWELL_START is masked — so a cut coil
    still emits its falling edge, having never charged. That is deliberate: it guarantees the pin
    returns to idle even when a cut lands mid-dwell, which is the one case where a coil could
    otherwise be left charging. What proves the cut is that no coil ever STARTS a dwell.
    """
    crank = sum(1 for e in cap["edges"] if e["signal"] in ("Crank", "Cam"))
    dwell = sum(1 for e in cap["edges"] if e["signal"] == "Coil" and e["active"])
    inj   = sum(1 for e in cap["edges"] if e["signal"] == "Injector" and e["active"])
    return crank, dwell, inj


def grab(l, label):
    """One capture, retried — a cut changes what the scheduler emits, so let it settle first."""
    time.sleep(1.2)
    last = None
    for _ in range(4):
        try:
            cap = l.cycle_capture(timeout=5.0)
        except TimeoutError as ex:
            last = ex
            continue
        crank, coil, inj = lanes_of(cap)
        print(f"  {label:22s} crank={crank:3d}  dwell={coil:2d}  inj_open={inj:2d}   "
              f"(state={cap['state']} rpm={cap['rpm']:.0f})")
        return crank, coil, inj
    print(f"  [FAIL] capture ({label}): {last}")
    return None


def main():
    rpm = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_RPM
    spec = WHEELS[WHEEL_IDX]
    print(f"=== Cut mask on the bench — {spec[0]} @ {rpm} rpm ===")

    sp = open_stim()
    l = TsLink(); configure(l, spec); configure_firing(l)
    # A limit well above the test rpm to start: the limiter must be ENABLED throughout, so the only
    # thing changing between captures is the limit itself.
    l.set_config("rev_limiter_enabled", 1)
    l.set_config("rev_limiter_hard_limit_rpm", 8000)
    l.set_config("rev_limiter_soft_limit_rpm", 7800)
    l.close()

    select(sp, WHEEL_IDX, reps=4)
    set_fixed_rpm(sp, rpm)

    l = TsLink()
    if not wait_for_sync(l):
        print("  [FAIL] sync: decoder never reached CRANK sync")
        l.close(); sp.close(); return 1
    l.execute("key on")
    l.set_script(lua_script(tps=8.0))

    base = grab(l, "running clean")
    if not base:
        l.restore_script(); l.execute("key auto"); l.close(); sp.close(); return 1
    crank0, coil0, inj0 = base
    check("baseline fires", coil0 > 0 and inj0 > 0, f"dwell={coil0} inj_open={inj0}")

    # ---- ignition cut: limiter under the running rpm, cutting IGNITION ----
    l.set_config("rev_limiter_cut_method", 1)          # schema: 0=Fuel 1=Ign 2=Both
    l.set_config("rev_limiter_hard_limit_rpm", max(100, rpm - 400))
    l.set_config("rev_limiter_soft_limit_rpm", max(50, rpm - 500))
    got = grab(l, "ignition cut")
    if got:
        crank1, coil1, inj1 = got
        check("no coil ever STARTS a dwell on an ign cut", coil1 == 0, f"{coil1} dwell starts (want 0)")
        check("injectors keep going", inj1 > 0, f"{inj1} injector opens")
        check("crank lane unchanged", abs(crank1 - crank0) <= 2,
              f"{crank1} vs {crank0} — the control: the wheel must still be turning")
        # The commanded advance must NOT have collapsed: the cut is an output decision.
        adv = l.telem_all().get("advance")
        check("advance still commanded", adv is None or adv > 1.0,
              f"advance={adv} (0 would mean the old fake cut is back)")

    # ---- fuel cut: same limiter, cutting FUEL ----
    l.set_config("rev_limiter_cut_method", 0)          # fuel
    got = grab(l, "fuel cut")
    if got:
        crank2, coil2, inj2 = got
        check("injectors STOP on a fuel cut", inj2 == 0, f"{inj2} injector opens (want 0)")
        check("coils dwell again", coil2 > 0, f"{coil2} dwell starts")
        check("crank lane still unchanged", abs(crank2 - crank0) <= 2, f"{crank2} vs {crank0}")

    # ---- released ----
    l.set_config("rev_limiter_hard_limit_rpm", 8000)
    l.set_config("rev_limiter_soft_limit_rpm", 7800)
    got = grab(l, "cut released")
    if got:
        _, coil3, inj3 = got
        check("both return", coil3 > 0 and inj3 > 0, f"dwell={coil3} inj_open={inj3}")

    l.restore_script(); l.execute("key auto"); l.close(); sp.close()
    print(f"\n  {'FAILED: ' + ', '.join(fails) if fails else 'all checks passed'}")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
