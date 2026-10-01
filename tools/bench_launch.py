#!/usr/bin/env python3
"""LAUNCH CONTROL on the real ECU: the arm expression, the End RPM table, both cut shapes, and the two
maps the engine runs on while it holds.

WHY THESE. They are the ones a host test cannot settle.

  the arm expression  the host builds the bytecode in the same process it runs. Here it goes through
                      the config protocol into a 64-byte block, and the module has to notice the
                      generation bump, revalidate and then read it every frame.
  the End RPM table   the limit is now a TABLE, and the module compares it against `pos.rpm` — the
                      DECODER's rpm, off the trigger — while the table's own axes read the bus. Only a
                      spinning wheel exercises both halves.
  the cut shapes      a soft cut is a duty SPREAD across frames (CutDuty). On the host that is a loop
                      counting frames; on the ECU it is a signal published one frame wide and released
                      by its ttl, which is where a doubled duty would show up.
  the ignition map    Launch publishes an absolute angle and IGNITION has to take it as the base in
                      place of the main map. That hand-off is two modules at two cadences, and nothing
                      but the ECU runs both.

HOW IT DRIVES. The launch switch is not fitted on this bench, so a Lua script publishes it at PRIO_LUA
from a config scratch cell the host writes. RPM IS SPUN, not injected: the limiter compares the
decoder's rpm, so injecting the bus channel alone would move the table axes and leave the cut asleep.

THE TUNE IS RESTORED ON EXIT, and nothing is burned: every write here lives in ECU RAM, so a reset
undoes the lot even if this script dies halfway.

    python3 tools/bench_launch.py
    python3 tools/bench_launch.py --keep     # leave the test tune in RAM to poke at by hand
"""
import argparse, struct, sys, time

sys.path.insert(0, ".")
from tools.ts_bench import TsLink, Meta
from tools.gen_rebench import open_stim, select, configure, WHEELS

WHEEL_IDX = 6          # '36-1' — dense, locks in ~7 s, single crank stream (no cam needed)

B = "launch_"          # the meta flattens a module scalar as <module>_<field>

# The expression ISA, enough of it to write `launch_sw != 0` (firmware/Signal/ExprIsa.h:25).
OP_END, OP_PUSH_SIG, OP_PUSH_I8, OP_NE = 0, 1, 5, 14

# THE SCRATCH CELL the Lua script reads, and it has to be one the module NEVER consults for the whole
# run. cut_adder_rpm looked inert and is not — the stagger section needs it for real — and borrowing it
# left the switch stuck on for the three sections after that one. The End RPM table's Y-axis channel
# selector is dead for the entire test instead: the axis ships off and stays off, and TableEval
# collapses a disabled axis to index 0 without ever fetching its coordinate (TableEval.h:131).
CELL_SW = "end_rpm_table_y_src"

LUA = """function onTick()
  signalWrite("launch_sw", (getCalibration("launch_end_rpm_table_y_src") or 0) > 0.5 and 1 or 0, 300)
  signalWrite("map", 100, 300)
end
setTickRate(200)
"""

FAILED = []


def ne_prog(sig_id):
    """The bytecode the studio compiles `launch_sw != 0` to — a plain switch, which is what four of the
    reference's five wiring positions are."""
    sel = sig_id + 1                                 # options_from:signals — 0 is None, else id+1
    return bytes([OP_PUSH_SIG, sel & 0xFF, sel >> 8, OP_PUSH_I8, 0, OP_NE, OP_END])


def ts(l, meta, name):
    """Telemetry in ENGINEERING units — read_telemetry hands back the raw frame, so the channel's own
    scale has to be applied."""
    return l.telem(name) * (meta.t(name).get("scale", 1.0) or 1.0)


def wait_for_sync(l, timeout_s=25.0):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        if l.telem("sync_level") >= 1 and l.telem("rpm") > 0:
            return True
        time.sleep(0.2)
    return False


