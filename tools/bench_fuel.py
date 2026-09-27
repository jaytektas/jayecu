#!/usr/bin/env python3
"""Bench fuel rig — validate the fuel pipeline on real hardware with NO engine sensors.

The bench ECU has no analog sensors wired, so on its own the air model is fed zeros (VE reads
garbage, ~525%, PW meaningless). This rig makes fuel computable + checkable on the bench:

  1. STIM (ttyUSB0): spin a 36-1 crank wheel at a fixed RPM so the decoder gives real RPM + CRANK
     sync + the RUNNING state that wakes the per-cycle fuel compute. (Reuses gen_rebench's trigger
     config + ardustim control.)
  2. LUA (signalWrite at PRIO_LUA): inject the absent analog sensors — MAP / CLT / IAT / lambda /
     TPS — at sane values, refreshed with a TTL so they stay live.
  3. TELEMETRY: read back the computed chain (rpm / ve / charge_temp / air_mass / base_pw / inj_pw /
     fuel_corr_accel) and assert each is sane — not the no-sensor garbage.
  4. TRANSIENT: step the injected TPS and confirm TransientThrottle enriches (fuel_corr_accel > 1,
     transient_active = ENRICH) then decays.

Usage:  python3 tools/bench_fuel.py [rpm]      (default 2000 rpm, 36-1 wheel)

This drives the LIVE ECU; it forces key-on so the full per-cycle commit runs. Injectors only fire
if physically connected — the rig validates the COMPUTATION via telemetry regardless.
"""
import sys, time, struct

from tools.ts_bench import TsLink
from tools.gen_rebench import open_stim, select, configure, WHEELS

WHEEL_IDX = 6          # '36-1' — dense, locks in ~7 s, single crank stream (no cam needed)
DEFAULT_RPM = 2000

# Injected sensor set (signal id name -> value in the signal's native units). fuel_pressure + battery
# feed the injector model (inj_press_diff → flow table, battery → dead-time table) — without them the
# bench injector model is starved and base_pw blows up.
INJECT = {"map": 50.0, "clt": 80.0, "iat": 30.0, "lambda_1": 1.0,
          "fuel_pressure": 300.0, "battery": 13.5}        # 50 kPa, warm, 30 C, stoich, 300 kPa rail, 13.5 V
TTL_MS = 500           # refresh TTL so the injected signals stay fresh between onTicks


def ts(l: TsLink, name: str):
    """Telemetry read WITH the channel's scale applied (l.telem returns the raw integer)."""
    return l.telem(name) * l.meta.t(name)["scale"]


def lua_script(tps: float) -> str:
    """A tiny injector script: hold the absent sensors at sane values every tick (200 Hz)."""
    lines = [f'  signalWrite("{n}", {v}, {TTL_MS})' for n, v in INJECT.items()]
    lines.append(f'  signalWrite("tps", {tps}, {TTL_MS})')
    body = "\n".join(lines)
    return f"function onTick()\n{body}\nend\nsetTickRate(200)\n"


def set_fixed_rpm(sp, rpm: int):
    """ardustim 'F' command: software-override the pot, hold a steady FIXED rpm (little-endian word)."""
    sp.reset_input_buffer()
    sp.write(b"F" + struct.pack("<H", int(rpm)))
    time.sleep(0.3)


def wait_for_sync(l: TsLink, timeout_s=20.0):
    """Poll telemetry until CRANK sync (sync_level>=1) with a non-zero rpm, or time out."""
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        e = l.telem_all()
        if e.get("sync_level", 0) >= 1 and e.get("rpm", 0) > 0:
            return e
        time.sleep(0.5)
    return None


