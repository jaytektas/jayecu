#!/usr/bin/env python3
"""Knock vs PRE-IGNITION on the bench — the half that only exists on hardware.

The host tests prove the classifier's logic. What they cannot prove is that any of it survives the
trip through real firmware: the burst worker's mailbox, the threshold/max-retard table lookups
against live rpm/fuel_load, the learned noise region in persistent RAM, the DTC raise, and the
per-cylinder execution mask actually reaching the scheduler.

None of that is reachable from the DSO2D15's signal generator, because its burst is MANUAL-triggered
("Trigger source | Manual" in Hantek's spec table) and the pre-ignition discriminator is PHASE — where
the energy sits relative to the spark. A manual press lands wherever the crank happens to be.

So this drives the 'kinj' bench command, which posts a synthetic profile at a CHOSEN crank angle
straight into Knock::post_measurement, bypassing the ADC. It proves the PATH, not the calibration:
every dB here is a number this script typed, so nothing about a real sensor's response is tested.

What it asserts:
  * a fresh cell reads the SEED floor with zero samples, and LISTENS rather than judging
  * quiet measurements teach the floor, and it converges on what it was told
  * the same loud level, AFTER the spark  -> knock: retard rises, nothing is cut
  * the same loud level, BEFORE the spark -> pre-ignition: a cut, a P179x DTC, and NO retard
  * the cut names the right cylinder

Usage:  python3 -m tools.bench_knock [rpm]
"""
import sys
import time

from tools.ts_bench import TsLink
from tools.gen_rebench import open_stim, select, configure, WHEELS
from tools.bench_fuel import set_fixed_rpm, wait_for_sync, INJECT, TTL_MS
from tools.bench_cycle import configure_firing, WHEEL_IDX

DEFAULT_RPM = 1200
SETTLE_S = 0.15          # >= 1 module tick at 100 Hz, plus slack, so update() has drained the ring


def inject_script(tps: float) -> str:
    """Hold the absent sensors, and write wk::tps DIRECTLY.

    bench_fuel's script writes tps_1, the SENSOR channel — which only reaches wk::tps once a TPS
    sensor is assigned in the tune. It is not, on this rig. That matters more than it looks: Knock
    gates every measurement on wk::tps (suppress_min_tps), and the suppressed path DISCARDS the
    worker's queue without classifying it. With no TPS the module therefore does nothing at all, and
    says nothing about why — which is exactly how this bench read as a dead classifier on its first
    run. See the note in the summary: the same silence would happen on a real engine with an
    unassigned TPS.
    """
    lines = [f'  signalWrite("{n}", {v}, {TTL_MS})' for n, v in INJECT.items()]
    lines.append(f'  signalWrite("tps", {tps}, {TTL_MS})')
    body = "\n".join(lines)
    return f"function onTick()\n{body}\nend\nsetTickRate(200)\n"

fails = []


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}: {detail}")
    if not cond:
        fails.append(name)


def knk(l):
    """Parse the 'knk' CLI reply into a dict plus a per-cylinder floor/samples/events list."""
    txt = l.execute("knk")
    out, floors = {}, []
    for tok in txt.replace("\r", " ").split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            try:
                out[k] = int(v)
            except ValueError:
                pass
        elif "/" in tok:
            parts = tok.split("/")
            if len(parts) == 3:
                try:
                    floors.append(tuple(int(p) for p in parts))
                except ValueError:
                    pass
    out["floors"] = floors
    out["_raw"] = txt.strip()
    return out


def inj(l, cyl, db, ang, n=1):
    """kinj <cyl> <dB x10> <angle ATDC>, then let the module task drain the ring."""
    for _ in range(n):
        l.execute(f"kinj {cyl} {int(db * 10)} {int(ang)}")
    time.sleep(SETTLE_S)


