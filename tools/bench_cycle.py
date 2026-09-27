#!/usr/bin/env python3
"""Bench rig for the engine-cycle capture — validate it against a SPINNING wheel.

The host test (tests/test_cycle_recorder.cpp) proves the buffer, the gate and the paging. What it
cannot prove is the part that only exists on hardware: that the angles recorded are the angles the
scheduler actually fired at, on a real decoder, at a real rpm, with the ISRs preempting each other.

So this rig spins a 36-1 wheel on the stim, injects the absent analog sensors so the fuel/ignition
chain actually computes and commits, then captures one cycle and checks it against what the ECU says
it intended:

  * the capture completes and is boundary-aligned (starts at 0, spans the configured cycle)
  * the trigger lane carries the wheel's teeth, at the tooth pitch the wheel implies
  * every coil lane forms dwell/spark PAIRS — a rise then a fall, not orphan edges
  * the recorded spark angle matches the ignition table's commanded advance
  * a re-arm gives a FRESH cycle, and the rpm in the header is the rpm at capture

Usage:  python3 tools/bench_cycle.py [rpm]      (default 1200 rpm, 36-1 wheel)
"""
import sys, time, struct

from tools.ts_bench import TsLink
from tools.gen_rebench import open_stim, select, configure, WHEELS
from tools.bench_fuel import set_fixed_rpm, wait_for_sync, lua_script
from tools.output_rows import write_engine_rows, SEMI_SEQUENTIAL

WHEEL_IDX = 6            # '36-1' — dense, single crank stream, locks quickly
DEFAULT_RPM = 1200

# A 4-cylinder four-stroke firing 1-3-4-2 (cyl1 fires 1st, cyl3 2nd, cyl4 3rd, cyl2 4th). The tune
# stores the firing ORDER; the ECU computes the even-fire TDCs (180 deg apart). Which coil and injector
# serve each cylinder are OUTPUT ROWS (tools/output_rows.py). Cylinder index -> firing position (1-based).
FIRE_POS = [1, 4, 2, 3]

