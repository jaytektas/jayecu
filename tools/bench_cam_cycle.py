#!/usr/bin/env python3
"""Bench rig — engine-cycle capture with a CAM stream, at PHASE sync.

tools/bench_cycle.py runs a crank-only wheel, so everything it proves is the CRANK-sync path: a
360 deg folded span, one revolution of teeth, and each coil appearing to fire four times because
both half-buckets dispatch. That is not how an engine runs.

With a cam the decoder reaches PHASE, and a different set of behaviours has to be right:

    span is 720 and NOT folded          the capture header carries sync_level, and CycleWire only
                                        folds to 360 below PHASE
    the cam lane exists at all          every stream reaches the recorder, not just the crank
    it is labelled by CAM SLOT          the stream index and the cam slot differ as soon as there
                                        is more than one stream (cam is stream 1, slot 0), and the
                                        rest of the system names cams by slot
    one pulse, both edges               the wheel carries a single cam pulse per cycle and the
                                        stream is configured edge=BOTH
    each coil fires ONCE per cycle      four times is the crank-sync artefact, not the truth
    spark still lands where commanded   the reason the whole feature exists

Usage:  python3 tools/bench_cam_cycle.py [rpm]     (default 1200, wheel 4 = "60-2 crank and cam")
"""
import sys
import time

from tools.ts_bench import TsLink
from tools.gen_rebench import open_stim, select, configure, WHEELS
from tools.bench_fuel import set_fixed_rpm, wait_for_sync, lua_script
from tools.bench_cycle import configure_firing, FIRING

WHEEL_IDX = 4            # ardustim "60-2 crank and cam"; indices match our WHEELS dict
DEFAULT_RPM = 1200
PHASE_TIMEOUT_S = 45     # a single cam pulse per 720 deg: give the width matcher room to lock


def lanes_of(cap):
    out = {}
    for e in cap["edges"]:
        out.setdefault((e["signal"], e["channel"]), []).append(e)
    for v in out.values():
        v.sort(key=lambda x: x["angle"])
    return out


