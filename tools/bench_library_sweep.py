#!/usr/bin/env python3
"""Every library wheel the rig can spin, decoded and MEASURED — not just decoded.

The library entries were derived from the rig's own pattern tables, so this closes the loop: spin
the wheel the tables describe, hand the ECU the entry derived from them, and check the engine-cycle
capture records the teeth the geometry predicts. A wheel that merely "syncs" proves very little —
a decoder can lock onto the wrong feature and count happily — so the assertion is on the COUNT of
recorded crank edges per cycle, which is a number the geometry states in advance.

  * sync reached, at the level the wheel claims (CRANK, or PHASE when it has a cam)
  * crank edges per cycle == present teeth x revolutions in the cycle
  * cam edges present when the wheel says PHASE

    python3 -m tools.bench_library_sweep [rpm] [name-substring]
"""
import json
import sys
import time
from pathlib import Path

from tools.ts_bench import TsLink
from tools.gen_rebench import open_stim, select, configure, STREAM0, STREAM_STRIDE
from tools.bench_fuel import set_fixed_rpm, wait_for_sync
from tools.extract_stim_wheels import load_arrays, load_table, edges, describe_gap_wheel

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_RPM = 900


def meta_wheels():
    raw = (ROOT / "shared/tuneit-meta.json").read_bytes()[:-4]
    return {w["name"]: w for w in json.loads(raw.decode())["trigger_wheels"]}


def to_spec(w):
    """A meta wheel -> the (name, streams, cam_edge, sync) tuple gen_rebench.configure() wants."""
    streams = []
    for s in w["streams"]:
        # configure() reads slots/ratio/cell off every stream whatever its primitive, so they are
        # always present — a SEQUENCE ignores slots, a WIDTH ignores the cell.
        base = {"rate": s["rate"], "slots": 0, "ratio": 0, "cell": [],
                "repeats": s.get("repeats", 0)}   # 0 = take the slot default
        if s["kind"] == "seq":
            streams.append({**base, "prim": 1, "cell": s["cell"]})
        elif s["kind"] == "width":
            streams.append({**base, "prim": 2, "wmin": s.get("width_min", 0),
                            "wmax": s.get("width_max", 7200), "wtgt": s.get("width_target", 0)})
        else:
            streams.append({**base, "prim": 0, "slots": s["slots"],
                            "ratio": s.get("ratio", 0), "cell": s.get("gaps", [])})
    # BENCH WIRING, not wheel geometry. configure() sends crank-rate streams to the rig's crank
    # output and cam-rate ones to DIG3. A distributor has ONE pickup and the rig emits it on the
    # crank output, so a wheel with no crank-rate stream must name that pin explicitly or it is
    # listening to a wire nothing is driving — which reads as "never synced" and looks like a decode
    # failure. The library says nothing about pins; this is the harness's business.
    if streams and not any(s["rate"] == 0 for s in streams):
        streams[0]["cap"] = 2          # gen_rebench.CRANK_CAP
    return (w["name"], streams, w.get("cam_edge", -1), w["sync"])


def expected_crank_edges(sym, arrays, table_row):
    """Present crank teeth per 360, from the rig's own table."""
    s = arrays[table_row["sym"]]
    deg_per = table_row["deg"] / len(s)
    rise, _ = edges(s, 0, deg_per)
    period = 360.0 if table_row["deg"] == 720 else float(table_row["deg"])
    return len([a for a in rise if a < period])


