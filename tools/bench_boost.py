#!/usr/bin/env python3
"""BOOST CONTROL on the real ECU: the handover, the re-axed feed-forward, a correction slot, and the
learned trim.

WHY THESE FOUR. They are the ones a host test cannot settle.

  the handover        host tests prove the arithmetic; only the ECU proves the module reaches that
                      code at all under a real config image written through the config protocol.
  the feed-forward    its X axis reads boost_target — a channel THIS MODULE publishes earlier in the
                      same frame. On the host the two happen in one function call; on the ECU they are
                      a bus write and a bus read with a scheduler in between, and if the read saw the
                      previous frame's value the whole re-axis would be quietly off by one frame.
  a correction slot   proves table_eval works through a RE-POINTED axis selector, which is the part
                      of the slot design that only exists because codegen synthesises the field.
  the learned trim    proves LEARNED_BOOST_LTT_OFFSET lands on its own slice of the region. A
                      collision is silent: another module's trim would simply start moving.

HOW IT DRIVES. There is no turbo on the bench, so MAP is injected by a Lua script at PRIO_LUA (which
out-votes the sensor pipeline) from a config scratch cell the host writes.

RPM IS SPUN, NOT INJECTED, and the distinction cost this script a first run. The module's activation
gate reads pos.rpm — the DECODER's rpm, off the trigger — while its table axes read the `rpm` BUS
channel. Injecting the bus channel alone moved every axis correctly and left activation reading zero,
so the target was computed, the error was published, and the wastegate duty stayed resolutely absent.
The stim spins a 36-1 wheel, which answers both.

THE TUNE IS RESTORED ON EXIT, and nothing is burned: every write here lives in ECU RAM, so a reset
undoes the lot even if this script dies halfway.

    python3 tools/bench_boost.py
    python3 tools/bench_boost.py --keep     # leave the test tune in RAM to poke at by hand
"""
import argparse, struct, sys, time

sys.path.insert(0, ".")
from tools.ts_bench import TsLink, Meta
from tools.gen_rebench import open_stim, select, configure, WHEELS

WHEEL_IDX = 6          # '36-1' — dense, locks in ~7 s, single crank stream (no cam needed)

B = "boost_"          # the meta flattens a module scalar as <module>_<field>

# THE SCRATCH CELLS the Lua script reads. Both are inert under this test's tune: scramble_kpa is only
# read while a scramble input is assigned (left at -1 here) and trim_max_kpa only while a trim input is
# (also -1). Borrowing a cell the module still consults would have the test fighting itself — the same
# rule bench_cruise.py follows for its two.
CELL_MAP = "scramble_kpa"     # read back as kPa

LUA = """
function onTick()
  signalWrite("map", getCalibration("boost_scramble_kpa"), 300)
end
"""

# The learned region's address and this block's slice of it, read out of the meta rather than typed.
def _learned():
    import json
    raw = open("shared/tuneit-meta.json", "rb").read()
    lr = json.loads(raw[:-4])["learned"]
    blk = next(b for b in lr["blocks"] if b["id"] == "boost_ltt")
    return lr["page_base"], blk["offset"], blk["rows"] * blk["cols"] * 2   # int16 cells


LEARNED_PAGE_BASE, LTT_OFFSET, LTT_BYTES = _learned()

FAILED = []


def ts(l, meta, name):
    """Telemetry in ENGINEERING units. read_telemetry hands back the raw frame, so the channel's own
    scale has to be applied — every reading in this script went out by a factor of ten before it did."""
    return l.telem(name) * (meta.t(name).get("scale", 1.0) or 1.0)