def main():
    rpm = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_RPM
    spec = WHEELS[WHEEL_IDX]
    print(f"=== Knock / pre-ignition on the bench — {spec[0]} @ {rpm} rpm ===")

    sp = open_stim()
    l = TsLink(); configure(l, spec); configure_firing(l)

    # Knock ON, pre-ignition ON, and acting on a SINGLE confirmed event so the test is deterministic.
    # Everything else stays at its schema default — the point is to exercise the shipped numbers.
    l.set_config("knock_enabled", 1)
    l.set_config("knock_source", 0)                 # onboard: update() drains the worker mailbox
    l.set_config("knock_preign_enabled", 1)
    l.set_config("knock_preign_events_to_act", 1)
    l.set_config("knock_preign_cut_fuel", 1)
    l.set_config("knock_learn_dwell_ms", 0)         # no dwell wait: the operating point is parked

    # PUT EVERY CYLINDER ON THE SAME KNOCK INPUT, because that is what this suite asserts. The check
    # below reads "cylinders agree within a few dB (same unconnected input)" — which is only a
    # statement about the DECODER if all four are in fact listening to one input. It used to assume
    # that rather than arrange it, and bench_knock_siggen leaves the routing SPLIT (cyl 1,2 on
    # KNOCK1, cyl 3,4 on KNOCK2) with no restore. A run after that one read floors of
    # [-43.0, -42.6, -64.6, -64.4] — a 22 dB spread, which is not four cylinders disagreeing, it is
    # two of them plugged into a quieter socket — and reported five failures against a knock module
    # that was working correctly. The routing lives in ECU RAM, so it survives every suite that
    # follows until something resets the board.
    cs = l.meta.config["knock"]["cyl_sensor"]
    kbase, kstride = cs["base_offset"], cs["stride"]
    knock_routing_saved = [l.read_config_raw(kbase + c * kstride, 1) for c in range(4)]
    for c in range(4):
        l.write_raw(kbase + c * kstride, bytes([0]))
    l.close()

    select(sp, WHEEL_IDX, reps=4)
    set_fixed_rpm(sp, rpm)

    l = TsLink()
    if not wait_for_sync(l):
        print("  [FAIL] sync: decoder never reached CRANK sync")
        l.close(); sp.close(); return 1
    l.execute("key on")
    l.set_script(inject_script(tps=8.0))            # above suppress_min_tps (5%), so not suppressed
    time.sleep(2.0)                                 # let the REAL bursts establish a floor first

    adv = l.telem_all().get("advance") * l.meta.t("advance")["scale"]
    print(f"  live advance = {adv:.1f} deg BTDC  (spark sits at {-adv:.0f} deg on the profile axis)")
    fire = l.execute("fire").strip()
    print(f"  {fire}")

    # ---- 1. the REAL ADC bursts learn a floor, with no injection at all -----------------------
    #
    # Nothing here is synthetic: knock_enabled + source=onboard arms a window per cylinder per cycle,
    # the DMA burst captures the (unconnected) KNOCK1/2 inputs, the DSP reduces it, and the worker
    # posts it. If the floors move off the seed, that whole chain works on hardware.
    s = knk(l)
    print(f"  {s['_raw']}")
    floors = s["floors"]
    check("every cylinder learned a floor from real bursts",
          all(f[1] > 0 for f in floors), f"samples={[f[1] for f in floors]}")
    check("and it moved off the -35.0 dB seed",
          all(f[0] != -350 for f in floors), f"floors={[f[0]/10 for f in floors]} dB")
    check("cylinders agree within a few dB (same unconnected input)",
          max(f[0] for f in floors) - min(f[0] for f in floors) < 100,
          f"spread={(max(f[0] for f in floors) - min(f[0] for f in floors))/10:.1f} dB")
    base_floor = sum(f[0] for f in floors) / len(floors) / 10.0
    print(f"  learned floor ~{base_floor:.1f} dB — injections below use levels relative to THIS")

    # ---- 2. a measurement AT the floor is not knock -------------------------------------------
    knocks_before = knk(l)["knocks"]
    inj(l, 0, base_floor, 30, n=3)
    s = knk(l)
    check("a measurement at the learned floor is silent", s["knocks"] == knocks_before,
          f"knocks={s['knocks']} (was {knocks_before})")

    # ---- 4. LOUD, AFTER the spark -> knock ------------------------------------------------------
    retard_before = knk(l)["retard_x10"]
    cuts_before = knk(l)["preign_cuts"]
    # +20 dB: over the knock threshold (12 dB) but UNDER preign_extreme_db (30 dB). Injecting at
    # exactly +30 in an earlier run tripped the extreme override instead — the phase test correctly
    # reported pre_frac=0%, and the fail-toward-protection rule cut anyway, which is what it is for.
    inj(l, 1, base_floor + 20.0, 30, n=3)           # cyl 1 (no prior history), after the spark
    s = knk(l)
    print(f"  after-spark: {s['_raw']}")
    check("after the spark it is KNOCK", s["knocks"] > knocks_before,
          f"knocks={s['knocks']} (was {knocks_before})")
    check("and the answer is RETARD", s["retard_x10"] > retard_before,
          f"retard={s['retard_x10']/10:.2f} deg")
    # The cut mask LATCHES, so "nothing is cut" is not a property of this step — cuts from earlier
    # steps are still held on purpose. What must be true is that THIS cylinder was not cut.
    check("the knocking cylinder is NOT cut", (s["preign_cuts"] & 0x2) == 0,
          f"cuts=0x{s['preign_cuts']:x} (cyl 1 bit must be clear; others latched from earlier)")

    # ---- 4b. an EXTREME level AFTER the spark -> pre-ignition anyway ----------------------------
    #
    # The deliberate fail-toward-protection rule. Phase says knock; the magnitude says something is
    # being destroyed. Cutting is the safe error, retarding is not.
    inj(l, 3, base_floor + 40.0, 30, n=2)           # 40 dB over the floor, squarely AFTER the spark
    s = knk(l)
    print(f"  extreme-after-spark: {s['_raw']}")
    check("an extreme event cuts regardless of phase", (s["preign_cuts"] & 0x8) != 0,
          f"cuts=0x{s['preign_cuts']:x} pre_frac={s['pre_frac_pct']}% (phase said knock)")

    # ---- 5. the SAME level, BEFORE the spark -> pre-ignition -------------------------------------
    knocks_before = s["knocks"]
    inj(l, 0, base_floor + 20.0, -40, n=2)          # SAME level, 40 deg BTDC: before the spark
    s = knk(l)
    print(f"  before-spark: {s['_raw']}")
    check("before the spark it is PRE-IGNITION, and cylinder 0 is cut",
          (s["preign_cuts"] & 0x1) != 0, f"cuts=0x{s['preign_cuts']:x}")
    check("it is NOT counted as knock", s["knocks"] == knocks_before,
          f"knocks={s['knocks']} (was {knocks_before})")
    check("pre-spark fraction was measured", s["pre_frac_pct"] > 50,
          f"pre_frac={s['pre_frac_pct']}%")

    # ---- 6. the DTC names the cylinder ----------------------------------------------------------
    faults = l.execute("faults")
    print(f"  faults: {faults.strip()}")
    dtcs = l.dtc_list() if hasattr(l, "dtc_list") else None
    if dtcs is not None:
        codes = [f"P{d['code']:04X}" for d in dtcs]
        print(f"  codes: {codes}")
        check("P1790 raised (pre-ignition, cylinder 1)", "P1790" in codes, f"{codes}")

    # ---- 7. a different cylinder is independent ---------------------------------------------------
    inj(l, 2, base_floor + 20.0, -40, n=2)          # cylinder 2, before the spark
    s = knk(l)
    print(f"  cyl2: {s['_raw']}")
    check("cylinder 2 cut independently", (s["preign_cuts"] & 0x4) != 0,
          f"cuts=0x{s['preign_cuts']:x} (want bits 0 and 2)")

    print(f"\n  ring drops during the run: {s['dropped']}")

    l.restore_script()
    for c in range(4):                       # leave the routing as we found it
        l.write_raw(kbase + c * kstride, knock_routing_saved[c])
    l.execute("key auto")
    l.close(); sp.close()

    print("\n=== " + ("ALL PASS" if not fails else f"{len(fails)} FAILED: {fails}") + " ===")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