def main():
    rpm = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else DEFAULT_RPM
    want = (sys.argv[2] if len(sys.argv) > 2 else "").lower()

    wheels, arrays, table = meta_wheels(), load_arrays(), load_table()
    # Match a library entry to the rig index that produces it, by the name the rig uses.
    from tools.gen_library_wheels import SELECTED
    todo = [(idx, name) for idx, name in SELECTED.items()
            if name in wheels and (not want or want in name.lower())]

    print(f"=== library sweep @ {rpm} rpm — {len(todo)} wheels ===")
    sp = open_stim()
    fails = []
    # THE RIG GOES BACK THE WAY IT WAS FOUND. This script rewrites the whole trigger stream block for
    # every wheel it sweeps and used to walk away from the last one — so the ECU was left decoding a
    # Renix 6-cylinder wheel that nobody owns, at 900 rpm, and the next person to look at it saw an
    # engine running with no wire connected. Nothing here is burned, so the stored image IS the way
    # back: read it before the first write, put it back after the last.
    _l0 = TsLink()
    saved_streams = _l0.read_config_chunked(STREAM0, STREAM_STRIDE * 6)   # 1296 B: one frame cannot hold it
    _l0.close()
    for idx, name in todo:
        w = wheels[name]
        want_edges = expected_crank_edges(name, arrays, table[idx])
        try:
            # Force the decoder to re-init on the new wheel: one still limping on the previous
            # pattern never drops below the auto-reconfigure gate, so it would never apply.
            l = TsLink(); configure(l, to_spec(w)); l.cmd(b"E", b"reconfig"); l.close()
            select(sp, idx, reps=4)
            set_fixed_rpm(sp, rpm)
            l = TsLink()
            e = wait_for_sync(l, 20.0)
            if not e:
                print(f"  [FAIL] {name:32s} never synced"); fails.append(name); l.close(); continue
            # A PHASE wheel needs a moment past crank sync for the cam to resolve the revolution.
            for _ in range(30):
                if l.telem_all()["sync_level"] >= (2 if w["sync"] == "PHASE" else 1): break
                time.sleep(0.4)
            lvl = l.telem_all()["sync_level"]
            l.execute("key on"); time.sleep(1.0)
            cap = l.cycle_capture(timeout=6.0)
            crank = sum(1 for x in cap["edges"] if x["signal"] == "Crank")
            cam   = sum(1 for x in cap["edges"] if x["signal"] == "Cam")
            # cycle_angle is the SCHEDULER's span (720 on a four-stroke however the decoder is
            # synced), but at CRANK sync the decoder only knows 360 and the capture covers one
            # revolution. Expecting two revolutions' teeth there made six correctly-decoded wheels
            # look broken, each off by exactly a factor of two.
            revs  = 2 if lvl >= 2 else 1
            want_total = want_edges * revs
            ok = abs(crank - want_total) <= 1 and lvl >= (2 if w["sync"] == "PHASE" else 1)
            if w["sync"] == "PHASE" and cam == 0:
                ok = False
            print(f"  [{'PASS' if ok else 'FAIL'}] {name:32s} sync={lvl} span={cap['cycle_angle']:.0f} "
                  f"crank={crank} (want {want_total}) cam={cam}")
            if not ok:
                fails.append(name)
            l.execute("key auto"); l.close()
        except Exception as ex:
            print(f"  [FAIL] {name:32s} {type(ex).__name__}: {ex}"); fails.append(name)
            try: l.close()
            except Exception: pass
    # …and stop the wheel, which is the other half of leaving the bench as it was. A stim left
    # spinning is a rig that looks alive to everyone who walks up to it afterwards.
    try:
        set_fixed_rpm(sp, 0)
    except Exception as ex:
        print(f"  ! could not stop the stim: {ex}")
    sp.close()
    try:
        l = TsLink()
        for off in range(0, len(saved_streams), 200):
            l.write_raw(STREAM0 + off, saved_streams[off:off + 200])
        l.cmd(b"E", b"reconfig")
        l.close()
        print("  trigger config restored (RAM — the burned tune was never touched)")
    except Exception as ex:
        print(f"  ! could not restore the trigger config: {ex} — reset the ECU to get it back")
    print(f"\n  {'FAILED: ' + ', '.join(fails) if fails else 'every wheel decoded and counted right'}")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