def clear_trim(l, meta):
    """Zero the boost learned block, through the config protocol.

    There is no `learn reset`: the CLI's learn command reads, pokes a marker, flushes a totem and
    reports a sequence number, and nothing else (CliCommands.cpp:377). `learned reset` was therefore a
    silently-rejected line, and the section that claimed to start from a neutral cell was starting from
    whatever the LAST RUN taught the ECU — battery-backed state does not care that the script restarted.

    The region is reachable instead: comms offsets at or above learned.page_base route to it, so the
    block's own slice can simply be written with zeros.
    """
    base = int(LEARNED_PAGE_BASE, 16)
    l.write_raw(base + LTT_OFFSET, b"\x00" * LTT_BYTES)


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
    # pipeline — so it happens on its own link before the test tune goes in, not in the middle of it.
    print("spinning the stim...")
    sp = open_stim()
    spec = WHEELS[WHEEL_IDX]
    l0 = TsLink(args.port); configure(l0, spec); l0.cmd(b"E", b"reconfig"); l0.close()
    select(sp, WHEEL_IDX, reps=4)

    def spin(rpm):
        sp.reset_input_buffer()
        sp.write(b"F" + struct.pack("<H", int(rpm)))   # ardustim 'F': hold a fixed rpm
        time.sleep(0.3)

    spin(4000)
    L = TsLink(args.port)
    print(L.hello())
    if not wait_for_sync(L):
        print("  the decoder never reached sync — is the stim wired to the crank input?")
        L.close(); sp.close()
        return 1
    print(f"  synced at {L.telem('rpm'):.0f} rpm")
    saved = {}

    # ENGINEERING IN, RAW OUT. TsLink.get_config/set_config are deliberately raw — they are the
    # protocol, not a tuner — so every value here is converted through the field's own scale rather
    # than by a constant, because the scales differ per field (0.001 on a gain, 0.1 on a pressure).
    def raw_of(c, eng):
        if c["datatype"] == "F32":
            return eng
        sc = c.get("scale", 1.0) or 1.0
        return int(round(eng / sc))

    def setc(name, value):
        """Set a boost field in ENGINEERING units, remembering its raw value for the restore."""
        full = B + name
        if full not in saved:
            saved[full] = L.get_config(full)
        L.set_config(full, raw_of(meta.c(full), value))

    def setcell(table, index, eng):
        """One cell of a table or axis. The named-scalar path cannot address these — a table is one
        meta entry with one offset — so this is offset + index * cell size, written raw."""
        c = meta.c(B + table)
        L.write_raw(c["offset"] + index * meta.size(c["datatype"]),
                    struct.pack(meta.fmt(c["datatype"]), raw_of(c, eng)))

    def drive(map_kpa, rpm, settle=0.6):
        """Put the engine somewhere and let the loop see it for a few of its 50 Hz frames. RPM is
        SPUN — the activation gate reads the decoder, not the bus — and MAP is injected."""
        setc(CELL_MAP, map_kpa)
        spin(rpm)
        time.sleep(settle)

    try:
        L.execute("key on")
        L.set_script(LUA)

        # A tune this script can reason about: flat 150 kPa target, flat 40 % feed-forward, closed loop,
        # every optional thing off. Anything the module might read is written, not assumed.
        for k, v in [("enabled", 1), ("mode", 1), ("arm_sig", -1), ("trim_sig", -1),
                     ("scramble_sig", -1), ("activation_rpm", 2000), ("activation_kpa", 110),
                     ("max_duty_pct", 90), ("min_duty_pct", 0), ("kp", 0.5), ("ki", 2.0), ("kd", 0),
                     ("ki_sched_en", 0), ("iterm_max_pct", 100), ("min_tps_pct", 0),
                     ("start_delay_en", 0), ("spool_assist", 0), ("control_point_kpa", 20),
                     ("overboost_limit_kpa", 400), ("overboost_offset_kpa", 0),
                     ("output_mode", 0), ("ltt_en", 0),
                     ("corr1_en", 0), ("corr2_en", 0), ("corr3_en", 0), ("corr4_en", 0)]:
            setc(k, v)
        for i in range(8):
            setcell("boost_target_table", i, 150.0)
        for i in range(16):
            setcell("base_boost_duty_table", i, 40.0)
            setcell("base_targ_axis", i, 100.0 + 10.0 * i)

        # CLEAR THE TRIM BEFORE ANYTHING, not just before the section that learns. It is
        # battery-backed: what the LAST run of this script taught the ECU is still there, and 15 % of
        # learned duty riding on the base table is enough to make the handover section read 55 % where
        # it expects 40 and call a working module broken.
        clear_trim(L, meta)
        time.sleep(0.3)

        print("\n--- the target, and the error published from it ---")
        drive(130.0, 4000)
        tgt, err = ts(L, meta, "boost_target"), ts(L, meta, "boost_error")
        check(abs(tgt - 150.0) < 2.0, "the target table is read", f"{tgt:.1f} kPa")
        check(abs(err - 20.0) < 2.0, "boost_error is target - map", f"{err:.1f} kPa")

        print("\n--- the handover ---")
        # 120 kPa is past activation (110) and 10 kPa short of the control point (150 - 20 = 130).
        drive(120.0, 4000, settle=1.2)
        resting = ts(L, meta, "wastegate_duty")
        check(abs(resting - 40.0) < 1.0, "below the control point it rests on the base duty",
              f"{resting:.1f} %")
        drive(140.0, 4000, settle=1.2)
        working = ts(L, meta, "wastegate_duty")
        check(working > 42.0, "above it the loop takes over and trims", f"{working:.1f} %")

        setc("spool_assist", 1)
        drive(120.0, 4000, settle=1.2)
        assisted = ts(L, meta, "wastegate_duty")
        check(abs(assisted - 90.0) < 1.0, "spool assist holds it shut to the ceiling instead",
              f"{assisted:.1f} %")
        setc("spool_assist", 0)

        print("\n--- the feed-forward is read against the target, in the SAME frame ---")
        # The axis channel is boost_target, which the module published moments earlier in this very
        # frame. Shape the table so the answer can only be right if the read saw THIS frame's value:
        # a one-frame lag would still land on a neighbouring cell, so the two targets are far apart.
        setc("mode", 0)                                    # open loop: duty IS the feed-forward
        for i in range(16):
            setcell("base_boost_duty_table", i, 5.0)
        setcell("base_boost_duty_table", 2, 20.0)   # 120 kPa of target -> 20 %
        setcell("base_boost_duty_table", 10, 70.0)  # 200 kPa of target -> 70 %
        for i in range(8):
            setcell("boost_target_table", i, 120.0)
        drive(115.0, 4000, settle=0.8)
        low = ts(L, meta, "wastegate_duty")
        for i in range(8):
            setcell("boost_target_table", i, 200.0)
        drive(115.0, 4000, settle=0.8)
        high = ts(L, meta, "wastegate_duty")
        check(abs(low - 20.0) < 1.5, "a 120 kPa target reads the 120 kPa cell", f"{low:.1f} %")
        check(abs(high - 70.0) < 1.5, "a 200 kPa target reads the 200 kPa cell", f"{high:.1f} %")

        print("\n--- a correction slot, through a re-pointed axis ---")
        # Slot 1 ships pointed at iat. Re-point it at rpm and give it a shape rpm can reach, which is
        # the part of the design that only works because codegen synthesised corr1_table_x_src.
        setc("corr1_en", 1)
        setc("corr1_applies", 0)                            # trims the target
        setc("corr1_table_x_src", meta.signals["rpm"])
        for i in range(8):
            setcell("corr1_axis", i, 1000.0 * (i + 1))
            setcell("corr1_table", i, 0.0)
        setcell("corr1_table", 2, -20.0)           # -20 % at 3000 rpm
        setcell("corr1_table", 5, 0.0)             #   0 % at 6000 rpm
        drive(115.0, 3000, settle=0.8)
        trimmed = ts(L, meta, "boost_target")
        drive(115.0, 6000, settle=0.8)
        untrimmed = ts(L, meta, "boost_target")
        check(abs(trimmed - 160.0) < 4.0, "the slot multiplies the target at its own axis value",
              f"{trimmed:.1f} kPa (200 - 20 %)")
        check(abs(untrimmed - 200.0) < 4.0, "...and leaves it alone where the curve is zero",
              f"{untrimmed:.1f} kPa")
        setc("corr1_en", 0)

        print("\n--- the learned trim has its own slice of the region ---")
        setc("mode", 1)
        for i in range(16):
            setcell("base_boost_duty_table", i, 40.0)
        for i in range(8):
            setcell("boost_target_table", i, 150.0)
        setc("ltt_en", 1)
        setc("ltt_min_tps_pct", 0)
        setc("ltt_min_rpm", 0)
        setc("ltt_max_rpm", 12000)
        setc("ltt_min_gear", 0)
        setc("ltt_dwell_ms", 200)
        setc("ltt_learn_pct", 100)
        setc("ltt_authority_pct", 15)
        clear_trim(L, meta)
        time.sleep(0.3)
        before = ts(L, meta, "boost_ltt_pct")
        drive(140.0, 4000, settle=4.0)                      # hold one cell, well past the dwell
        after = ts(L, meta, "boost_ltt_pct")
        check(abs(before) < 0.2, "reset leaves the cell neutral", f"{before:.2f} %")
        check(after > 0.2, "holding a cell under target learns into it", f"{after:.2f} %")
        check(abs(after) <= 15.0 + 0.2, "and never past its authority", f"{after:.2f} %")

        print("\n--- the motorised gate publishes a position, not a duty ---")
        setc("mode", 0)
        setc("ltt_en", 0)
        # AND CLEAR WHAT THE LAST SECTION TAUGHT IT. A learned trim stays APPLIED when learning is
        # switched off — that is the design, and it is why the switch reads Enabled rather than Used —
        # so the 15 % just learned would otherwise ride on this section's base duty and put the valve
        # 15 % from where the arithmetic here says. (It did, on the first run: 45 % instead of 60.)
        clear_trim(L, meta)
        time.sleep(0.3)
        setc("output_mode", 1)
        setc("gate_pos_sig", meta.signals["wastegate_valve_pos_1"])
        setc("gate_min_pos_pct", 0)
        setc("gate_max_pos_pct", 100)
        drive(115.0, 4000, settle=0.8)
        pos_t = ts(L, meta, "wastegate_pos_target")
        check(abs(pos_t - 60.0) < 2.0, "40 % of duty is a valve 60 % open", f"{pos_t:.1f} %")

    finally:
        if not args.keep:
            print("\nrestoring the tune (RAM only — nothing was burned)")
            for name, value in saved.items():
                try:
                    L.set_config(name, value)   # already raw: that is what get_config handed back
                except Exception as e:      # keep going: a half-restored tune is worse than a noisy one
                    print(f"  could not restore {name}: {e}")
            try:
                L.restore_script()
            except Exception as e:
                print(f"  could not restore the Lua script: {e}")
        L.close()
        sp.close()

    print(f"\n[boost] {'FAILURES: ' + ', '.join(FAILED) if FAILED else 'all passed'}")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
