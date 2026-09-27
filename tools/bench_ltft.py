#!/usr/bin/env python3
"""The long-term fuel trim, on the real ECU — apply, learn, and WHICH SENSORS FEED IT.

Four claims, none of which a static read can settle:

  applied on its own switch  — the firmware gates LTFT on ltft_enabled ALONE ("Applied whenever
                               enabled, conditions or not", Lambda.cpp), so the stored surface must
                               still multiply fuel with closed loop OFF. The studio greyed that switch
                               out on lambda.enabled, which is what made this worth proving.
  learning needs the loop    — learn = stft_on && ltft_on && permit, so with closed loop off a cell
                               must NOT move however wrong the mixture is.
  it learns the right way    — lean (lambda above target) has to ADD fuel: the cell goes positive.
  a narrowband must NOT     — resolve_widebands() admits every enabled sensor whose catalogue type is
  feed a lambda loop           `lambda` and group is `o2`, and narrowband_1 is declared as exactly that.
                               But a narrowband is a SWITCHING sensor: ~0.9 V rich, ~0.1 V lean, with an
                               almost vertical transition at stoich. Its calibrated "lambda" is a
                               fiction outside about 0.98-1.02, and the module consumes it as a
                               PROPORTIONAL error -- err += (lam - target)/target -- so a rich narrowband
                               reading 0.85 is read as "15 % rich" when all it said was "rich, by an
                               unknown amount". It is then AVERAGED with the real widebands, so adding
                               one degrades a sensor that was working. The last check asserts the
                               behaviour that SHOULD hold. It is expected to FAIL against this firmware:
                               that failure is the report.

The trim lives in the LEARNED region (base LEARNED_PAGE_BASE), not the tune, and is addressed at the
ALLOCATION stride — cell = z*xa*ya + y*xa + x with xa/ya the axis ALLOCATIONS, not the live bin counts
(Lambda.cpp cell_at / Cache::tiCellOffset agree on this; TableImage.h's comment does not).

  python3 tools/bench_ltft.py
"""
import struct, sys, time

sys.path.insert(0, ".")
from tools.ts_bench import TsLink, Meta

# LEARNING NEEDS A RUNNING ENGINE, and that is not a bench inconvenience — it is the gate. The trim may
# only absorb what belongs in an rpm x load cell, so the firmware requires the engine to have CAUGHT and
# to have been running for `learn_run_time_s`. A static rig with signals injected under it can prove the
# APPLY path all day and can never prove learning, so this bench drives the stim.
try:
    from tools.gen_rebench import open_stim, select, configure, WHEELS
    from tools.bench_fuel import set_fixed_rpm
except ImportError:                      # pyserial or the stim absent: apply-only, and say so
    open_stim = None
WHEEL_IDX = 6                            # 36-1: dense, single crank stream, locks quickly
RUN_RPM   = 1200

TTL_MS   = 500
IDX_WB1  = 1     # sensors.sensor[] catalogue index — Wideband O2 1
IDX_NB1  = 52    # …and Narrowband 1
# SETTLE IS A TIMEOUT NOW, NOT A COST. Each wait below polls for the thing it is waiting for; a
# signalWrite lands in 6-8 ms and a config write reaches the module in 35 ms, measured. The learning
# and ramp waits further down are NOT converted — those are the measurement itself, and polling
# cannot shorten an integrator.
SETTLE   = 1.2


def lua(vals):
    body = "\n".join(f'  signalWrite("{k}", {v}, {TTL_MS})' for k, v in vals.items())
    return f"function onTick()\n{body}\nend\nsetTickRate(200)\n"


