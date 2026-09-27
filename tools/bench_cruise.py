#!/usr/bin/env python3
"""CRUISE CONTROL on the real ECU: the state machine, the conditions that drive it, and every reason it
refuses to engage.

*** cruise_demand IS A THROTTLE FLOOR. *** ElectronicThrottle takes max(pedal, cruise_demand), so a
cruise engagement on a bench with a live throttle body OPENS IT. This script disables every ETB before
it does anything and puts them back on exit — but unplug the throttle anyway, and watch cruise_demand
rather than throttle_demand.

WHAT IT DRIVES. Road speed and the stalk position are injected by a Lua script at PRIO_LUA, which
out-votes the sensor pipeline's PRIO_BASE, so this works whether or not a VSS or a real ladder is
fitted. The host steers both through two config scratch cells the script reads back, so the script is
pushed EXACTLY ONCE: pushing it is a config write, and every config write rebuilds every sensor
pipeline, which throws away what a band decoder had settled on (bench_multi_switch.py:58).

THE KEY MUST BE ON. DtcManager suppresses runtime codes while the system is inactive (USB/bench), so
every DTC assertion here would be vacuously true without `key on` — which is exactly the kind of green
that proves nothing.

    python3 tools/bench_cruise.py                 # the full sequence
    python3 tools/bench_cruise.py --monitor       # watch state/inhibit/target while you drive it by hand
"""
import argparse, struct, sys, time

sys.path.insert(0, ".")
from tools.ts_bench import TsLink, Meta

C = "cruise_control_"      # the meta flattens a module scalar as <module>_<field>

# The stalk this script pretends to have. Position 2 is BOTH Set and Speed Down, and 3 BOTH Resume and
# Speed Up — a "Set/Coast" and a "Resume/Accel" button, which is the arrangement the expression model
# exists to make possible, so it is the one worth proving on real hardware.
POSITIONS = {0: "rest", 1: "main", 2: "set/coast", 3: "resume/accel", 4: "cancel"}

STATE = {0: "Off", 1: "Disabled", 2: "Ready", 3: "Cruising", 4: "Fault"}

# The expression ISA, enough of it to write `cruise_sw == N` (firmware/Signal/ExprIsa.h:25).
OP_END, OP_PUSH_SIG, OP_PUSH_I8, OP_EQ = 0, 1, 5, 13
SIG_CRUISE_SW = 447          # generated/signal_ids.h — locked by `owner: firmware`


def eq_prog(sig_id, n):
    """The bytecode the studio compiles `cruise_sw == N` to. The studio owns the compiler, but a button
    condition is four instructions and a bench script that cannot set up its own inputs is not much of a
    bench script — so this writes them directly, exactly as the host tests do."""
    sel = sig_id + 1                                 # options_from:signals — 0 is None, else id+1
    return bytes([OP_PUSH_SIG, sel & 0xFF, sel >> 8, OP_PUSH_I8, n & 0xFF, OP_EQ, OP_END])

# One name per bit of cruise_inhibit, in CruiseControl::Inhibit order.
REASON = ["brake", "clutch", "handbrake", "below-min", "above-max", "no-speed", "speed-stale",
          "speed-jump", "wheel-diff", "switch-fault", "bad-condition", "runaway", "rpm-low",
          "rpm-high", "gear", "pedal-fault", "(vacant)", "unmonitored", "no-brake"]

