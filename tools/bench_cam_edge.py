#!/usr/bin/env python3
"""A single-pulse cam through the phase-sync rpm band, on the real ECU and its VR inputs.

Kelly's Hilux lost phase (P0341) on every quick stab up through the cam's phase-sync rpm: a 36-2 crank and a
one-pulse front cam on VR inputs (MAX9924), cam Edge = Rising. A WIDTH stream used to capture both edges and
time itself from the closing one, which on a VR input wanders. Edge now decides what is captured. This rig
cannot make a VR falling edge wander (the stim's pulses are square), so what it proves is the other half: on
the real hardware path the cam locks on its selected edge and holds PHASE through repeated dips and surges
across the band edge — on Rising (the car's setting) and on Both.

The ECU must already carry the trigger: 36-2 on VR1 (capture 0), the cam a WIDTH stream on VR2 (capture 1),
phase-sync band at `band`. The stim's "36-2 with 1 tooth cam" puts its cam about 65 deg earlier than a 2JZ/1JZ
front cam, so the cam's Nominal Angle must be ~430 deg on this rig (495 on the car), or every edge lands
outside its allowance and phase is lost each cycle — which says nothing about the decoder.

The stim changes speed in one step, mid-tooth, which no engine can: a few noise edges and missed teeth per run
come from that, and none may cost phase.

Run:  python3 -m tools.bench_cam_edge [band_rpm]
"""
import sys
import time

from tools.ts_bench import TsLink
from tools.gen_rebench import open_stim, select, STREAM0, STREAM_STRIDE, F
from tools.bench_fuel import set_fixed_rpm

WHEEL_NAME = "36-2 with 1 tooth cam"
CAM_SLOT = 2
EDGES = {0: "Rising", 2: "Both"}
# The car's events: a dip under the band edge, then a surge back through it.
PROFILE = [(1300, 1.0), (1900, 0.6), (1450, 0.4), (1800, 0.5), (1520, 0.3),
           (2000, 0.5), (1400, 0.6), (1700, 0.4), (1480, 0.25), (1650, 0.35)]
CYCLES = 8


def wheel_index(sp):
    sp.reset_input_buffer(); sp.write(b"L"); time.sleep(1.0)
    names = sp.read(sp.in_waiting or 1).decode(errors="replace").split("\r\n")
    for i, n in enumerate(names):
        if n.strip() == WHEEL_NAME:
            return i
    return -1


def main():
    band = int(sys.argv[1]) if len(sys.argv) > 1 else 1500
    fails = []

    def check(name, cond, detail):
        print(f"  [{'PASS' if cond else 'FAIL'}] {name}: {detail}")
        if not cond:
            fails.append(name)

    print(f"=== Bench: {WHEEL_NAME}, cam on each Edge setting, phase-sync band {band} rpm ===")
    sp = open_stim()
    idx = wheel_index(sp)
    if idx < 0 or not select(sp, idx):
        print(f"  [FAIL] the stim has no '{WHEEL_NAME}'"); sys.exit(1)
    l = TsLink()
    base = STREAM0 + STREAM_STRIDE * CAM_SLOT
    edge_was = l.read_config_raw(base + F['edge'], 1)[0]
    for edge, ename in EDGES.items():
        print(f"\n-- cam Edge = {ename}")
        l.write_raw(base + F['edge'], bytes([edge])); l.execute("reconfig")
        set_fixed_rpm(sp, 2200); time.sleep(4.0)
        e0 = l.telem_all()
        check(f"{ename}: PHASE before the run", e0["sync_level"] >= 2, f"sync_level={e0['sync_level']}")
        errs, crossings, prev = [], 0, 2200
        for _ in range(CYCLES):
            for rpm, hold in PROFILE:
                crossings += (prev < band) != (rpm < band)
                prev = rpm
                set_fixed_rpm(sp, rpm); time.sleep(hold)
                e = l.telem_all()
                if hold >= 0.5 and e["rpm"] > 0:
                    errs.append(abs(e["rpm"] - rpm) / rpm * 100)
        set_fixed_rpm(sp, 2200); time.sleep(2.0)
        e1 = l.telem_all()
        lost = e1["trigger_phase_lost"] - e0["trigger_phase_lost"]
        noise = e1["trigger_noise_edges"] - e0["trigger_noise_edges"]
        check(f"{ename}: no phase lost through {crossings} band crossings", lost == 0, f"phase lost {lost}")
        check(f"{ename}: still PHASE", e1["sync_level"] >= 2, f"sync_level={e1['sync_level']}")
        worst = max(errs) if errs else 99.0
        check(f"{ename}: rpm reads true", worst < 5.0, f"worst {worst:.1f} % over {len(errs)} holds")
        print(f"     (noise edges {noise}, cam {e1['vvt_angle_1'] / 10:.1f} deg from nominal)")
    l.write_raw(base + F['edge'], bytes([edge_was])); l.execute("reconfig")
    l.close(); sp.close()
    print(f"\n=== {'ALL PASS' if not fails else 'FAILED: ' + ', '.join(fails)} ===")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