def main():
    rpm = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_RPM
    spec = WHEELS[WHEEL_IDX]
    fails = []
    def check(name, cond, detail):
        print(f"  [{'PASS' if cond else 'FAIL'}] {name}: {detail}")
        if not cond:
            fails.append(name)

    print(f"=== Bench fuel rig — {spec[0]} @ {rpm} rpm ===")

    # ---- 1. stim up: trigger config + spin the wheel ----
    sp = open_stim()
    l = TsLink(); configure(l, spec); l.cmd(b"E", b"reconfig"); l.close()
    select(sp, WHEEL_IDX, reps=4)
    set_fixed_rpm(sp, rpm)

    l = TsLink()
    e = wait_for_sync(l)
    if not e:
        print("  [FAIL] sync: decoder never reached CRANK sync — is the stim wired to the crank input?")
        l.close(); sp.close(); sys.exit(1)
    live_rpm = e["rpm"]
    check("sync", e["sync_level"] >= 1, f"sync_level={e['sync_level']} rpm={live_rpm:.0f}")

    # ---- 2. key-on + inject the absent sensors ----
    l.execute("key on")
    l.set_script(lua_script(tps=8.0))            # idle-ish throttle, steady (no transient)
    time.sleep(1.5)                              # let the injected signals settle + a few fuel cycles run
    err = l.get_debug()
    if err.strip():
        print("  ECU console:", err.strip()[:200])

    # ---- 3. read the computed fuel chain + assert sane (not no-sensor garbage) ----
    rpm_v   = ts(l, "rpm")
    ve      = ts(l, "ve")
    ctemp   = ts(l, "charge_temp")
    airmass = ts(l, "air_mass")
    base_pw = ts(l, "base_pw")
    inj_pw  = ts(l, "inj_pw")
    print(f"  steady @ {rpm_v:.0f} rpm, MAP=50 CLT=80 IAT=30, rail=300kPa:")
    print(f"    ve={ve:.1f}%  charge_temp={ctemp:.1f}C  air_mass={airmass:.2f}mg  base_pw={base_pw:.0f}us  inj_pw={inj_pw:.2f}ms")
    check("rpm",       rpm_v > 0,                 f"{rpm_v:.0f}")
    check("ve sane",   0 < ve < 200,              f"{ve:.1f}% (no-sensor garbage was ~525%)")
    check("charge_temp", 10 < ctemp < 120,        f"{ctemp:.1f} C")
    check("air_mass",  0 < airmass < 1000,        f"{airmass:.2f} mg/cyl")
    check("base_pw",   50 < base_pw < 50000,      f"{base_pw:.0f} us (plausible injector PW)")
    check("inj_pw",    inj_pw > 0,                f"{inj_pw:.2f} ms")

    # ---- 4. transient: step TPS up, watch the enrichment fire + decay ----
    print("  transient: stepping TPS 8 -> 45 ...")
    base_corr = ts(l, "fuel_corr_accel")
    l.set_script(lua_script(tps=45.0))           # tip-in step (live script reload)
    peak_corr, saw_active = base_corr, False
    for _ in range(40):                          # ~1 s of rapid polling to catch the spike
        c = ts(l, "fuel_corr_accel")
        if c > peak_corr: peak_corr = c
        if l.telem("transient_active") == 1: saw_active = True
        time.sleep(0.025)
    time.sleep(1.0)
    settled = ts(l, "fuel_corr_accel")
    print(f"    base_corr={base_corr:.3f}  peak={peak_corr:.3f}  active_seen={saw_active}  settled={settled:.3f}")
    check("transient enrich", peak_corr > 1.02,        f"fuel_corr_accel peaked {peak_corr:.3f} (>1 on tip-in)")
    check("transient active", saw_active,              "transient_active reached ENRICH")
    check("transient decay",  settled < peak_corr,     f"decayed to {settled:.3f} (< peak)")

    # ---- clean up: release key, drop the injector script ----
    l.restore_script()
    l.execute("key auto")
    l.close(); sp.close()

    print(f"\n=== {'ALL PASS' if not fails else 'FAILED: ' + ', '.join(fails)} ===")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