def main():
    M, L = Meta(), TsLink()
    print(L.hello())

    tbl = M.config["fuel_calculator"]["lambda_ltft"]
    base, scale, cell_sz = tbl["offset"], tbl["scale"], 2
    xa = tbl.get("cols_max", 32)
    ya = tbl.get("rows_max", 32)
    print(f"  lambda_ltft @ 0x{base:08x}  alloc {xa}x{ya}  scale {scale}")

    def cell_off(x, y, z=0):
        return base + (z * xa * ya + y * xa + x) * cell_sz

    def set_cell(x, y, pct):
        L.write_raw(cell_off(x, y), struct.pack("<h", int(round(pct / scale))))

    def get_cell(x, y):
        return struct.unpack("<h", L.read_config_raw(cell_off(x, y), 2))[0] * scale

    # TELEMETRY IS RAW ON THE WIRE. telem() hands back counts; the channel's own scale is what turns
    # 700 into 7.00 %. Comparing counts against engineering numbers is how the first run of this bench
    # decided the ECU was applying nothing at all.
    def tel(ch):
        return L.telem(ch) * M.t(ch)["scale"]

    # POLL, DO NOT GUESS A SETTLE TIME. A fixed sleep after a write is a race: the same check passed on
    # one run and failed on the next with the ECU behaving identically. Wait for the value to appear, or
    # time out and report what it actually was.
    def settle(read, want, tol=0.01, secs=4.0):
        t0 = time.time(); v = read()
        while time.time() - t0 < secs:
            v = read()
            if abs(v - want) <= tol:
                return v
            time.sleep(0.1)
        return v

    fails = []

    def check(ok, what, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {what}" + (f"   — {detail}" if detail else ""))
        if not ok:
            fails.append(what)

    # ---- arm: both O2 sensors enabled so the module resolves them, values injected on the bus ----
    sensors = M.array("sensors", "sensor")
    def sensor_en(idx, on):
        o = sensors["base_offset"] + idx * sensors["stride"] + sensors["fields"]["enabled"]["rel_offset"]
        L.write_raw(o, bytes([1 if on else 0]))

    saved = {n: L.get_config(n) for n in ("lambda_enabled", "lambda_ltft_enabled", "lambda_ltft_gain")}

    # THE GAINS ARE A SURFACE NOW (rpm x MAP), not two scalars — so winding the loop up for a bench run
    # means writing the whole table, not one field. Saved and restored like everything else here.
    ki = M.config["lambda"]["stft_ki_table"]
    ki_off, ki_scale = ki["offset"], ki["scale"]
    ki_n = ki.get("cols_max", 8) * ki.get("rows_max", 8)
    ki_saved = L.read_config_chunked(ki_off, ki_n * 2, chunk=200)
    def set_ki(pct):
        L.write_raw(ki_off, struct.pack("<" + "H" * ki_n, *([int(round(pct / ki_scale))] * ki_n)))
    # clt IS PART OF THE LEARN GATE, so the rig has to present a warm engine or nothing is learned
    # and the bench reports a firmware fault that is really a cold one. lambda.learn_min_clt defaults
    # to 60 C; the bench's own CLT sensor reads ambient (0.1 C on an unwired input), so without this
    # the positive learning case can never pass. It went unnoticed for as long as there was no trigger:
    # with the engine stopped the suite took the refusal path and agreed that nothing was learned.
    #
    # rpm is listed but NOT injectable once a trigger is connected — the decoder owns it and the
    # priority gate rejects the write. It is left here because it is what makes the suite work on a
    # rig with no trigger at all; where there is one, the real 1203 rpm is inside the gate's band
    # anyway. See bench-harness-pitfalls.
    sig = {"rpm": 3000.0, "clt": 90.0, "fuel_load": 50.0, "lambda_1": 1.00, "narrowband_1": 1.00}
    L.set_script_wait(lua(sig), {k: v for k, v in sig.items() if k in M.telem}, timeout=SETTLE)
    sensor_en(IDX_WB1, True); sensor_en(IDX_NB1, False)
    # AND SELECT IT. The loop's sensor is chosen now, not inferred from what happens to be enabled —
    # an enabled sensor nobody pointed the loop at drives nothing, deliberately.
    L.set_config("lambda_o2_src_1", 4)          # Wideband 1
    L.set_config("lambda_o2_src_2", 0)          # unbanked
    L.set_config("lambda_enabled", 0)
    L.set_config("lambda_ltft_enabled", 1)
    time.sleep(SETTLE)

    # Where is the engine on the grid? Read it back from the ECU rather than assuming a cell: the
    # axes are ve_table's, whatever this tune's bins happen to be.
    # Sweep the row for the one cell the ECU reacts to — that IS the live cell, by definition.
    # ---- get the engine RUNNING first: the live cell is a function of rpm, and learning needs it ----
    stim = None
    if open_stim:
        try:
            stim = open_stim()
            configure(L, WHEELS[WHEEL_IDX]); select(stim, WHEEL_IDX)
            set_fixed_rpm(stim, RUN_RPM)
            L.wait_until(lambda f: f["engine_state"] == 2, timeout=6.0)
            st = L.telem("engine_state")
            if st != 2:
                print(f"    engine_state={st}: the stim is not driving the trigger input "
                      f"(teeth={L.telem('trigger_teeth')}). Zero teeth AND zero errors is a rig "
                      f"state, not a firmware one — apply is still testable, learning is not.")
            # NOT CONVERTIBLE. This is the firmware's own settling gate — the engine must have been
            # running for learn_run_time_s (5 s here) before a cell may move, and nothing is published
            # that says "the gate is now open". Waiting it out IS the test setup. Turning the config
            # down for the bench would make the suite faster and stop it testing the shipped value.
            time.sleep(float(L.get_config("lambda_learn_run_time_s")) + 1.0)
        except Exception as ex:
            print(f"    (no stim: {ex} — learning cannot be tested, only apply)")
            stim = None
    else:
        print("    (pyserial/stim unavailable — learning cannot be tested, only apply)")

    # WHICH CELL IS LIVE? ASK THE ECU, in one shot. Sweeping cell-by-cell and watching for a reaction
    # is a race against telemetry lag — it reported (0,5) here yesterday when the ECU was demonstrably
    # applying and learning (0,10). So instead: write a RAMP over the whole surface, cell i holding
    # i*0.05 %, and read ltft_pct back. The value IS the index. No dwell, no polling, no guessing.
    print("\n-- locating the live cell (the ECU names it) --")
    STEP = 0.05
    ramp = struct.pack("<" + "h" * (xa * ya),
                       *[int(round(i * STEP / scale)) for i in range(xa * ya)])
    for k in range(0, len(ramp), 200):                       # the link writes in blocks
        L.write_raw(base + k, ramp[k:k + 200])
    time.sleep(SETTLE)
    idx = int(round(tel("ltft_pct") / STEP))
    lx, ly = idx % xa, idx // xa
    check(0 <= idx < xa * ya, "the ECU reported which cell it is applying",
          f"index {idx} -> (x={lx}, y={ly})")
    for k in range(0, xa * ya * 2, 200):                     # wipe the ramp back out
        L.write_raw(base + k, b"\x00" * min(200, xa * ya * 2 - k))
    time.sleep(SETTLE)

    # ---- 1. applied with CLOSED LOOP OFF ----
    print("\n-- LTFT applies on its own switch, closed loop OFF --")
    set_cell(lx, ly, 10.0)
    pct  = settle(lambda: tel("ltft_pct"), 10.0, 0.5)
    mult = settle(lambda: tel("fuel_corr_ltft"), 1.10)
    check(abs(pct - 10.0) < 0.5, "a +10 % cell reads back as +10 %", f"{pct:.2f} %")
    check(abs(mult - 1.10) < 0.01, "…and multiplies fuel by 1.10 with lambda_enabled = 0", f"x{mult:.3f}")

    set_cell(lx, ly, -10.0)
    mult = settle(lambda: tel("fuel_corr_ltft"), 0.90)
    check(abs(mult - 0.90) < 0.01, "a NEGATIVE cell takes fuel away", f"x{mult:.3f}")

    # ---- 2. the trim's own switch really is the gate ----
    L.set_config("lambda_ltft_enabled", 0)
    mult = settle(lambda: tel("fuel_corr_ltft"), 1.0)
    check(abs(mult - 1.0) < 0.01, "switching Long-Term Trim off makes it neutral", f"x{mult:.3f}")
    L.set_config("lambda_ltft_enabled", 1)

    # ---- 3. learning is gated on the fast loop ----
    # LEARNING NEEDS A RUNNING ENGINE — that is the gate doing its job, not a bench inconvenience. With
    # no trigger input the firmware is RIGHT to refuse, so report that the case could not be exercised
    # rather than a failure of something never run.
    if L.telem("engine_state") != 2:
        # THE GATE'S OTHER HALF, and it is the half this rig CAN prove. Learning happening needs a
        # running engine and there is no way to fake one — EngineTask takes run state from the
        # decoder's pos.rpm, not from the bus, so no injection reaches it. But "learning is REFUSED
        # while the engine is not running" is the same gate seen from the other side, it is shipped
        # behaviour, and it is exactly what a stopped rig is in a position to demonstrate. Asserting
        # it beats printing SKIPPED and calling the suite finished.
        print("\n-- the learn gate REFUSES while the engine is not running --")
        print("    (learning HAPPENING needs teeth on the trigger input; this is the complement)")
        set_cell(lx, ly, 0.0)
        L.set_config("lambda_ltft_enabled", 1)
        L.set_config("lambda_enabled", 1)
        set_ki(20.0)
        # Everything else the gate asks for, satisfied: a lean sensor it can hear, and an rpm inside
        # the band (learn_min/max_rpm read the BUS, so this one IS injectable — the running-engine
        # condition is the only one that is not).
        sig["lambda_1"] = 1.10
        L.set_script(lua({**sig, "rpm": 3000.0, "clt": 90.0}))
        # NOT CONVERTIBLE, and this is the important case. The claim is that the cell does NOT move.
        # You cannot poll for something failing to happen — the elapsed time IS the evidence, and it
        # only means anything if it is longer than the time the SAME cell took to move when the gate
        # was open (that is the poll above, bounded at 6 s). Shortening this would leave a check that
        # passes because it did not wait, which is the "assertion that cannot fail" trap again.
        time.sleep(8.0)
        corr = tel("fuel_corr_stft")
        check(corr > 1.0, "the fast loop DOES run with the engine stopped (it needs no gate)",
              f"x{corr:.4f}")
        check(get_cell(lx, ly) == 0.0,
              "…and nothing is learned, because the engine has never run",
              f"cell {get_cell(lx, ly):+.2f} %")
        check(L.telem("engine_state") != 2, "engine_state is not RUNNING", "as expected on a static rig")
        set_cell(lx, ly, 0.0)
        sensor_en(IDX_NB1, False)
        if stim: set_fixed_rpm(stim, 0); stim.close()
        L.write_raw(ki_off, ki_saved)
        L.set_config("lambda_o2_src_1", 0); L.set_config("lambda_o2_src_2", 0)
        for n, v in saved.items(): L.set_config(n, v)
        L.restore_script()
        print(f"\n{'ALL PASSED' if not fails else str(len(fails)) + ' FAILED'}")
        return 1 if fails else 0

    print("\n-- learning needs closed loop, and moves the right way --")
    set_cell(lx, ly, 0.0)
    sig["lambda_1"] = 1.10                      # LEAN: measured above target -> must ADD fuel
    L.set_script(lua(sig)); time.sleep(3.0)   # also a NOT-moved claim: the wait is the evidence
    check(abs(get_cell(lx, ly)) < 0.2, "with closed loop OFF a lean mixture teaches nothing",
          f"{get_cell(lx, ly):+.2f} %")

    L.set_config("lambda_enabled", 1)
    set_ki(20.0)                                # 20 %/lambda across the surface: brisk, for a bench run
    L.set_config("lambda_ltft_gain", 200)   # U08 at scale 0.001 -> 0.2 of the STFT per step
    # NOT CONVERTIBLE — and this one bit. Polling until the cell passes the 0.2 % the check asserts
    # returns in a fraction of a second and the check still passes, so the conversion looked free.
    # It is not: section 4 below compares two fixed-length ramps against each other, and both inherit
    # the loop state THIS wait leaves behind. Stopping mid-ramp handed leg 1 a half-wound integrator
    # and the comparison failed (+12.34 vs +9.01, against a 15 % window). The 6 s is not slack, it is
    # leaving the loop settled for the next section.
    time.sleep(6.0)
    learned = get_cell(lx, ly)
    check(learned > 0.2, "with closed loop ON a LEAN mixture learns POSITIVE (adds fuel)",
          f"{learned:+.2f} %")

    # ---- 4. a narrowband must not be averaged into a lambda loop ----
    print("\n-- narrowband: a switching sensor in a proportional loop --")
    L.set_config("lambda_enabled", 0)
    set_cell(lx, ly, 0.0)
    L.wait_until(lambda f: abs(f["ltft_pct"] * M.telem["ltft_pct"]["scale"]) < 0.05, timeout=SETTLE)
    sig["lambda_1"] = 1.10          # the real sensor: genuinely 10 % lean
    L.set_script_wait(lua(sig), {k: v for k, v in sig.items() if k in M.telem}, timeout=SETTLE)

    sensor_en(IDX_NB1, False); time.sleep(0.1)
    L.set_config("lambda_enabled", 1); time.sleep(4.0)
    wb_only = get_cell(lx, ly)
    check(wb_only > 0.2, "the wideband alone learns positive on a lean mixture", f"{wb_only:+.2f} %")

    L.set_config("lambda_enabled", 0)
    set_cell(lx, ly, 0.0)
    # A narrowband pegged RICH. On a real sensor this is ~0.9 V and means "rich, amount unknown"; the
    # lambda calibration turns it into a number the loop treats as a measurement.
    sig["narrowband_1"] = 0.90
    L.set_script(lua(sig))
    sensor_en(IDX_NB1, True); time.sleep(0.1)
    L.set_config("lambda_enabled", 1); time.sleep(4.0)
    both = get_cell(lx, ly)
    print(f"    wideband only {wb_only:+.2f} %   |   with a narrowband enabled {both:+.2f} %")
    # TOLERANCE SET FROM WHAT THIS IS FOR, not from wishful precision. Each leg learns for a fixed
    # four seconds from a zeroed cell, and the trim is a RAMP — so where it has got to depends on how
    # many control frames actually ran, when the loop enabled, and serial latency on the way in. The
    # spread across runs is a percentage point or two on values around 11-14; a 0.2 pp window is
    # inside the noise and failed two runs in three while nothing was wrong.
    #
    # What the check exists to catch is a switching sensor being AVERAGED INTO the wideband loop, and
    # that is not a subtle effect: when it was real the learned value went from +11.80 % to +1.75 %.
    # Fifteen percent of the reading catches that with room to spare and does not fire on ramp jitter.
    # MEASURED, not guessed. Four consecutive runs on the settled rig gave (wb_only, both) of
    # 13.61/13.44, 12.34/9.01, 11.02/13.17 and 13.32/12.11 — gaps of 0.17, 3.33, 2.15 and 1.21 pp on
    # values of 9-14, so the run-to-run spread of a fixed 4 s ramp is up to 27 %. The 15 % window
    # below failed two of those four with nothing wrong, in BOTH directions (the narrowband leg came
    # out higher as often as lower), which is the signature of jitter and not of a sensor being
    # averaged in. The defect this exists to catch moved the learned value from +11.80 % to +1.75 %,
    # an 85 % collapse in one direction, so 40 % still catches it with more than twice the margin.
    check(abs(both - wb_only) <= max(0.5, 0.40 * abs(wb_only)),
          "enabling a NARROWBAND does not change what the wideband loop learns",
          f"{both:+.2f} % vs {wb_only:+.2f} % - it is being averaged in as if it were a wideband")

    # ---- leave the rig as found ----
    set_cell(lx, ly, 0.0)
    sensor_en(IDX_NB1, False)
    if stim:
        set_fixed_rpm(stim, 0); stim.close()
    L.write_raw(ki_off, ki_saved)               # the gain surface back as it was
    L.set_config("lambda_o2_src_1", 0); L.set_config("lambda_o2_src_2", 0)
    for n, v in saved.items():
        L.set_config(n, v)
    L.restore_script()

    print(f"\n{'ALL PASSED' if not fails else str(len(fails)) + ' FAILED: ' + ', '.join(fails)}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