# TWO BORROWED CELLS, and why these two. The host has no direct way to write a bus signal, so it writes
# config and a Lua script mirrors it onto the bus at PRIO_LUA — which out-votes the sensor pipeline, so
# this runs with or without a real VSS and a real ladder fitted. The cells are cruise's own max_error_ms
# and min_gear, chosen because BOTH are inert under this test's tune: max_error_ms is only read when
# max_error_kph is non-zero (set to 0 here) and min_gear only when gear_check_enabled is set (0 here).
# Borrowing a cell the module still reads would have the test fighting itself.
# getCalibration applies the field's scale, so max_error_ms arrives as milliseconds — the host sends
# kph x 10 and the script divides, which keeps a 0.1 kph resolution through a uint16.
# Position 8 is the SENTINEL for "stop publishing the stalk at all". Writing an out-of-range position
# would still be a VALID reading of a number no band covers, which is not the same fault as a wire that
# has stopped reporting — and the invalid case is the one the module has a code for.
LUA = """function onTick()
  local spd = getCalibration("cruise_control_max_error_ms") or 0   -- scratch A: kph x 10
  local pos = getCalibration("cruise_control_min_gear") or 0       -- scratch B: stalk position
  signalWrite("vehicle_spd", spd * 0.1, 300)
  if pos < 8 then signalWrite("cruise_sw", pos, 300) end
  -- The bench has no brake or clutch pedal switch fitted, and cruise reads an INVALID cancel input as
  -- PRESSED — correctly, that is the whole fail-safe. So they are published here as a released pedal.
  signalWrite("brake_sw", 0, 300)
  signalWrite("clutch_sw", 0, 300)
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


def reasons(mask):
    return tuple(n for i, n in enumerate(REASON) if mask & (1 << i)) or ("none",)


class Rig:
    """The ECU, plus the one place that knows how this script fakes a stalk and a speedometer."""

    def __init__(self, L):
        self.L = L
        self.fails = 0

    def read(self):
        g = lambda n: self.L.telem(n) * self.L.meta.t(n)["scale"]
        return {"state": int(g("cruise_state") + 0.5), "target": g("cruise_target"),
                "demand": g("cruise_demand"), "error": g("cruise_error_kph"),
                "inhibit": int(g("cruise_inhibit") + 0.5), "speed": g("vehicle_spd"),
                "sw": g("cruise_sw")}

    def drive(self, speed_kph=None, pos=None, settle=0.15):
        """Move the fake speedometer and/or the fake stalk, then let the ECU act on it."""
        if speed_kph is not None:
            self.L.set_config(C + "max_error_ms", int(round(speed_kph * 10)))   # scratch A
        if pos is not None:
            self.L.set_config(C + "min_gear", pos)                              # scratch B
        time.sleep(settle)
        return self.read()

    def tap(self, pos, speed_kph, hold=0.15):
        """A TAP is acted on when the button is RELEASED, so both halves are needed."""
        self.drive(speed_kph=speed_kph, pos=pos, settle=hold)
        return self.drive(pos=0)

    def hold(self, pos, speed_kph, secs):
        self.drive(speed_kph=speed_kph, pos=pos, settle=secs)
        return self.drive(pos=0)

    def check(self, what, ok, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}{('  — ' + detail) if detail else ''}")
        if not ok:
            self.fails += 1
        return ok

    def expect_state(self, want, s, what):
        return self.check(what, s["state"] == want,
                          f"state={STATE.get(s['state'], s['state'])} want={STATE[want]} "
                          f"inhibit={'|'.join(reasons(s['inhibit']))}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--monitor", action="store_true", help="print state/inhibit changes, assert nothing")
    a = ap.parse_args()

    L = TsLink(a.port)
    r = Rig(L)
    saved = {}
    saved_expr = {}
    saved_diag = {}
    etbs = []          # declared out here: the finally below runs even if the try never reaches them

    def save(*names):
        for n in names:
            saved[n] = L.get_config(n)

    try:
        # --- the throttle first, before anything can command a floor into it -----------------------
        etb_arr = L.meta.array("electronic_throttle", "etb")
        for i in range(etb_arr["count"]):
            off = L.meta.array_offset("electronic_throttle", "etb", i, "enabled")
            etbs.append((off, L.read_config_raw(off, 1)))
            L.write_raw(off, b"\x00")
        print(f"throttle: {len(etbs)} body(ies) disabled for the duration")

        L.execute("key on")     # runtime DTCs are SUPPRESSED while the system is inactive
        print("key: forced ON — without this every DTC assertion below is vacuous")

        save(C + "enabled", C + "power_on_state", C + "min_speed_kph", C + "max_speed_kph",
             C + "min_gear", C + "gear_check_enabled", C + "brake_sig", C + "clutch_sig",
             C + "handbrake_sig", C + "min_rpm", C + "max_rpm", C + "speed_increment_kph",
             C + "max_error_kph", C + "max_error_ms", C + "max_accel_kph_s", C + "sw_fault_ms",
             C + "long_press_ms")

        # --- the tune this test runs on --------------------------------------------------------
        # max_error_ms and min_gear are BORROWED as the two scratch cells (see LUA above), so the
        # runaway check and the gear gate are both switched off for the duration.
        # START FROM A KNOWN STATE. The module may well be enabled already — a burned tune has it
        # running, and on a bench with no road speed and a grounded switch pin it will have been sitting
        # in Fault since boot, with a latched reason. Switching it off takes it to Off, and the state
        # machine then decides the rest from power_on_state as if the key had just been turned.
        L.set_config(C + "enabled", 0)
        time.sleep(0.4)
        L.set_config(C + "min_speed_kph", 300)         # 30 kph
        L.set_config(C + "power_on_state", 0)          # starts Disabled, like a real car
        L.set_config(C + "gear_check_enabled", 0)
        L.set_config(C + "min_rpm", 0)
        L.set_config(C + "max_rpm", 0)
        L.set_config(C + "max_speed_kph", 2000)        # 200 kph
        L.set_config(C + "speed_increment_kph", 10)    # 1.0 kph a tap
        L.set_config(C + "long_press_ms", 500)
        L.set_config(C + "max_accel_kph_s", 0)         # the host steps the speed instantly
        L.set_config(C + "max_error_kph", 0)           # ... and holds a large error on purpose
        L.set_config(C + "sw_fault_ms", 300)

        # --- the stalk this script pretends to have, written as bytecode ---------------------------
        # Position 2 is BOTH Set and Speed Down, and 3 BOTH Resume and Speed Up. That is a Set/Coast and
        # a Resume/Accel button, which is the arrangement the expression model exists to make possible,
        # so it is the one worth proving on real hardware.
        WIRING = [("enable_disable_expr", 1), ("set_expr", 2), ("bump_down_expr", 2),
                  ("resume_expr", 3), ("bump_up_expr", 3), ("cancel_expr", 4)]
        for field, pos in WIRING:
            c = L.meta.c(C + field)
            prog = eq_prog(SIG_CRUISE_SW, pos)
            saved_expr[field] = L.read_config_raw(c["offset"], c["size"])
            L.write_raw(c["offset"], prog + b"\x00" * (c["size"] - len(prog)))
        for field in ("enable_expr", "disable_expr"):     # unused here: the toggle does both
            c = L.meta.c(C + field)
            saved_expr[field] = L.read_config_raw(c["offset"], c["size"])
            L.write_raw(c["offset"], b"\x00" * c["size"])
        print("stalk: " + ", ".join(f"{f.replace('_expr','')}=pos{p}" for f, p in WIRING))

        # ARM THE CHECKS on the two interlock sensors. Cruise refuses to run on an unmonitored safety
        # input, and diag_enable ships as 0 — so without this the rig sits in Fault on P17A8, which is
        # the rule doing its job and not something to work around quietly.
        sen = L.meta.array("sensors", "sensor")
        ids = sen.get("element_ids") or []
        for sig in ("brake_sw", "clutch_sw"):
            if sig in ids:
                off = L.meta.array_offset("sensors", "sensor", ids.index(sig), "diag_enable")
                saved_diag[off] = L.read_config_raw(off, 1)
                L.write_raw(off, b"\x01")            # Detect Raw Low — something is watching
        print(f"sensors: fault checks armed on {len(saved_diag)} interlock input(s)")

        L.set_script(LUA)
        time.sleep(1.0)
        L.set_config(C + "enabled", 1)                 # ...and only now does it start, cleanly
        time.sleep(0.4)

        if a.monitor:
            last = None
            print("monitoring — drive the stalk by hand (Ctrl-C to stop)")
            while True:
                s = r.read()
                key = (s["state"], s["inhibit"], round(s["target"], 1))
                if key != last:
                    print(f"  {STATE.get(s['state'],'?'):9} target={s['target']:6.1f} "
                          f"demand={s['demand']:5.1f} err={s['error']:6.1f} "
                          f"why={'|'.join(reasons(s['inhibit']))}")
                    last = key
                time.sleep(0.1)

        # --- the sequence ---------------------------------------------------------------------
        print("1 · it starts Disabled, and Set alone will not engage it")
        s = r.drive(speed_kph=100, pos=0)
        r.expect_state(1, s, "Disabled at power-on")
        s = r.tap(2, 100)
        r.expect_state(1, s, "Set while Disabled does nothing")

        print("2 · the main button arms it, and Set engages at the speed it was pressed at")
        s = r.tap(1, 100)
        r.expect_state(2, s, "main button -> Ready")
        s = r.tap(2, 100)
        r.expect_state(3, s, "Set -> Cruising")
        r.check("latched the current speed", abs(s["target"] - 100.0) < 1.5, f"target={s['target']:.1f}")

        print("3 · one position is Set AND Coast, and a tap does only the one it was pressed in")
        s = r.tap(2, 100)
        r.check("tapped again while cruising, it coasts one increment",
                abs(s["target"] - 99.0) < 1.5, f"target={s['target']:.1f}")

        print("4 · a tap is one increment; the same button HELD ramps instead")
        s = r.tap(3, 100)
        before = s["target"]
        r.check("tap up = one increment", abs(before - 100.0) < 1.5, f"target={before:.1f}")
        s = r.hold(3, 100, 2.0)
        r.check("held, it ramps further than a tap", s["target"] > before + 1.0,
                f"{before:.1f} -> {s['target']:.1f}")

        print("5 · Cancel keeps the set speed; Resume returns to it")
        kept = s["target"]
        s = r.tap(4, 100)
        r.expect_state(2, s, "Cancel -> Ready")
        r.check("the set speed survives a Cancel", abs(s["target"] - kept) < 1.5,
                f"target={s['target']:.1f} kept={kept:.1f}")
        s = r.tap(3, 90)
        r.expect_state(3, s, "Resume -> Cruising")
        r.check("resumed to the KEPT speed, not the current one", abs(s["target"] - kept) < 1.5,
                f"target={s['target']:.1f}")

        print("6 · the speed window cancels, and says which end")
        s = r.drive(speed_kph=250)
        r.expect_state(2, s, "above the maximum -> Ready")
        r.check("and it names the reason", bool(s["inhibit"] & (1 << 4)),
                "|".join(reasons(s["inhibit"])))
        r.drive(speed_kph=100)
        r.tap(2, 100)

        print("7 · a switch that stops reporting FAULTS, and says so")
        # THE LUA INJECTION OVERRIDES THE DECODED SIGNAL, not the pin, so it cannot simulate a reading
        # in no band — that is the decoder's verdict and only a real voltage can provoke it. What it CAN
        # do is go silent. But silence only reaches the module if nothing else is publishing, and a
        # configured cruise_sw sensor is: on this bench its pin is grounded, which now decodes as a
        # legitimate position. So the sensor is switched off for this section and put back after.
        sen_en = L.meta.array_offset("sensors", "sensor",
                                     L.meta.array("sensors", "sensor")["element_ids"].index("cruise_sw"),
                                     "enabled")
        sen_was = L.read_config_raw(sen_en, 1)
        L.write_raw(sen_en, b"\x00")
        s = r.drive(pos=8)                             # the sentinel: Lua stops publishing too
        time.sleep(0.8)                                # past sw_fault_ms, and past the TTL
        s = r.read()
        r.expect_state(4, s, "undecodable switch -> Fault")
        r.check("P17A2 is active", "P17A2" in active_dtcs(L), " ".join(active_dtcs(L)))
        r.check("a fault discards the set speed", s["target"] < 1.0, f"target={s['target']:.1f}")

        print("8 · a fault holds until it is acknowledged, and never resumes straight into Cruising")
        L.write_raw(sen_en, sen_was)                   # the switch is reporting again
        r.drive(pos=0)
        time.sleep(0.5)
        s = r.read()
        r.expect_state(4, s, "cause gone, but nothing pressed: still Fault")
        s = r.tap(2, 100)
        r.expect_state(2, s, "acknowledged -> Ready, NOT Cruising")
        r.check("P17A2 healed", "P17A2" not in active_dtcs(L), " ".join(active_dtcs(L)))

        print("9 · with no brake input assigned, it will not arm at all")
        L.set_config(C + "brake_sig", -1)
        time.sleep(0.3)
        s = r.tap(1, 100)
        r.check("No Brake is named", bool(s["inhibit"] & (1 << 18)), "|".join(reasons(s["inhibit"])))
        r.check("P17A9 is active", "P17A9" in active_dtcs(L), " ".join(active_dtcs(L)))
        s = r.tap(2, 100)
        r.check("and Set does nothing", s["state"] != 3, STATE.get(s["state"], "?"))

        print(f"\n{'PASS' if r.fails == 0 else str(r.fails) + ' FAILED'}")
        return 1 if r.fails else 0

    finally:
        for off, raw in saved_diag.items():
            try:
                L.write_raw(off, raw)
            except Exception:
                print(f"  !! could not restore the sensor checks at {off}")
        for field, raw in saved_expr.items():
            try:
                L.write_raw(L.meta.c(C + field)["offset"], raw)
            except Exception:
                print(f"  !! could not restore {field}")
        for n, v in saved.items():
            try:
                L.set_config(n, v)
            except Exception:
                print(f"  !! could not restore {n}")
        for off, raw in etbs:
            try:
                L.write_raw(off, raw)
            except Exception:
                print(f"  !! could not restore the throttle enable at {off}")
        try:
            L.restore_script()
        except Exception:
            print("  !! could not restore the Lua slot")
        try:
            L.execute("key auto")
        except Exception:
            pass
        print("restored: cruise config + conditions, sensor checks, throttle bodies, Lua slot, key = auto")


if __name__ == "__main__":
    sys.exit(main())
