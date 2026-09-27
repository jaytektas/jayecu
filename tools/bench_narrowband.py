#!/usr/bin/env python3
"""The NARROWBAND closed loop, on the real ECU.

A narrowband is a zirconia Nernst cell: ~0.1 V lean, ~0.9 V rich, switching almost vertically at
stoichiometry. It reports which SIDE of stoich the mixture is on and nothing about how far, so the
loop that consumes it cannot be proportional — it ramps until the sensor switches, jumps, and ramps
back. What the engine gets is a limit cycle whose AVERAGE is the correction.

Every claim below follows from that and none of it can be read off the source:

  it drives the loop     — cl_state must say NARROWBAND (2), not wideband, not open.
  lean ADDS fuel         — below the switch point the correction ramps POSITIVE, and keeps going while
                           the sensor stays there. Direction is the whole safety argument.
  rich TAKES fuel        — and symmetrically.
  it switches, not settles— crossing the threshold reverses the ramp, so the correction turns around
                           rather than converging on a value it has no way to compute.
  a wideband wins        — enable one and the narrowband stops driving, because one of them measures
                           the quantity and the other brackets it.
  off stoich it OPENS    — move the target away from lambda 1 and the loop must open (state 3) rather
                           than servo a sensor that is pinned to a rail. This is the case a narrowband
                           car meets under boost.
  the surface learns the — LTFT must take the CENTRE of the oscillation, not whatever phase the learn
  centre, not the saw     step landed in.

  python3 tools/bench_narrowband.py
"""
import struct, sys, time

sys.path.insert(0, ".")
from tools.ts_bench import TsLink, Meta

TTL_MS  = 500
IDX_WB1 = 1
IDX_NB1 = 52
# SETTLE IS NOW A CEILING, NOT A COST. Every wait below polls for the thing it is waiting for and
# returns as soon as it happens — measured, a signalWrite lands in 6-8 ms and a config write changes
# a module's behaviour in 35 ms, against the 1.2 s this used to sleep unconditionally. The number is
# kept as the TIMEOUT, so a genuinely stuck rig still fails in the same time it used to pass in.
SETTLE  = 1.2
OFF, WIDEBAND, NARROWBAND, OPEN = 0, 1, 2, 3


def lua(vals):
    body = "\n".join(f'  signalWrite("{k}", {v}, {TTL_MS})' for k, v in vals.items())
    return f"function onTick()\n{body}\nend\nsetTickRate(200)\n"


