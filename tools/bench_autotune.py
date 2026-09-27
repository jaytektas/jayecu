#!/usr/bin/env python3
"""Arm the rig so the STUDIO's VE autotuner has something real to learn from — then disarm it.

The autotuner lives in the studio, not the ECU, so it cannot be tested the way every other bench
script here works: the studio holds /dev/ttyACM0 while it runs, and nothing else can have the port at
the same time. So this is not a test, it is a FIXTURE. It puts the rig into a state where an autotune
run is meaningful, gets out of the way, and can put it back afterwards.

What "meaningful" needs:

  a turning engine    the trigger has to be running or the operating point is nothing and the studio's
                      Minimum RPM filter — correctly — throws away every record.
  a wideband          there is none on the bench, so lambda_1 is injected. A FIXED offset from the
                      target is what makes the run checkable: ask for `--lean 5` and every cell the
                      engine visits should end up proposing +5 %, which is a number to check rather
                      than a shape to admire.
  a load axis         fuel_load is the VE table's y channel; injected so the run sits in one cell.
  OPEN LOOP           closed loop off, so the ECU is not correcting the very error being measured.
                      The studio folds the ECU's correction back in either way (that is what the
                      `ego_channels` in the definition are for) but a bench that proves the plain
                      case first is a bench whose failures mean something.

  python3 tools/bench_autotune.py --arm [--rpm 1200] [--load 50] [--lean 5]
  python3 tools/bench_autotune.py --disarm

Arming SAVES what it changed to a state file and --disarm restores it, so the rig is handed back the
way it was found. Nothing here is burned: every write is to ECU RAM and a reset undoes the lot.
"""
import argparse, json, os, sys, time

sys.path.insert(0, ".")
from tools.ts_bench import TsLink, Meta

try:
    from tools.gen_rebench import open_stim, select, configure, WHEELS
    from tools.bench_fuel import set_fixed_rpm
except ImportError:
    open_stim = None

WHEEL_IDX = 6            # 36-1 — dense, single crank stream, locks quickly
IDX_WB1   = 1            # sensors.sensor[] catalogue index: Wideband O2 1
TTL_MS    = 500
STATE     = os.path.join(os.path.dirname(__file__), ".bench_autotune_state.json")
SAVED     = ("lambda_enabled", "lambda_ltft_enabled", "lambda_o2_src_1", "lambda_o2_src_2")


def lua(vals):
    body = "\n".join(f'  signalWrite("{k}", {v}, {TTL_MS})' for k, v in vals.items())
    return f"function onTick()\n{body}\nend\nsetTickRate(200)\n"


def sensor_enable(L, M, idx, on):
    a = M.array("sensors", "sensor")
    off = a["base_offset"] + idx * a["stride"] + a["fields"]["enabled"]["rel_offset"]
    L.write_raw(off, bytes([1 if on else 0]))