# What the wasted-spark rows written below bind, for the per-coil TDC check: each cylinder's even-fire
# TDC = (fire_pos-1) x (720/N) deg. The rows put IGN1 on cylinder 1 and IGN2 on cylinder 2, and the ECU
# fires each coil's companion too: on 1-3-4-2 that is IGN1 = 1+4, IGN2 = 2+3, which for this order is
# (fire_pos-1) mod (N/2). (tdc_deg10, coil_channel, cyl_index) per cylinder.
_N = len(FIRE_POS)
FIRING = [((fp - 1) * (7200 // _N), (fp - 1) % (_N // 2), i) for i, fp in enumerate(FIRE_POS)]


def configure_firing(l: TsLink, coils="wasted"):
    """Set the firing order, the injection stage and the coil + injector OUTPUT ROWS.

    The shipped default tune has an empty firing order and no coil or injector rows, so the scheduler
    correctly drives nothing — a capture against it records an empty output lane and would make this
    rig assert on a state it never established. Wasted spark + semi-sequential so CRANK-only sync is
    enough; a COP / full-sequential map would need a cam the 36-1 stim does not provide.
    """
    l.set_config("engine_cylinder_count", len(FIRE_POS))
    l.set_config("engine_num_inj_stages", 1)
    # Primary injection stage: semi-sequential (once per crank revolution).
    l.write_raw(l.meta.array_offset("engine", "inj_stage", 0, "mode"), bytes([1]))  # SEMI_SEQUENTIAL
    l.write_raw(l.meta.array_offset("engine", "inj_stage", 0, "injections_per_cycle"), bytes([2]))
    # The firing order is stored as firing_order[position].cyl = the cylinder that fires there (1-based).
    # FIRE_POS is cylinder->position, so invert it.
    firing_order = [0] * 12
    for cyl0, pos in enumerate(FIRE_POS):
        firing_order[pos - 1] = cyl0 + 1
    for i, cyl in enumerate(firing_order):
        l.write_raw(l.meta.array_offset("engine", "firing_order", i, "cyl"), bytes([cyl]))
    # One coil row per firing pair (or per cylinder for coil-on-plug), one injector row per cylinder.
    write_engine_rows(l, [c for c in firing_order if c], coils=coils, stages=[(SEMI_SEQUENTIAL, None)])
    l.execute("reconfig")


def main():
    rpm = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_RPM
    spec = WHEELS[WHEEL_IDX]
    fails = []

    def check(name, cond, detail):
        print(f"  [{'PASS' if cond else 'FAIL'}] {name}: {detail}")
        if not cond:
            fails.append(name)

    print(f"=== Bench engine-cycle capture — {spec[0]} @ {rpm} rpm ===")

    # ---- stim up ----
    sp = open_stim()
    l = TsLink(); configure(l, spec); configure_firing(l); l.close()
    select(sp, WHEEL_IDX, reps=4)
    set_fixed_rpm(sp, rpm)

    l = TsLink()
    e = wait_for_sync(l)
    if not e:
        print("  [FAIL] sync: decoder never reached CRANK sync — is the stim wired to the crank input?")
        l.close(); sp.close(); sys.exit(1)
    print(f"  synced: sync_level={e['sync_level']} rpm={e['rpm']:.0f}")

    # Key on + inject sensors, so the scheduler is actually committing spark and fuel rather than
    # sitting with nothing armed. A capture of an idle scheduler would pass an "it didn't crash"
    # test while proving nothing about the angles.
    l.execute("key on")
    l.set_script(lua_script(tps=8.0))
    time.sleep(1.5)

    # ---- a capture with the engine turning ----
    try:
        cap = l.cycle_capture(timeout=5.0)
    except TimeoutError as ex:
        print(f"  [FAIL] capture: {ex}")
        l.restore_script(); l.execute("key auto"); l.close(); sp.close(); sys.exit(1)

    print(f"  capture: state={cap['state']} span={cap['cycle_angle']:.0f}deg edges={cap['total']} "
          f"rpm={cap['rpm']:.0f} sync={cap['sync_level']} dropped={cap['dropped']}")

    lanes = {}
    for ed in cap["edges"]:
        lanes.setdefault((ed["signal"], ed["channel"]), []).append(ed)
    for k in lanes:
        lanes[k].sort(key=lambda x: x["angle"])
    print("  lanes: " + ", ".join(f"{s}{c}:{len(v)}" for (s, c), v in sorted(lanes.items())))

    check("completed", cap["state"] == "Complete", cap["state"])
    check("not truncated", cap["dropped"] == 0,
          f"dropped={cap['dropped']} (non-zero means a ring filled)")
    check("header rpm tracks the stim", abs(cap["rpm"] - rpm) < rpm * 0.15,
          f"header {cap['rpm']:.0f} vs stim {rpm}")
    check("every angle inside the span",
          all(0 <= ed["angle"] < cap["cycle_angle"] for ed in cap["edges"]),
          f"span 0..{cap['cycle_angle']:.0f}")

    # ---- the trigger lane: teeth at the wheel's pitch ----
    trig = [ed for ed in cap["edges"] if ed["signal"] in ("Crank", "Cam")]
    check("trigger lane present", len(trig) > 10, f"{len(trig)} tooth edges")
    if len(trig) > 4:
        ang = sorted(ed["angle"] for ed in trig)
        gaps = [round(b - a, 1) for a, b in zip(ang, ang[1:])]
        gaps_sorted = sorted(gaps)
        median = gaps_sorted[len(gaps_sorted) // 2]
        # A 36-1 wheel is a 10 deg pitch. The recorded angles come from the PLL, so the common gap
        # must land on it — if the capture were stamping tick counts or a stale angle, this is where
        # it would show up as noise instead of a pitch.
        check("tooth pitch matches the wheel", 8.0 <= median <= 12.0,
              f"median gap {median} deg (36-1 => 10 deg)")

    # ---- the coil lanes: dwell/spark PAIRS ----
    coils = {c: v for (s, c), v in lanes.items() if s == "Coil"}
    check("coil lanes present", len(coils) > 0, f"{len(coils)} coil channel(s)")
    for c, edges in sorted(coils.items()):
        # A coil that rises and never falls is a coil left dwelling — the capture must show the
        # pair, because "dwell start with no spark" is exactly the failure a tuner is looking for.
        rises = sum(1 for x in edges if x["active"])
        falls = sum(1 for x in edges if not x["active"])
        check(f"coil {c} pairs up", rises == falls and rises > 0,
              f"{rises} dwell-start / {falls} spark")

    # ---- THE check: recorded angles vs what the ECU says it commanded ----
    #
    # Everything above proves the capture is well-formed. This proves it is TRUE: the ECU
    # publishes the advance and dwell it computed, and the capture says where the coil actually
    # charged and discharged. If those disagree, the display is drawing something the engine did
    # not do — which is the only way this feature can fail while still looking healthy.
    adv   = l.telem("advance") * l.meta.t("advance")["scale"]     # deg BTDC
    dwell = l.telem("dwell")   * l.meta.t("dwell")["scale"]       # us
    # At CRANK-only sync the recorder's angles are folded to one revolution (the header says so),
    # so a cylinder's 540 deg TDC is seen at 180. Fold the expectation the same way or every
    # second cylinder "fails" for a reason that is not a fault.
    fold = cap["cycle_angle"] if cap["sync_level"] >= 2 else 360.0
    deg_per_us = (cap["rpm"] * 360.0) / 60e6 if cap["rpm"] > 0 else 0.0
    print(f"  commanded: {adv:.1f} deg BTDC, {dwell:.0f} us dwell "
          f"({dwell * deg_per_us:.1f} deg at {cap['rpm']:.0f} rpm)")

    for c, edges in sorted(coils.items()):
        tdc_deg = next((t / 10.0 for t, ign, _ in FIRING if ign == c), None)
        if tdc_deg is None:
            continue
        expect = (tdc_deg - adv) % fold
        sparks = sorted({x["angle"] for x in edges if not x["active"]})
        dwells = sorted({x["angle"] for x in edges if x["active"]})
        if not sparks or not dwells:
            check(f"coil {c} timing", False, "missing a dwell-start or spark edge")
            continue
        best = min(sparks, key=lambda a: min(abs(a - expect), fold - abs(a - expect)))
        err  = min(abs(best - expect), fold - abs(best - expect))
        check(f"coil {c} sparks at the commanded advance", err < 2.0,
              f"TDC {tdc_deg:.0f} - {adv:.1f} BTDC => {expect:.1f} deg; recorded {best:.1f} "
              f"(err {err:.1f})")

        # Dwell duration end-to-end: the recorded arc, converted back to microseconds at the
        # capture's own rpm, must be the dwell the ECU commanded.
        start = min(dwells, key=lambda a: (best - a) % fold)
        arc   = (best - start) % fold
        got_us = arc / deg_per_us if deg_per_us > 0 else 0.0
        check(f"coil {c} dwell duration matches", abs(got_us - dwell) < dwell * 0.10,
              f"{arc:.1f} deg = {got_us:.0f} us vs commanded {dwell:.0f} us")

    # ---- a re-arm gives a FRESH cycle ----
    cap2 = l.cycle_capture(timeout=5.0)
    check("re-arm captures again", cap2["state"] == "Complete" and cap2["total"] > 0,
          f"second capture: {cap2['total']} edges, state={cap2['state']}")

    # ---- the gate: a capture that is never armed stays empty ----
    # This is the 20k-rpm safety property on real hardware: with nothing armed, the running engine
    # must not be filling the buffer in the background.
    time.sleep(0.5)
    idle = l._cycle_page(0x01, 0)
    check("no capture runs unarmed", idle["state"] == "Complete" and idle["total"] == cap2["total"],
          f"state={idle['state']} total={idle['total']} (unchanged since the last read)")

    l.restore_script()
    l.execute("key auto")
    l.close(); sp.close()

    print(f"\n=== {'ALL PASS' if not fails else 'FAILED: ' + ', '.join(fails)} ===")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
