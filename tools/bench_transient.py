#!/usr/bin/env python3
"""Transient fuel on the real ECU: MAP prediction, tip-out, the closed-loop hold, and the fuel film.

Everything here needs a RUNNING engine (the fuel calculator only computes per cycle), so the stim spins a
36-1 wheel at a fixed speed and Lua injects the sensors the bench has none of — MAP, coolant, air temp,
lambda, rail pressure, battery — plus the throttle, which is what the tests move.

  prediction   a throttle stab with MAP prediction on stands the Predicted MAP table in for the measured
               MAP (map_source = predicted, map_est = the table), then hands back once the hold expires.
  noise        a throttle held still does not predict: its rate sits under a tenth of the scaling table.
  tip-out      a fast LIFT predicts only when Predict Tip-Out is on — and then moves the estimate DOWN.
  CL hold      Closed-Loop Hold reads "Transient" (6) while prediction is active.
  fuel film    with the film on, a load step makes fuel_corr_film leave 1.000 and return to it.

Every setting it changes is read first and written back at the end, and nothing is burned: a reset
brings the ECU's stored tune back regardless.

WITH NO STIM the engine cannot run, so the film test is skipped — the film is modelled only while
running. Prediction still is: with the engine stopped the fuel calculation runs every 50 ms, and the
prediction, noise floor and tip-out are all worked out there.

  python3 tools/bench_transient.py [rpm]        (default 2000)
"""
import struct, sys, time
sys.path.insert(0, ".")
from tools.ts_bench import TsLink, Meta
from tools.gen_rebench import open_stim, select, configure, stim_halt, WHEELS, STREAM0, STREAM_STRIDE

WHEEL_IDX = 6            # 36-1
TTL_MS    = 500
BASE = {"clt": 80.0, "iat": 30.0, "lambda_1": 1.0, "fuel_pressure": 300.0, "battery": 13.5}

TOUCH = [  # every setting this changes, snapshotted first and put back last
    "fuel_calculator_map_predict_enabled", "fuel_calculator_map_predict_hold_ms",
    "fuel_calculator_map_predict_tipout", "fuel_calculator_wallfilm_enabled",
    "fuel_calculator_map_predict_scale_table", "fuel_calculator_map_predict_scale_table_y_en",
    "fuel_calculator_predicted_map_table", "fuel_calculator_film_pool_table",
    "fuel_calculator_film_evap_table", "fuel_calculator_stage1_film_enabled",
    "transient_throttle_enabled",
]


def lua(tps, map_kpa):
    vals = dict(BASE, tps=tps, map=map_kpa)
    body = "\n".join(f'  signalWrite("{k}", {v}, {TTL_MS})' for k, v in vals.items())
    return f"function onTick()\n{body}\nend\nsetTickRate(200)\n"