def main():
    M, L = Meta(), TsLink()
    print(L.hello())
    tbl = M.config["fuel_calculator"]["lambda_ltft"]
    base, cscale = tbl["offset"], tbl["scale"]
    xa, ya = tbl.get("cols_max", 32), tbl.get("rows_max", 32)

    def tel(c):   return L.telem(c) * M.t(c)["scale"]

    sensors = M.array("sensors", "sensor")
    def sensor_en(i, on):
        o = sensors["base_offset"] + i * sensors["stride"] + sensors["fields"]["enabled"]["rel_offset"]
        L.write_raw(o, bytes([1 if on else 0]))

    fails = []
    def check(ok, what, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {what}" + (f"   — {detail}" if detail else ""))
        if not ok: fails.append(what)

    saved = {n: L.get_config(n) for n in ("lambda_enabled", "lambda_ltft_enabled", "lambda_ltft_gain")}

    # ONE CONTROL LAW NOW, so the gain is the same surface a wideband uses — rpm x MAP, in % per unit
    # of error, and for a narrowband the error is in VOLTS. Written whole, saved and restored.
    ki = M.config["lambda"]["stft_ki_table"]
    ki_off, ki_scale = ki["offset"], ki["scale"]
    ki_n = ki.get("cols_max", 8) * ki.get("rows_max", 8)
    ki_saved = L.read_config_chunked(ki_off, ki_n * 2, chunk=200)
    def set_ki(pct):
        L.write_raw(ki_off, struct.pack("<" + "H" * ki_n, *([int(round(pct / ki_scale))] * ki_n)))
    kp = M.config["lambda"]["stft_kp_table"]
    kp_off, kp_scale = kp["offset"], kp["scale"]
    kp_saved = L.read_config_chunked(kp_off, ki_n * 2, chunk=200)
    def set_kp(pct):
        L.write_raw(kp_off, struct.pack("<" + "H" * ki_n, *([int(round(pct / kp_scale))] * ki_n)))
    # RESET THE INTEGRATOR by switching the loop off and on: open loop holds nothing, by design, so
    # this is the only honest way to start a ramp measurement from zero.
    def rearm():
        L.set_config("lambda_enabled", 0); time.sleep(0.4)
        L.set_config("lambda_enabled", 1); time.sleep(0.4)

    # WARM IT FIRST. The loop refuses a narrowband that has never reached nb_warm_mv, because a cold
    # Nernst cell cannot source EMF and a WARMING one sits near the switch point looking exactly like a
    # live sensor reporting stoichiometry. So the rig has to present a hot sensor before anything else
    # here means anything — which is itself worth asserting.
    # clt: the LEARN GATE wants a warm engine (lambda.learn_min_clt, 60 C by default) and the rig's
    # own sensor reads ambient. Without it the surface can never learn and the suite blames the loop.
    sig = {"narrowband_1": 0.80, "clt": 90.0}       # above Warm Voltage: declare the sensor alive
    L.set_script_wait(lua(sig), {k: v for k, v in sig.items() if k in M.telem}, timeout=SETTLE)
    sensor_en(IDX_WB1, False); sensor_en(IDX_NB1, True)
    # THE SENSOR IS CHOSEN NOW, not inferred from what happens to be enabled. Selecting it is part of
    # arming the rig — an enabled sensor nobody pointed the loop at drives nothing, on purpose.
    L.set_config("lambda_o2_src_1", 1)          # Narrowband 1
    L.set_config("lambda_o2_src_2", 0)          # unbanked
    L.set_config("lambda_ltft_enabled", 0)
    # GENTLE ON PURPOSE. The error here is fractional — (target - measured)/target, so ~0.55 with the
    # sensor pegged lean — and the authority clamp is 20 %. At the shipped 80 %/unit the loop reaches
    # that clamp in about a tenth of a second and there is no ramp left to watch: the first run of this
    # bench read x1.2000 -> x1.2000 and called it a stalled loop when it was a saturated one.
    # BOTH gains gentle. The error is fractional — (target - measured)/target, about 0.55 with the
    # sensor pegged — and the authority clamp is 20 %, so the SHIPPED proportional gain alone puts the
    # loop hard against that clamp before the integrator contributes anything. Two earlier runs of this
    # bench read x1.2000 -> x1.2000 and called it a stalled loop when it was a saturated one.
    set_kp(2.0)
    set_ki(4.0)
    L.set_config("lambda_enabled", 1)
    # WAIT FOR THE STATE THIS SECTION IS ABOUT, not merely for the loop to be non-OFF. The narrowband
    # has to be seen hot before it can drive, and "not OFF" is satisfied the instant the loop enables
    # — so a poll on that returned in milliseconds and the check below read the loop mid-warm-up.
    # That is the trap in converting a sleep: the old constant was covering a second condition nobody
    # had written down. Waiting for NARROWBAND itself is both correct and still far quicker.
    L.wait_until(lambda f: round(f["lambda_cl_state"] * M.telem["lambda_cl_state"]["scale"]) == NARROWBAND,
                 timeout=SETTLE * 4)

    print(f"\n-- who is driving --   (target lambda {tel('lambda_target'):.3f})")
    st = tel("lambda_cl_state")
    check(round(st) == NARROWBAND, "cl_state says NARROWBAND once the sensor has been hot",
          f"state={st:.0f}")
    # …and now let it go lean, which is what the ramp tests below are about.
    sig["narrowband_1"] = 0.20; L.set_script_wait(lua(sig), {k: v for k, v in sig.items() if k in M.telem}, timeout=SETTLE)

    print("\n-- lean ADDS fuel, and keeps adding while it stays lean --")
    rearm()
    a = tel("fuel_corr_stft"); time.sleep(2.0); b = tel("fuel_corr_stft")
    check(b > a, "the correction ramps UP below the switch point", f"x{a:.4f} -> x{b:.4f}")
    check(b > 1.0, "…and it is ADDING fuel, not removing it", f"x{b:.4f}")

    print("\n-- rich TAKES fuel: crossing the switch point reverses the ramp --")
    sig["narrowband_1"] = 0.80; L.set_script(lua(sig)); time.sleep(0.6)
    c = tel("fuel_corr_stft"); time.sleep(2.0); d = tel("fuel_corr_stft")
    check(d < c, "the correction ramps DOWN above the switch point", f"x{c:.4f} -> x{d:.4f}")
    check(c < b + 0.001, "…and it turned around at the crossing rather than carrying on up",
          f"peak x{b:.4f} -> x{c:.4f}")

    print("\n-- selecting a wideband moves the loop to it --")
    # There is no priority left to test: a wideband used to win automatically whenever one was
    # readable, which is the behaviour that let a narrowband be averaged in behind your back. It is a
    # SELECTION now, so what is worth proving is that changing the selection changes what drives.
    sensor_en(IDX_WB1, True)
    sig["lambda_1"] = 1.00; L.set_script_wait(lua(sig), {k: v for k, v in sig.items() if k in M.telem}, timeout=SETTLE)
    L.set_config("lambda_o2_src_1", 4)          # Wideband 1
    L.wait_until(lambda f: f["lambda_cl_state"] == 1, timeout=SETTLE * 2)
    st = tel("lambda_cl_state")
    check(round(st) == WIDEBAND, "cl_state says WIDEBAND once one is selected", f"state={st:.0f}")
    L.set_config("lambda_o2_src_1", 1)          # back to Narrowband 1
    sensor_en(IDX_WB1, False); del sig["lambda_1"]
    L.set_script_wait(lua(sig), {k: v for k, v in sig.items() if k in M.telem}, timeout=SETTLE * 2)

    print("\n-- off stoich it OPENS rather than chasing a saturated rail --")
    # MOVE THE TARGET, do not shrink the band to zero. The first version set nb_stoich_band = 0 and
    # expected "nothing counts as stoich" — but the shipped target is exactly lambda 1.000, and
    # |1.000 - 1.0| <= 0 is TRUE, so the loop stayed closed and the test blamed the firmware. This is
    # also the case that actually happens: a boost target the sensor cannot see.
    tl = M.config["fuel_calculator"]["target_lambda_table"]
    tl_n = tl.get("cols_max", 8) * tl.get("rows_max", 8) * tl.get("depth_max", 1)
    tl_saved = L.read_config_chunked(tl["offset"], tl_n * 2, chunk=200)
    raw = int(round(0.85 / tl["scale"]))
    for k in range(0, tl_n * 2, 200):
        n = min(200, tl_n * 2 - k)
        L.write_raw(tl["offset"] + k, struct.pack("<" + "H" * (n // 2), *([raw] * (n // 2))))
    L.wait_until(lambda f: abs(f["lambda_target"] * M.telem["lambda_target"]["scale"] - 0.85) < 0.02,
                 timeout=SETTLE * 2)
    tgt = tel("lambda_target")
    check(abs(tgt - 0.85) < 0.02, "the target moved off stoichiometry", f"lambda_target={tgt:.3f}")
    st, corr = tel("lambda_cl_state"), tel("fuel_corr_stft")
    check(round(st) == OPEN, "cl_state says OPEN LOOP", f"state={st:.0f}")
    check(abs(corr - 1.0) < 0.001, "…and it corrects NOTHING while open", f"x{corr:.4f}")
    for k in range(0, tl_n * 2, 200):            # target back as it was — chunked, like the write
        L.write_raw(tl["offset"] + k, tl_saved[k:k + min(200, tl_n * 2 - k)])
    L.wait_until(lambda f: abs(f["lambda_target"] * M.telem["lambda_target"]["scale"] - 1.0) < 0.02,
                 timeout=SETTLE)

    def scan():
        raw = L.read_config_chunked(base, 8192, chunk=200)
        v = struct.unpack("<" + "h" * 4096, raw)
        return [(i % 32, (i // 32) % 32, i // 1024, x * cscale) for i, x in enumerate(v) if x]

    print("\n-- the surface learns, and learns the right way --")
    # LEARNING NEEDS A RUNNING ENGINE — that is the gate, not a bench inconvenience. Without teeth on
    # the trigger input the firmware is right to refuse, so say the test could not run rather than
    # report a failure of something that was never exercised.
    if L.telem("engine_state") != 2:
        # THE COMPLEMENT, which a stopped rig CAN prove. Learning happening needs a running engine and
        # nothing fakes one (EngineTask takes run state from the decoder's pos.rpm, not the bus). But
        # "the switching loop corrects while the trim learns nothing" isolates the gate to exactly the
        # thing it gates, and is shipped behaviour worth asserting rather than skipping past.
        print("    engine not RUNNING — asserting the gate's REFUSAL instead (the positive case needs teeth)")
        # Wipe FIRST, then run the loop hard for long enough that a working learner would have written
        # something. An empty scan only means anything if the surface started empty and the loop was
        # given its chance; scanning a surface nothing tried to move proves nothing at all.
        for k in range(0, 8192, 200):
            L.write_raw(base + k, b"\x00" * min(200, 8192 - k))
        # ARM THE TRIM. Learning is switched OFF above, and a surface that stays zero because nobody
        # asked it to learn proves nothing about the gate — it is an assertion that cannot fail. Turn
        # LTFT on, at the same gain the positive test uses, so the ONLY thing still withheld is the
        # running engine.
        L.set_config("lambda_ltft_enabled", 1)
        L.set_config("lambda_ltft_gain", 200)
        time.sleep(0.1)          # no single observable; a config write lands in ~35 ms
        sig["narrowband_1"] = 0.20                          # LEAN — the loop must want to add fuel
        L.set_script(lua(sig))
        time.sleep(10.0)
        corr = tel("fuel_corr_stft")
        check(corr > 1.001,
              "the narrowband loop corrects with the engine stopped (the fast loop has no gate)",
              f"x{corr:.4f}")
        moved = scan()
        check(not moved, "…and the surface learned NOTHING, because the engine has never run",
              f"{len(moved)} cell(s) moved" if moved else "surface still all zero")
        for k in range(0, 8192, 200):
            L.write_raw(base + k, b"\x00" * min(200, 8192 - k))
        L.write_raw(ki_off, ki_saved); L.write_raw(kp_off, kp_saved)
        sensor_en(IDX_NB1, False)
        L.set_config("lambda_o2_src_1", 0); L.set_config("lambda_o2_src_2", 0)
        for n, v in saved.items(): L.set_config(n, v)
        L.restore_script()
        print(f"\n{'ALL PASSED' if not fails else str(len(fails)) + ' FAILED: ' + ', '.join(fails)}")
        return 1 if fails else 0
    # FIND THE CELL, do not assume one. The live site is wherever the axes put it — on this rig with the
    # engine stopped it is nowhere near (0,0) — and a check that reads a cell the ECU never writes
    # passes for a table that learned nothing at all. So: zero the whole surface, teach it, and let the
    # ECU tell us which cell moved.
    for (x, y, z, _) in scan():
        L.write_raw(base + (z * 1024 + y * 32 + x) * 2, struct.pack("<h", 0))
    L.set_config("lambda_ltft_enabled", 1)
    L.set_config("lambda_ltft_gain", 200)
    sig["narrowband_1"] = 0.20                      # held LEAN: a one-sided error it must absorb
    L.set_script(lua(sig)); time.sleep(10.0)

    moved = scan()
    check(len(moved) == 1, "exactly one cell learned", f"{len(moved)} cell(s) moved")
    if moved:
        x, y, z, pct = moved[0]
        check(pct > 0.2, "…and a LEAN sensor taught it to ADD fuel",
              f"cell(x={x}, y={y}, z={z}) = {pct:+.2f} %")
        check(abs(tel("ltft_pct") - pct) < 0.1, "…and that cell is the one being applied",
              f"ltft_pct={tel('ltft_pct'):+.2f} % vs cell {pct:+.2f} %")
        for (x, y, z, _) in moved:
            L.write_raw(base + (z * 1024 + y * 32 + x) * 2, struct.pack("<h", 0))

    # ---- leave the rig as found ----
    sensor_en(IDX_NB1, False)
    L.write_raw(ki_off, ki_saved); L.write_raw(kp_off, kp_saved)
    L.set_config("lambda_o2_src_1", 0); L.set_config("lambda_o2_src_2", 0)
    for n, v in saved.items(): L.set_config(n, v)
    L.restore_script()

    print(f"\n{'ALL PASSED' if not fails else str(len(fails)) + ' FAILED: ' + ', '.join(fails)}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