def main():
    rpm = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_RPM
    fails = []

    def check(name, cond, detail):
        print(f"  [{'PASS' if cond else 'FAIL'}] {name}: {detail}")
        if not cond:
            fails.append(name)

    spec = WHEELS[WHEEL_IDX]
    print(f"=== Bench: engine cycle at PHASE — {spec[0]} @ {rpm} rpm ===")

    sp = open_stim()
    l = TsLink()
    configure(l, spec)
    # COIL-ON-PLUG, because that is the whole point of having a cam. configure_firing() is shared with
    # bench_cycle, a CRANK-only suite, whose default is wasted-spark rows — on a four-cylinder two coils
    # firing twice each. This suite asserts four coils firing once each, which needs the cam PHASE sync
    # this bench establishes, so it asks for one coil row per cylinder.
    configure_firing(l, coils="cop")
    l.close()
    select(sp, WHEEL_IDX, reps=4)
    set_fixed_rpm(sp, rpm)

    l = TsLink()
    if not wait_for_sync(l, 30.0):
        print("  [FAIL] no crank sync — is the stim wired to DIG1?")
        l.close(); sp.close(); sys.exit(1)

    # PHASE needs the cam. Wait for it explicitly rather than capturing early and reporting the
    # crank-sync picture as if it were the cam one.
    t0 = time.time()
    lvl = 0
    while time.time() - t0 < PHASE_TIMEOUT_S:
        lvl = l.telem_all()["sync_level"]
        if lvl >= 2:
            break
        time.sleep(0.5)
    check("reaches PHASE", lvl >= 2, f"sync_level={lvl} after {time.time()-t0:.0f}s (2 = cam locked)")
    if lvl < 2:
        print("\n=== FAILED: no PHASE, the rest of this rig tests nothing ===")
        l.close(); sp.close(); sys.exit(1)

    l.execute("key on")
    l.set_script(lua_script(tps=8.0))
    time.sleep(2.0)

    cap = l.cycle_capture(timeout=8.0)
    lanes = lanes_of(cap)
    print(f"  capture: span={cap['cycle_angle']:.0f}deg rpm={cap['rpm']:.0f} sync={cap['sync_level']} "
          f"edges={cap['total']} dropped={cap['dropped']}")
    print("  lanes: " + ", ".join(f"{s}{c}:{len(v)}" for (s, c), v in sorted(lanes.items())))

    check("not truncated", cap["dropped"] == 0, f"dropped={cap['dropped']}")
    # The span is the header's, un-folded. Below PHASE the decoder cannot place an event in 720 deg
    # and the host folds to 360 — so 720 here IS the assertion that phase reached the capture.
    check("span is the full 720, not folded", abs(cap["cycle_angle"] - 720.0) < 0.1,
          f"{cap['cycle_angle']:.0f} deg (folded would be 360)")
    check("capture reports PHASE", cap["sync_level"] >= 2, f"sync_level={cap['sync_level']}")

    # ---- the cam lane ----
    cam = [(c, v) for (s, c), v in lanes.items() if s == "Cam"]
    check("cam lane present", len(cam) == 1, f"{len(cam)} cam lane(s)")
    if cam:
        ch, edges = cam[0]
        # Channel 0 = cam SLOT 0 (the first cam), NOT stream index 1. They differ with >1 stream and
        # the studio renders this as "Cam 1"; a stream-indexed lane would read "Cam 2" for the only
        # cam on the engine.
        check("labelled by cam SLOT, not stream index", ch == 0,
              f"channel={ch} (slot 0 = 'Cam 1'; stream index would be 1)")
        check("one pulse, both edges", len(edges) == 2, f"{len(edges)} edges")
        if len(edges) == 2:
            width = edges[1]["angle"] - edges[0]["angle"]
            # The wheel carries the cam high for one pattern entry of 240 across 720 deg = 3.0 deg
            # nominal; the recorded high-time is the gap between the two captured edges.
            check("pulse width is one pattern entry", 0.5 < width < 6.0,
                  f"{width:.1f} deg (nominal 3.0 = 1 of 240 entries)")
            check("edges are rise then fall", edges[0]["active"] and not edges[1]["active"],
                  f"active flags {edges[0]['active']}/{edges[1]['active']}")

    # ---- crank + grid at PHASE ----
    crank = [v for (s, c), v in lanes.items() if s == "Crank"]
    # 60-2 = 58 teeth per revolution; PHASE spans TWO revolutions, so 116. Crank sync would show 58.
    check("crank spans two revolutions", crank and 110 <= len(crank[0]) <= 120,
          f"{len(crank[0]) if crank else 0} teeth (58/rev x 2 = 116)")
    grid = [v for (s, c), v in lanes.items() if s == "Virtual"]
    check("virtual grid spans the full cycle", grid and 68 <= len(grid[0]) <= 76,
          f"{len(grid[0]) if grid else 0} marks (720/10 = 72; folded would be 36)")

    # ---- coils: ONCE per cycle at PHASE ----
    coils = {c: v for (s, c), v in lanes.items() if s == "Coil"}
    check("all four coils present", len(coils) == 4, f"{len(coils)}")
    per = {c: len(v) for c, v in coils.items()}
    check("each coil fires ONCE per cycle", all(n == 2 for n in per.values()),
          f"edges per coil {sorted(per.values())} (2 = one dwell+spark; 8 = the crank-sync artefact)")

    # ---- the cross-check that matters: spark vs commanded advance ----
    adv = l.telem("advance") * l.meta.t("advance")["scale"]
    # `dwell` is MICROSECONDS on the wire. Multiplying it by the meta scale as if it were a
    # display-scaled channel printed "3000.00 ms" — three seconds of dwell, which is nonsense on its
    # face. bench_cycle.py converts explicitly; do the same here.
    dwell_us = l.telem("dwell") * l.meta.t("dwell")["scale"]
    deg_per_us = cap["rpm"] * 360.0 / 60e6
    print(f"  commanded: {adv:.1f} deg BTDC, {dwell_us:.0f} us dwell "
          f"({dwell_us*deg_per_us:.1f} deg at {cap['rpm']:.0f} rpm)")
    for ch, edges in sorted(coils.items()):
        tdc = next((t / 10.0 for t, ign, _ in FIRING if ign == ch), None)
        if tdc is None or len(edges) != 2:
            continue
        # At PHASE there is no folding: the cylinder's own 720 deg TDC is where it actually is.
        expect = (tdc - adv) % 720.0
        spark = next((e["angle"] for e in edges if not e["active"]), None)
        err = min(abs(spark - expect), 720.0 - abs(spark - expect))
        check(f"coil {ch} sparks at the commanded advance", err < 2.0,
              f"TDC {tdc:.0f} - {adv:.1f} => {expect:.1f}; recorded {spark:.1f} (err {err:.1f})")

    # ---- the cam anchor must be STABLE, or phase is wandering ----
    angles = []
    for _ in range(4):
        c2 = l.cycle_capture(timeout=8.0)
        ce = sorted(e["angle"] for e in c2["edges"] if e["signal"] == "Cam" and e["active"])
        if ce:
            angles.append(ce[0])
        time.sleep(0.3)
    if angles:
        spread = max(angles) - min(angles)
        # A cam that moves between cycles means the phase anchor is slipping — the capture would
        # look fine each time while the engine's idea of which revolution it is on drifts.
        check("cam anchor is stable across cycles", spread < 2.0,
              f"rise angles {[round(a,1) for a in angles]} spread {spread:.1f} deg")

    l.restore_script()
    l.execute("key auto")
    l.close(); sp.close()

    print(f"\n=== {'ALL PASS' if not fails else 'FAILED: ' + ', '.join(fails)} ===")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