def main():
    rpm = int(sys.argv[1]) if len(sys.argv) > 1 else 2000
    M = Meta()
    fails = []

    def check(ok, what, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {what}" + (f"   — {detail}" if detail else ""))
        if not ok:
            fails.append(what)

    # ---- stim up, if there is one ----
    spec = WHEELS[WHEEL_IDX]
    try:
        sp = open_stim()
    except SystemExit:
        sp = None
        print("  no stim: the engine stays stopped — prediction is tested, the fuel film is not")
    trig_snap = None
    if sp:
        # THE TRIGGER IS PUT BACK TOO: pointing the ECU at the stim's 36-1 rewrites all six stream slots.
        L = TsLink()
        trig_snap = L.read_config_chunked(STREAM0, STREAM_STRIDE * 6)
        configure(L, spec); L.cmd(b"E", b"reconfig"); L.close()
        select(sp, WHEEL_IDX, reps=4)
        sp.reset_input_buffer(); sp.write(b"F" + struct.pack("<H", rpm)); time.sleep(0.3)

    L = TsLink()
    hello = L.hello()
    print(hello)
    if M.layout_hash not in hello:
        print(f"  STOP: this meta is {M.layout_hash} and the ECU is not — offsets would be wrong: {hello}")
        L.close(); sp and sp.close(); sys.exit(2)

    def tel(c):
        return L.telem(c) * M.t(c)["scale"]

    def put(name, value):
        L.set_config(name, value)

    def write_chunked(offset, data):
        for k in range(0, len(data), 200):                          # a frame carries ~200 bytes
            L.write_raw(offset + k, data[k:k + 200])

    def fill(name, value):
        e = M.c(name)
        n = e["size"] // M.size(e["datatype"])
        write_chunked(e["offset"], struct.pack("<" + M.fmt(e["datatype"])[1:] * n, *([value] * n)))

    snap = {}
    for n in TOUCH:
        e = M.c(n)
        snap[n] = L.read_config_chunked(e["offset"], e["size"])

    try:
        if sp:
            t0 = time.time()
            while time.time() - t0 < 20:
                e = L.telem_all()
                if e.get("sync_level", 0) >= 1 and e.get("rpm", 0) > 0:
                    break
                time.sleep(0.5)
            else:
                print("  FAIL  sync: the decoder never reached CRANK sync — is the stim on the crank input?")
                sys.exit(1)
        L.execute("key on")

        # ---- set up: prediction on, classic transient and film off, flat tables ----
        put("transient_throttle_enabled", 0)
        put("fuel_calculator_wallfilm_enabled", 0)
        put("fuel_calculator_map_predict_enabled", 1)
        put("fuel_calculator_map_predict_hold_ms", 200)
        put("fuel_calculator_map_predict_tipout", 0)
        put("fuel_calculator_map_predict_scale_table_y_en", 0)
        fill("fuel_calculator_map_predict_scale_table", 100)          # fully in at 100 %/s
        fill("fuel_calculator_predicted_map_table", 1000)             # 100.0 kPa everywhere
        L.set_script(lua(5.0, 40.0))
        time.sleep(1.5)

        print("\n== noise: throttle held still ==")
        src = [L.telem("map_source") for _ in range(20) if not time.sleep(0.025)]
        check(max(src) == 0 and abs(tel("map_est") - 40.0) < 1.0, "a still throttle does not predict",
              f"map_source max {max(src)}, map_est {tel('map_est'):.1f}")

        print("\n== prediction: throttle 5 -> 50 % ==")
        L.set_script(lua(50.0, 40.0))
        seen_src, peak_est, hold = 0, 0.0, set()
        for _ in range(40):
            s = L.telem("map_source")
            seen_src = max(seen_src, s)
            peak_est = max(peak_est, tel("map_est"))
            if s == 1:
                hold.add(L.telem("lambda_cl_hold"))
            time.sleep(0.02)
        check(seen_src == 1, "the stab switches to the predicted MAP", f"map_source reached {seen_src}")
        check(peak_est > 90.0, "…and the estimate is the table's 100 kPa, not the measured 40",
              f"peak map_est {peak_est:.1f}")
        print(f"        Closed-Loop Hold while predicting: {sorted(hold)} (6 = Transient; lambda may be off)")
        if L.get_config("lambda_enabled"):
            check(6 in hold, "closed-loop O2 holds for the transient", f"holds seen {sorted(hold)}")
        time.sleep(0.8)
        check(L.telem("map_source") == 0 and abs(tel("map_est") - 40.0) < 1.0,
              "the hold expires and the sensor is used again",
              f"map_source {L.telem('map_source')}, map_est {tel('map_est'):.1f}")

        print("\n== tip-out: throttle 50 -> 5 % ==")
        fill("fuel_calculator_predicted_map_table", 200)              # 20.0 kPa: a lift predicts DOWN
        time.sleep(0.3)
        L.set_script(lua(5.0, 40.0))
        seen = max(L.telem("map_source") for _ in range(30) if not time.sleep(0.02))
        check(seen == 0, "with Predict Tip-Out OFF a lift is left to the sensor", f"map_source max {seen}")
        L.set_script(lua(50.0, 40.0)); time.sleep(1.0)
        put("fuel_calculator_map_predict_tipout", 1)
        L.set_script(lua(5.0, 40.0))
        seen, low = 0, 99.0
        for _ in range(30):
            seen = max(seen, L.telem("map_source")); low = min(low, tel("map_est")); time.sleep(0.02)
        check(seen == 1 and low < 25.0, "with it ON the lift predicts, towards the lower table value",
              f"map_source {seen}, lowest map_est {low:.1f}")

        if not sp:
            print("\n== fuel film: SKIPPED (no stim, the engine is not running) ==")
        else:
            print("\n== fuel film: MAP 40 -> 90 kPa ==")
            put("fuel_calculator_map_predict_enabled", 0)
            put("fuel_calculator_wallfilm_enabled", 1)
            put("fuel_calculator_stage1_film_enabled", 1)
            fill("fuel_calculator_film_pool_table", 250)                  # 25 %
            fill("fuel_calculator_film_evap_table", 200)                  # 200 ms
            L.set_script(lua(5.0, 40.0)); time.sleep(2.0)
            steady = tel("fuel_corr_film")
            check(abs(steady - 1.0) < 0.01, "steady state: the film corrects nothing", f"x{steady:.3f}")
            L.set_script(lua(5.0, 90.0))
            peak = 1.0
            for _ in range(40):
                peak = max(peak, tel("fuel_corr_film")); time.sleep(0.02)
            time.sleep(2.0)
            after = tel("fuel_corr_film")
            check(peak > 1.02, "a load rise builds the film: more fuel", f"peak x{peak:.3f}")
            check(abs(after - 1.0) < 0.01, "…and it settles back to x1.000", f"x{after:.3f}")
    finally:
        for n, raw in snap.items():
            write_chunked(M.c(n)["offset"], raw)
        if trig_snap is not None:
            write_chunked(STREAM0, trig_snap)
            L.cmd(b"E", b"reconfig")
        L.restore_script()
        L.execute("key auto")
        L.close()
        if sp:
            # STOP THE WHEEL. 'F' sets a speed the stim then holds for good; left spinning, the bench ECU
            # sat at 2000 rpm afterwards, and an output test (engine-stopped only) cancelled itself the
            # instant it started — no signal on the scope and nothing to say why.
            stim_halt(sp)
            sp.close()
        print("\n  (every setting put back as it was; nothing burned)")

    print(f"\n=== {'ALL PASS' if not fails else 'FAILED: ' + ', '.join(fails)} ===")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