def check(ok, what, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {what}{('  — ' + detail) if detail else ''}")
    if not ok:
        FAILED.append(what)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--keep", action="store_true", help="leave the test tune in RAM on exit")
    args = ap.parse_args()

    meta = Meta()

    # THE WHEEL FIRST. configure() writes the trigger config, and every config write rebuilds every
    # pipeline — so it happens on its own link before the test tune goes in.
    print("spinning the stim...")
    sp = open_stim()
    spec = WHEELS[WHEEL_IDX]
    l0 = TsLink(args.port); configure(l0, spec); l0.cmd(b"E", b"reconfig"); l0.close()
    select(sp, WHEEL_IDX, reps=4)

    def spin(rpm):
        sp.reset_input_buffer()
        sp.write(b"F" + struct.pack("<H", int(rpm)))   # ardustim 'F': hold a fixed rpm
        time.sleep(0.3)

    spin(3000)
    L = TsLink(args.port)
    print(L.hello())
    if not wait_for_sync(L):
        print("  the decoder never reached sync — is the stim wired to the crank input?")
        L.close(); sp.close()
        return 1
    print(f"  synced at {L.telem('rpm'):.0f} rpm")

    saved, saved_expr, saved_cells = {}, {}, {}

    def raw_of(c, eng):
        if c["datatype"] == "F32":
            return eng
        sc = c.get("scale", 1.0) or 1.0
        return int(round(eng / sc))

    def setc(name, value):
        """Set a launch field in ENGINEERING units, remembering its raw value for the restore."""
        full = B + name
        if full not in saved:
            saved[full] = L.get_config(full)
        L.set_config(full, raw_of(meta.c(full), value))

    def setcell(table, index, eng):
        """One cell of a table or axis. A table is ONE meta entry with one offset, so the named-scalar
        path cannot address a cell: this is offset + index * cell size, written raw and restored the
        same way."""
        c = meta.c(B + table)
        off = c["offset"] + index * meta.size(c["datatype"])
        if off not in saved_cells:
            saved_cells[off] = L.read_config_raw(off, meta.size(c["datatype"]))
        L.write_raw(off, struct.pack(meta.fmt(c["datatype"]), raw_of(c, eng)))

    def set_expr(field, prog):
        c = meta.c(B + field)
        if field not in saved_expr:
            saved_expr[field] = L.read_config_raw(c["offset"], c["size"])
        L.write_raw(c["offset"], prog + b"\x00" * (c["size"] - len(prog)))

    def arm(on):
        """The launch switch, through the scratch cell the Lua script mirrors onto the bus."""
        setc(CELL_SW, 1 if on else 0)
        time.sleep(0.3)

    def at(rpm, settle=0.5):
        spin(rpm)
        time.sleep(settle)

    def cutting(name, samples=40):
        """How many of `samples` telemetry frames show the cut asserted. A soft cut is a DUTY spread
        across frames, so one reading says nothing — this is the only way to see a percentage."""
        n = 0
        for _ in range(samples):
            if L.telem(name) >= 0.5:
                n += 1
            time.sleep(0.02)
        return n

    try:
        L.execute("key on")
        L.set_script(LUA)
        time.sleep(0.8)

        # A tune this script can reason about. Everything the module might read is written, not assumed.
        setc("enabled", 0)                    # start from a known state before the maps go in
        time.sleep(0.3)
        for k, v in [("timeout_s", 0), ("cut_method", 0), ("cut_type", 0), ("cut_range_rpm", 500),
                     ("cut_lead", 0), ("cut_adder_rpm", 0), ("resume_band_rpm", 200), (CELL_SW, 0)]:
            setc(k, v)
        # A flat 5000 rpm limit with both axes off — the shipped shape, stated rather than assumed.
        setc("end_rpm_table_x_en", 0)
        setc("end_rpm_table_y_en", 0)
        setcell("end_rpm_table", 0, 5000)
        # Flat maps, so a section that is not about the maps cannot be moved by them. The ignition map
        # is written across the whole 8x8 because its Y axis is live and the bench's load is whatever
        # the injected MAP makes it.
        for i in range(64):
            setcell("ign_advance_table", i, 15.0)
            setcell("fuel_corr_table", i, 0.0)

        set_expr("arm_expr", ne_prog(meta.signals["launch_sw"]))
        setc("enabled", 1)
        time.sleep(0.5)

        print("\n--- the arm expression ---")
        arm(False)
        at(3000)
        off_active = ts(L, meta, "launch_active")
        arm(True)
        at(3000)
        on_active = ts(L, meta, "launch_active")
        check(off_active < 0.5, "switch open: not armed", f"launch_active={off_active:.0f}")
        check(on_active > 0.5, "switch closed: armed", f"launch_active={on_active:.0f}")
        end = ts(L, meta, "launch_end_rpm")
        check(abs(end - 5000.0) < 1.0, "the End RPM table is read", f"{end:.0f} RPM")

        print("\n--- the End RPM is the limit, and the cut is total at it ---")
        at(4000, settle=0.8)
        below = cutting("fuel_cut", 25)
        at(5400, settle=0.8)
        above = cutting("fuel_cut", 25)
        check(below == 0, "under the limit nothing is cut", f"{below}/25 frames")
        check(above == 25, "at and over it every cylinder is", f"{above}/25 frames")
        check(ts(L, meta, "launch_cut_pct") > 99.0, "…and it says so",
              f"{ts(L, meta, 'launch_cut_pct'):.0f} %")
        check(ts(L, meta, "ign_cut") < 0.5, "the fuel method leaves the spark alone")

        print("\n--- the End RPM table moves with its axis ---")
        # Switch the load axis on and give two bins that the injected MAP can sit between. This is the
        # synthesised axis selector doing its job: the same table, re-pointed and resized, with no
        # firmware change anywhere.
        setc("end_rpm_table_x_en", 1)
        setc("end_rpm_table_x_src", meta.signals["map"])
        for i, kpa in enumerate([50, 100, 150, 200, 250, 300, 350, 400]):
            setcell("end_load_axis", i, kpa)
        setcell("end_rpm_table", 0, 3000)          # 50 kPa
        setcell("end_rpm_table", 1, 6500)          # 100 kPa — where the Lua script holds MAP
        at(4000, settle=0.8)
        moved = ts(L, meta, "launch_end_rpm")
        check(abs(moved - 6500.0) < 50.0, "the limit follows the load axis", f"{moved:.0f} RPM")
        check(cutting("fuel_cut", 20) == 0, "…and 4000 rpm is no longer over it")
        setc("end_rpm_table_x_en", 0)
        setcell("end_rpm_table", 0, 5000)
        time.sleep(0.4)

        print("\n--- soft cut is a duty, not a second hard cut ---")
        setc("cut_type", 1)
        setc("cut_range_rpm", 500)                 # 4500..5000
        at(4400, settle=0.8)
        check(cutting("fuel_cut", 30) == 0, "below the range: nothing")
        # AGAINST THE ECU'S OWN RPM, not the one the stim was asked for. The wheel runs a little fast
        # — 4600 commanded reads ~4655 — and a fifth of a 500 rpm band is 10 rpm per per cent, so a
        # fixed window around the commanded figure fails on a module that is exactly right.
        def duty_at(cmd_rpm):
            at(cmd_rpm, settle=0.8)
            rpm = ts(L, meta, "rpm")
            pct = ts(L, meta, "launch_cut_pct")
            want = max(0.0, min(100.0, (rpm - 4500.0) / 500.0 * 100.0))
            return rpm, pct, want
        rpm_l, low_pct, want_l = duty_at(4600)
        rpm_h, high_pct, want_h = duty_at(4900)
        check(abs(low_pct - want_l) < 6.0, "a fifth of the way in is about a fifth cut",
              f"{low_pct:.0f} % at {rpm_l:.0f} rpm (want {want_l:.0f} %)")
        check(abs(high_pct - want_h) < 6.0, "four fifths in is about four fifths",
              f"{high_pct:.0f} % at {rpm_h:.0f} rpm (want {want_h:.0f} %)")
        # …and the duty is DELIVERED, not just reported: the frames it cuts have to track it.
        at(4750, settle=0.8)
        mid = cutting("fuel_cut", 60)
        check(10 < mid < 50, "and the frames it cuts follow the duty", f"{mid}/60 frames")
        setc("cut_type", 0)

        print("\n--- the stagger puts the second cut above the first ---")
        setc("cut_method", 2)                      # Both
        setc("cut_lead", 0)                        # ignition leads, fuel follows the adder
        setc("cut_adder_rpm", 300)                 # ignition at 5000, fuel at 5300
        at(5150, settle=0.8)
        ign_first = cutting("ign_cut", 20), cutting("fuel_cut", 20)
        at(5500, settle=0.8)
        both = cutting("ign_cut", 20), cutting("fuel_cut", 20)
        check(ign_first[0] == 20 and ign_first[1] == 0,
              "between the two thresholds only the leading cut acts", f"ign={ign_first[0]}/20 fuel={ign_first[1]}/20")
        check(both[0] == 20 and both[1] == 20, "past the adder both do",
              f"ign={both[0]}/20 fuel={both[1]}/20")
        setc("cut_method", 0)
        setc("cut_adder_rpm", 0)

        print("\n--- the launch ignition map IS the advance ---")
        # Below the limit, so nothing is being cut and the angle is what the engine would actually run.
        at(4000, settle=0.8)
        for i in range(64):
            setcell("ign_advance_table", i, 12.0)
        time.sleep(0.5)
        adv12 = ts(L, meta, "launch_ign_adv")
        cmd12 = ts(L, meta, "advance")
        kind = ts(L, meta, "ign_base_kind")
        for i in range(64):
            setcell("ign_advance_table", i, -5.0)
        time.sleep(0.5)
        advm5 = ts(L, meta, "launch_ign_adv")
        cmdm5 = ts(L, meta, "advance")
        check(abs(adv12 - 12.0) < 0.3, "the map is published as an absolute angle", f"{adv12:.1f} deg")
        check(abs(cmd12 - 12.0) < 0.5, "and Ignition commands it, not the main map", f"{cmd12:.1f} deg")
        check(abs(kind - 2.0) < 0.1, "…and says which table it came from", f"ign_base_kind={kind:.0f}")
        check(abs(advm5 + 5.0) < 0.3, "a cell past TDC is published as it is", f"{advm5:.1f} deg")
        check(abs(cmdm5 + 5.0) < 0.5, "…and commanded — no retard could have produced it",
              f"{cmdm5:.1f} deg")
        arm(False)
        time.sleep(0.5)
        released = ts(L, meta, "advance")
        check(abs(released - cmdm5) > 1.0, "disarming hands the engine back to the main map",
              f"{released:.1f} deg")
        arm(True)

        print("\n--- the launch fuel map is a percentage on the charge ---")
        for i in range(64):
            setcell("fuel_corr_table", i, 10.0)
        time.sleep(0.5)
        mult = ts(L, meta, "fuel_corr_launch")
        check(abs(mult - 1.10) < 0.01, "+10 % is a 1.10x multiplier", f"{mult:.3f} x")
        for i in range(64):
            setcell("fuel_corr_table", i, 0.0)
        time.sleep(0.5)
        neutral = ts(L, meta, "fuel_corr_launch")
        check(abs(neutral - 1.0) < 0.01, "and zero is 1.000x, not zero fuel", f"{neutral:.3f} x")

        print("\n--- the timeout ends a launch, and will not restart it on its own ---")
        setc("timeout_s", 2)
        arm(False); time.sleep(0.4)
        arm(True)
        at(5200, settle=0.5)
        started = ts(L, meta, "launch_active")
        time.sleep(2.5)
        expired = ts(L, meta, "launch_active")
        time.sleep(2.0)
        still = ts(L, meta, "launch_active")
        arm(False); time.sleep(0.4); arm(True); time.sleep(0.4)
        again = ts(L, meta, "launch_active")
        check(started > 0.5, "a launch starts")
        check(expired < 0.5, "…and ends when its time is up")
        check(still < 0.5, "…and does not come back while the switch is still held")
        check(again > 0.5, "…until the switch is released and pressed again")

    finally:
        print("\nrestoring the tune (RAM only — nothing was burned)")
        if not args.keep:
            for off, raw in saved_cells.items():
                try:
                    L.write_raw(off, raw)
                except Exception as e:
                    print(f"  could not restore cell @{off}: {e}")
            for field, raw in saved_expr.items():
                try:
                    L.write_raw(meta.c(B + field)["offset"], raw)
                except Exception as e:
                    print(f"  could not restore {field}: {e}")
            for name, value in saved.items():
                try:
                    L.set_config(name, value)   # already raw: that is what get_config handed back
                except Exception as e:
                    print(f"  could not restore {name}: {e}")
            try:
                L.restore_script()
            except Exception as e:
                print(f"  could not restore the Lua script: {e}")
        L.close()
        sp.close()

    print(f"\n[launch] {'FAILURES: ' + ', '.join(FAILED) if FAILED else 'all passed'}")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