def arm(args):
    M, L = Meta(), TsLink()
    print(L.hello())

    state = {"config": {n: L.get_config(n) for n in SAVED},
             "lua": L.get_config_string("lua_source") if hasattr(L, "get_config_string") else None}
    with open(STATE, "w") as f:
        json.dump(state, f)

    sensor_enable(L, M, IDX_WB1, True)
    L.set_config("lambda_o2_src_1", 4)     # Wideband 1, named outright
    L.set_config("lambda_o2_src_2", 0)     # unbanked
    # OPEN LOOP. Both trims off — and off ZEROES them in this firmware rather than freezing them, so
    # fuel_corr_stft and fuel_corr_ltft both read 1.000 and the studio measures the table's own error.
    L.set_config("lambda_enabled", 0)
    L.set_config("lambda_ltft_enabled", 0)

    # THE OPERATING POINT FIRST, THE MIXTURE SECOND. The target is a MAP: it is a function of the rpm
    # and load the engine is at, so reading it before the engine is running and the load is injected
    # gives the target of a place the engine is about to leave. Doing that produced an injected 0.880
    # against a target that had since moved to 1.000 — a rig set up to be 6 % lean that was in fact
    # 12 % rich, and an autotuner correctly proposing -12 % while the bench claimed it was wrong.
    sig = {"clt": 90.0, "fuel_load": float(args.load)}
    L.set_script_wait(lua(sig), {k: v for k, v in sig.items() if k in M.telem}, timeout=2.0)

    stim = None
    if open_stim:
        try:
            stim = open_stim()
            configure(L, WHEELS[WHEEL_IDX]); select(stim, WHEEL_IDX)
            set_fixed_rpm(stim, args.rpm)
            L.wait_until(lambda f: f["engine_state"] == 2, timeout=8.0)
        except Exception as ex:
            print(f"  (no stim: {ex} — rpm will read 0 and Minimum RPM will filter everything)")
    else:
        print("  (pyserial/stim unavailable — rpm will read 0)")

    time.sleep(1.0)
    def tel(ch):
        return L.telem(ch) * M.t(ch)["scale"]

    # NOW ask what the mixture is being asked for, at the point the engine is actually at.
    target = tel("lambda_target")
    if not (0.5 <= target <= 1.6):
        target = 1.0
    lam = round(target * (1.0 + args.lean / 100.0), 4)
    sig["lambda_1"] = lam
    L.set_script_wait(lua(sig), {k: v for k, v in sig.items() if k in M.telem}, timeout=2.0)
    time.sleep(0.5)

    # WHAT THE RIG IS ACTUALLY AT, not what it was asked for. lambda_1 is a U08 at 0.01 per count, so
    # the bus cannot carry a finer mixture than 1 % of lambda — ask for 1.053 and the studio reads
    # 1.05. Reporting the requested figure would have this fixture accusing a correct autotuner of
    # being 0.3 % out. So the offset is read BACK, and that is the number to check the panel against.
    got  = tel("lambda_1")
    real = (got / target - 1.0) * 100.0 if target > 0 else 0.0
    print(f"  target   {target:.3f}   asked for {lam:.3f}, bus carries {got:.3f}"
          f"   ({real:+.2f} % lean, quantised from {args.lean:+.1f})")
    print(f"  rpm      {tel('rpm'):.0f}        fuel_load {tel('fuel_load'):.1f}   clt {tel('clt'):.1f}")
    print(f"  stft x{tel('fuel_corr_stft'):.3f}   ltft x{tel('fuel_corr_ltft'):.3f}"
          "   (both 1.000 = open loop, nothing hiding the error)")
    print(f"\n  ARMED. Open the studio, Tools > Auto Tune, Start. Every cell the engine visits should\n"
          f"  propose {real:+.2f} %. Then: python3 tools/bench_autotune.py --disarm")


def disarm(_args):
    M, L = Meta(), TsLink()
    print(L.hello())
    try:
        with open(STATE) as f:
            state = json.load(f)
    except OSError:
        state = {"config": {}}
        print("  (no saved state — restoring the injection only)")
    L.set_script_wait("function onTick()\nend\n", {}, timeout=2.0)
    for k, v in (state.get("config") or {}).items():
        L.set_config(k, v)
    if open_stim:
        try:
            set_fixed_rpm(open_stim(), 0)
        except Exception as ex:
            print(f"  (stim not stopped: {ex})")
    print("  DISARMED — injection cleared, closed-loop settings restored, stim stopped.")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", action="store_true")
    ap.add_argument("--disarm", action="store_true")
    ap.add_argument("--rpm", type=int, default=1200)
    ap.add_argument("--load", type=float, default=50.0)
    ap.add_argument("--lean", type=float, default=5.0, help="percent lean of target to inject")
    a = ap.parse_args()
    if a.disarm:
        disarm(a)
    elif a.arm:
        arm(a)
    else:
        ap.error("one of --arm / --disarm")
