#!/usr/bin/env python3
"""Bench rig: does async acceleration enrichment actually DELIVER, and can you see it?

TransientThrottle answers a tip-in two ways. The steady correction multiplies the NEXT sequential
injection (fuel_corr_accel). The ASYNC burst is different: extra all-injector squirts inserted
BETWEEN the sequential events, so the fuel arrives now rather than at the next intake. The module
half is unit-tested (it produces async_inj_pw_us/pulses); what has never been checked is whether the
scheduler actually fires them — and the engine-cycle capture is exactly the instrument for that,
because fire_async_pulse drives the injectors through the same drive_inj() the recorder hooks.

So this proves delivery by SEEING it: capture a cycle with no tip-in (baseline), then capture while
stepping the throttle, and require extra injector pulses to appear that were not there before.

    baseline   each injector opens ONCE per cycle (sequential)
    tip-in     the same injectors show ADDITIONAL squirts, all at the same angle
               (an async pulse opens every active injector simultaneously)

Usage:  python3 tools/bench_async_enrich.py [rpm]
"""
import sys
import time

from tools.ts_bench import TsLink
from tools.gen_rebench import open_stim, select, configure, WHEELS
from tools.bench_fuel import set_fixed_rpm, wait_for_sync, lua_script
from tools.bench_cycle import configure_firing

WHEEL_IDX = 6
DEFAULT_RPM = 1200

ASYNC_PULSES = 3          # squirts per burst; the scheduler spreads them across the cycle
HOLDOFF_MS = 0            # no rate limit, so a scripted tip-in always arms


def inj_pulse_counts(cap):
    """Injector -> number of OPEN edges in this capture."""
    out = {}
    for e in cap["edges"]:
        if e["signal"] == "Injector" and e["active"]:
            out[e["channel"]] = out.get(e["channel"], 0) + 1
    return out


def main():
    rpm = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_RPM
    fails = []

    def check(name, cond, detail):
        print(f"  [{'PASS' if cond else 'FAIL'}] {name}: {detail}")
        if not cond:
            fails.append(name)

    print(f"=== Bench: async acceleration enrichment, seen in the engine cycle @ {rpm} rpm ===")

    sp = open_stim()
    l = TsLink()
    configure(l, WHEELS[WHEEL_IDX])
    configure_firing(l)
    # Arm the async path. Without enable_async the module computes a correction and never asks the
    # scheduler for anything, so the capture would look identical and the test would prove nothing.
    l.set_config("transient_throttle_enabled", 1)
    l.set_config("transient_throttle_enable_async", 1)
    l.set_config("transient_throttle_max_async_pulses", ASYNC_PULSES)
    l.set_config("transient_throttle_async_holdoff_ms", HOLDOFF_MS)
    l.close()

    select(sp, WHEEL_IDX, reps=4)
    set_fixed_rpm(sp, rpm)

    l = TsLink()
    if not wait_for_sync(l):
        print("  [FAIL] sync: decoder never reached CRANK sync")
        l.close(); sp.close(); sys.exit(1)

    l.execute("key on")
    l.set_script(lua_script(tps=8.0))
    time.sleep(2.0)                       # settle: no tip-in in flight

    # ---- baseline: steady throttle, sequential injection only ----
    base = l.cycle_capture(timeout=5.0)
    base_counts = inj_pulse_counts(base)
    print(f"  baseline: {base['total']} edges, injector opens {base_counts}")
    check("baseline captured", base["state"] == "Complete" and bool(base_counts),
          f"{len(base_counts)} injector(s)")
    base_per_inj = max(base_counts.values()) if base_counts else 0

    # ---- tip-in: step the throttle and catch a cycle containing the burst ----
    # The burst is edge-triggered and fires within a few teeth of the request, so this steps the
    # throttle and re-arms repeatedly until a capture lands on one. Alternating the TPS gives a fresh
    # tip-in edge each round; without the alternation only the first step would ever arm.
    best, best_counts = None, None
    for attempt in range(12):
        l.set_script(lua_script(tps=8.0))
        time.sleep(0.25)
        l.cycle_arm()                      # arm FIRST, then tip in, so the burst lands inside
        l.set_script(lua_script(tps=60.0))
        try:
            cap = l.cycle_read()
            t0 = time.time()
            while cap["state"] != "Complete" and time.time() - t0 < 3.0:
                time.sleep(0.02)
                cap = l.cycle_read()
        except Exception:
            continue
        counts = inj_pulse_counts(cap)
        peak = max(counts.values()) if counts else 0
        print(f"    attempt {attempt}: injector opens {counts}")
        if peak > base_per_inj:
            best, best_counts = cap, counts
            break

    if best is None:
        check("async burst appears in the capture", False,
              f"no capture exceeded the baseline of {base_per_inj} open(s) per injector")
    else:
        peak = max(best_counts.values())
        check("async burst appears in the capture", peak > base_per_inj,
              f"{peak} injector opens vs baseline {base_per_inj}")

        # An async pulse opens EVERY active injector at once, so the extra opens should share an
        # angle across channels. Sequential squirts do not — they are 180 deg apart on a 4-cyl.
        extra_angles = {}
        for e in best["edges"]:
            if e["signal"] == "Injector" and e["active"]:
                extra_angles.setdefault(round(e["angle"], 0), set()).add(e["channel"])
        simultaneous = [a for a, ch in extra_angles.items() if len(ch) >= 2]
        check("a burst opens all injectors together", bool(simultaneous),
              f"angles with >=2 injectors opening: {sorted(simultaneous)[:6]}")

        check("capture not truncated", best["dropped"] == 0, f"dropped={best['dropped']}")

    # ---- clean up ----
    l.restore_script()
    l.execute("key auto")
    l.close(); sp.close()

    print(f"\n=== {'ALL PASS' if not fails else 'FAILED: ' + ', '.join(fails)} ===")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
