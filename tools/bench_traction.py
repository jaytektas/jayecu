#!/usr/bin/env python3
"""TRACTION CONTROL on the real ECU: what it measures, what it pulls, and what it refuses to do.

*** IT CUTS. *** Over a big enough slip error this module publishes ign_cut/fuel_cut as a duty. The
engine is not running on a bench, so that is harmless here — but do not run this with an engine turning.
It also publishes traction_cap, which ElectronicThrottle consumes as a CEILING (not a floor, unlike
cruise), so it can only ever close a throttle. Every ETB is disabled for the duration anyway.

WHAT IT DRIVES. The four wheel speeds and the road speed are injected by a Lua script at PRIO_LUA,
which out-votes the sensor pipeline's PRIO_BASE — so this runs on a bench with no wheel pickups fitted
at all. The host steers the script through three config scratch cells it reads back, so the script is
pushed EXACTLY ONCE: pushing it is a config write, and every config write rebuilds every sensor
pipeline (bench_multi_switch.py:58).

THE CELLS ARE BORROWED FROM CRUISE, which is switched off for the duration, so all three are inert.
Traction control has no inert cell of its own — every field it owns is read on every frame — and
borrowing one the module still reads would have the test fighting itself.

THE KEY MUST BE ON. DtcManager suppresses runtime codes while the system is inactive (USB/bench), so
every DTC assertion here would be vacuously true without it.

    python3 tools/bench_traction.py
    python3 tools/bench_traction.py --monitor     # watch slip/cap/retard/cut while you drive it by hand
"""
import argparse, struct, sys, time

sys.path.insert(0, ".")
from tools.ts_bench import TsLink

T = "traction_control_"      # the meta flattens a module scalar as <module>_<field>
C = "cruise_control_"

# The three borrowed cells. Scales are the FIELD's, and getCalibration applies them, so the script reads
# engineering units and the host writes them: max_error_kph and max_accel_kph_s are both 0.1, so a speed
# goes across as kph x 10 and comes back as kph.
REF_CELL  = C + "max_error_kph"      # scratch A: the reference (front) wheels, kph
DRV_CELL  = C + "max_accel_kph_s"    # scratch B: the driven (rear) wheels, kph
MODE_CELL = C + "sw_fault_ms"        # scratch C: which wheels the script publishes at all

# MODE 3 is how the ROAD SPEED goes away, which is the only thing that stops this module now: it has no
# reference setting of its own, it reads whatever Vehicle Speed publishes.
# MODE IS HOW A WHEEL GOES AWAY. Writing a speed of zero is a valid reading of a stopped wheel, which is
# not the same fault as a pickup that has stopped reporting — and the invalid case is the one the module
# has a code for. So the script simply stops publishing, and the 300 ms ttl expires it.
MODE_ALL, MODE_NO_FRONTS, MODE_SPLIT_REARS, MODE_NO_ROAD_SPEED = 0, 1, 2, 3

LUA = """function onTick()
  local ref  = getCalibration("cruise_control_max_error_kph") or 0
  local drv  = getCalibration("cruise_control_max_accel_kph_s") or 0
  local mode = getCalibration("cruise_control_sw_fault_ms") or 0
  if mode ~= 1 and mode ~= 3 then
    signalWrite("wheel_fl", ref, 300)
    signalWrite("wheel_fr", ref, 300)
  end
  if mode == 2 then
    signalWrite("wheel_rl", drv * 0.75, 300)   -- one rear gripping...
    signalWrite("wheel_rr", drv, 300)          -- ...and one spinning: an open diff
  else
    signalWrite("wheel_rl", drv, 300)
    signalWrite("wheel_rr", drv, 300)
  end
  if mode ~= 3 then signalWrite("vehicle_spd", ref, 300) end
end
setTickRate(200)
"""


def active_dtcs(L):
    """Active codes from the DTC table image ('G'): 36-byte records from offset 10, status bit0 = active."""
    _, img = L.cmd(b"G")
    n = struct.unpack_from("<H", img, 8)[0]
    out = []
    for i in range(n):
        off = 10 + 36 * i
        if off + 36 > len(img):
            break
        code = struct.unpack_from("<H", img, off)[0]
        if code and img[off + 4] & 1:
            out.append(f"P{code:04X}")
    return tuple(sorted(out))


class Rig:
    def __init__(self, L):
        self.L = L
        self.fails = 0

    def read(self):
        g = lambda n: self.L.telem(n) * self.L.meta.t(n)["scale"]
        return {"slip": g("traction_slip"), "err": g("traction_slip_err"), "cap": g("traction_cap"),
                "retard": g("traction_retard"), "cut": g("traction_cut_pct"), "spd": g("vehicle_spd"),
                "rl": g("wheel_rl"), "rr": g("wheel_rr")}

    def drive(self, ref=None, drv=None, mode=None, settle=0.25):
        if mode is not None: self.L.set_config(MODE_CELL, mode)
        if ref is not None:  self.L.set_config(REF_CELL, int(round(ref * 10)))
        if drv is not None:  self.L.set_config(DRV_CELL, int(round(drv * 10)))
        time.sleep(settle)
        return self.read()

    def check(self, what, ok, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}{('  — ' + detail) if detail else ''}")
        if not ok:
            self.fails += 1
        return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--monitor", action="store_true", help="print live values, assert nothing")
    a = ap.parse_args()

    L = TsLink(a.port)
    r = Rig(L)
    saved, etbs = {}, []

    def save(*names):
        for n in names:
            saved[n] = L.get_config(n)

    try:
        etb_arr = L.meta.array("electronic_throttle", "etb")
        for i in range(etb_arr["count"]):
            off = L.meta.array_offset("electronic_throttle", "etb", i, "enabled")
            etbs.append((off, L.read_config_raw(off, 1)))
            L.write_raw(off, b"\x00")
        print(f"throttle: {len(etbs)} body(ies) disabled for the duration")

        L.execute("key on")
        print("key: forced ON — without this every DTC assertion below is vacuous")

        save(T + "enabled", T + "driven_axle", T + "min_speed_kph",
             T + "kp", T + "ki", T + "cap_min_pct", T + "release_pct_s", T + "cut_method",
             C + "enabled", REF_CELL, DRV_CELL, MODE_CELL, "vehicle_speed_enabled")

        # Cruise off: its three cells are this script's scratch space, and an enabled cruise would be
        # reading them as a runaway limit and a switch-fault window while we use them as speeds.
        L.set_config(C + "enabled", 0)
        # VehicleSpeed off too — it would publish wheel_* from pickups that are not fitted, at PRIO_BASE,
        # which is out-voted anyway but makes the trace harder to read than it needs to be.
        L.set_config("vehicle_speed_enabled", 0)
        time.sleep(0.3)

        L.set_config(T + "enabled", 1)
        L.set_config(T + "driven_axle", 1)      # rear
        L.set_config(T + "min_speed_kph", 5)
        L.set_config(T + "cap_min_pct", 300)    # 30%
        L.set_config(T + "release_pct_s", 100)  # 100 %/s — slow enough to SEE the release
        L.set_config(T + "cut_method", 1)       # Ignition

        L.set_script(LUA)
        print("lua: wheel speeds + road speed injected at PRIO_LUA (pushed once)")
        # START FROM A KNOWN CEILING. The scratch cells hold whatever the last run left in them, so
        # between the push and the first measurement the module may have been looking at a large slip
        # and pulled the ceiling to its floor — and GIVING IT BACK is deliberately slow (release_pct_s
        # is 100 %/s here, so 70% of ceiling takes 0.7 s). Reading the first assertion before that has
        # finished measures the previous run, not this one.
        r.drive(ref=100, drv=100, mode=MODE_ALL, settle=2.0)

        if a.monitor:
            print("monitoring — ^C to stop")
            while True:
                s = r.read()
                print(f"  slip {s['slip']:6.1f}%  err {s['err']:6.1f}%  cap {s['cap']:5.1f}%  "
                      f"retard {s['retard']:4.1f}deg  cut {s['cut']:5.1f}%", end="\r")
                time.sleep(0.2)

        print("\n--- measurement")
        s = r.read()
        r.check("no slip: no cap, no retard, no cut",
                s["cap"] > 99.5 and s["retard"] < 0.05 and s["cut"] < 0.05,
                f"slip={s['slip']:.1f} cap={s['cap']:.1f} retard={s['retard']:.1f} cut={s['cut']:.1f}")

        s = r.drive(drv=130)
        r.check("30% slip is measured as 30%", abs(s["slip"] - 30.0) < 1.5, f"slip={s['slip']:.1f}")

        s = r.drive(drv=130, mode=MODE_SPLIT_REARS)
        # One rear at 130 and one at 97.5. The FASTEST driven wheel is the answer, so this is still 30%;
        # averaging the pair — what an open diff would hide behind — would read about 13%.
        #
        # THE TEST PROVES ITS OWN PREMISE FIRST. With the two rears equal this assertion is not wrong,
        # it is VACUOUS: fastest and average are the same number, and it passes whatever the firmware
        # does. Found by trying to make it go red and watching it stay green.
        r.check("...and the rears really are split, or the check above proves nothing",
                s["rr"] - s["rl"] > 20.0, f"rl={s['rl']:.1f} rr={s['rr']:.1f}")
        r.check("an open diff: the FASTEST driven wheel is the slip, not the pair's average",
                abs(s["slip"] - 30.0) < 1.5, f"slip={s['slip']:.1f} (an average would read ~13)")

        print("--- the throttle ceiling")
        r.drive(ref=100, drv=100, mode=MODE_ALL, settle=0.6)     # let it fully release first
        s1 = r.drive(drv=110, settle=0.12)
        first = s1["cap"]
        r.check("over target the ceiling comes down", first < 99.5, f"cap={first:.1f}")
        s2 = r.drive(settle=1.2)
        r.check("...and the integral keeps pulling while the slip stays", s2["cap"] < first - 0.2,
                f"cap {first:.1f} -> {s2['cap']:.1f} (a P-only loop would sit still)")

        s3 = r.drive(drv=400, settle=1.5)
        floor = L.get_config(T + "cap_min_pct") * 0.1
        r.check("...but never below its floor", abs(s3["cap"] - floor) < 1.0,
                f"cap={s3['cap']:.1f} floor={floor:.1f}")

        print("--- and how it lets go")
        s4 = r.drive(drv=100, settle=0.12)      # slip gone, this instant
        r.check("the ceiling is GIVEN BACK, not snapped back",
                s4["cap"] > floor and s4["cap"] < 99.0,
                f"cap={s4['cap']:.1f} — 100 would mean it snapped")
        s5 = r.drive(settle=2.0)
        r.check("...and it does get all the way back", s5["cap"] > 99.5, f"cap={s5['cap']:.1f}")

        print("--- the fast paths")
        s = r.drive(drv=100, settle=0.5)
        r.check("no retard and no cut with no slip", s["retard"] < 0.05 and s["cut"] < 0.05,
                f"retard={s['retard']:.1f} cut={s['cut']:.1f}")
        s = r.drive(drv=160, settle=0.4)
        r.check("a big slip error pulls timing", s["retard"] > 0.5, f"retard={s['retard']:.1f}deg")
        r.check("...and asks for a cut", s["cut"] > 0.5, f"cut={s['cut']:.1f}%")

        print("--- the road speed IS the reference")
        r.drive(ref=100, drv=100, settle=0.4)
        # The script stops publishing the FRONTS, and stops publishing vehicle_spd with them — the ttl
        # expires both. No road speed is the only thing that stops this module now.
        s = r.drive(mode=MODE_NO_ROAD_SPEED, settle=0.6)
        codes = active_dtcs(L)
        r.check("no road speed: no cap, and P17B0 says why",
                s["cap"] > 99.5 and "P17B0" in codes, f"cap={s['cap']:.1f} codes={codes or ('none',)}")

        # …and the whole point of having ONE answer: the front pickups are still gone, but Vehicle Speed
        # is publishing a road speed from somewhere else — a gearbox pickup, GPS, anything — and traction
        # control simply works. This is the two-pickup car and the four-wheel-drive car on GPS, with no
        # setting of its own to get wrong.
        s = r.drive(drv=110, mode=MODE_NO_FRONTS, settle=0.6)
        codes = active_dtcs(L)
        r.check("no undriven pickups, but a road speed: it works, with nothing to configure",
                abs(s["slip"] - 10.0) < 1.5 and "P17B0" not in codes,
                f"slip={s['slip']:.1f} codes={codes or ('none',)}")

        print(f"\n{'PASS' if r.fails == 0 else str(r.fails) + ' FAILED'}")
        return 1 if r.fails else 0

    finally:
        for n, v in saved.items():
            try: L.set_config(n, v)
            except Exception: pass
        for off, v in etbs:
            try: L.write_raw(off, v)
            except Exception: pass
        try: L.restore_script()   # the slot belongs to whoever loaded it; a bench only borrows it
        except Exception: pass
        print("restored: config, throttle bodies, lua")


if __name__ == "__main__":
    sys.exit(main())
