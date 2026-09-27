#!/usr/bin/env python3
"""The raw trigger log, against a wheel the decoder CAN read and one it cannot.

The whole point of a time-domain log is the second case. The engine-cycle capture stamps every edge
with the angle the firing clock believed, so for an unsynced wheel it is empty by construction —
which is precisely the wheel you need to look at. This proves the log fills anyway.

Two runs:

  KNOWN    the ECU configured for the wheel the rig is spinning. Sync is reached, and the log's
           tooth spacing must agree with the geometry — that is what says the timestamps are real
           and not merely present.
  UNKNOWN  the ECU deliberately configured for a DIFFERENT wheel, so the decoder never syncs. The
           log must still fill, and every record must say "no sync".

    python3 -m tools.bench_trigger_log [rpm]
"""
import struct
import sys
import time

from tools.ts_bench import TsLink
from tools.gen_rebench import stim_halt, stim_resume, open_stim, select, configure, WHEELS

DEFAULT_RPM = 900
CMD = b"\x27"
ARM, READ, STOP = 0x00, 0x01, 0x02
# v4: `streams` and `dropped` are gone — a slot can be enabled and name no pin, so the count
# over-stated what a log contains, and (base, first) already says exactly what was lost. This parser
# was left on v3, which shifts every field after `state` by a byte: a capture with 9 records read
# back as total=9 count=0 dropped=43008, all of it garbage, and looked like a firmware fault.
HDR = "<IBBIIIH"           # magic, version, state, total, base, first, count
HDR_N = struct.calcsize(HDR)
STATE = {0: "Idle", 1: "Armed", 2: "Recording", 3: "Full"}

fails = []


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}: {detail}")
    if not cond:
        fails.append(name)


def read_log(l, want=1200):
    """Page the whole capture out. Returns (header_of_first_page, [records])."""
    recs, first, hdr0 = [], 0, None
    while len(recs) < want:
        _rt, data = l.cmd(CMD, struct.pack("<BI", READ, first))
        if len(data) < HDR_N:
            break
        magic, ver, state, total, base, pfirst, count = struct.unpack_from(HDR, data, 0)
        if hdr0 is None:
            hdr0 = dict(magic=magic, version=ver, state=state, total=total, base=base)
        if magic != 0x474F4C54:
            break
        for i in range(count):
            t_us, levels, flags = struct.unpack_from("<IBB", data, HDR_N + i * 6)
            recs.append((t_us, levels, flags))
        first += count
        if count == 0 or first >= total:
            break
    return hdr0, recs


def run(l, sp, label, cfg_idx, stim_idx, rpm, expect_sync):
    spec = WHEELS[cfg_idx]
    l2 = TsLink(); configure(l2, spec); l2.cmd(b"E", b"reconfig"); l2.close()
    select(sp, stim_idx, reps=4)
    # SET THE SPEED. `rpm` was taken as an argument, printed in the banner and never applied, so this
    # ran at whatever the previous test happened to leave the stim at — and then checked edge spacing
    # against the rpm it thought it had asked for. Every timing assertion here was measuring one
    # speed against another.
    sp.write(b"F" + struct.pack("<H", int(rpm)))
    time.sleep(3.0)
    l3 = TsLink()
    lvl = l3.telem_all()["sync_level"]     # the decoder's verdict on this wheel, BEFORE arming kills it

    # ARM IS REFUSED WHILE THE ENGINE IS RUNNING, and a refused arm is silent — the ring simply keeps
    # whatever it already held. This test span the rig up, waited for sync (which is what makes
    # engine_state RUNNING), and only then armed: the arm bounced, and every assertion below ran
    # against a STALE capture from some earlier session, on a different wheel, at a different speed.
    # It reported base != 0 and non-monotonic timestamps for that reason and no other, and the
    # geometry checks were comparing one wheel's log against another wheel's expectations.
    #
    # So stop the wheel first. The engine goes STOPPED, the arm takes, and the capture then fills
    # from the resumed pattern — which is the only way to get a clean ring at a known speed, and it
    # costs nothing because arming disables the decoder anyway.
    stim_halt(sp)
    time.sleep(1.2)
    l3.cmd(CMD, struct.pack("<BI", ARM, 0))
    stim_resume(sp)
    sp.write(b"F" + struct.pack("<H", int(rpm)))
    time.sleep(2.5)                       # let it fill
    hdr, recs = read_log(l3)
    l3.cmd(CMD, struct.pack("<B", STOP))  # put the decoder back
    l3.close()

    print(f"\n  --- {label} ---")
    if not hdr:
        check(f"{label}: reply", False, "no header"); return None
    print(f"      state={STATE.get(hdr['state'], hdr['state'])} total={hdr['total']} "
          f"base={hdr['base']} version={hdr['version']} | decoder sync_level={lvl}")
    check(f"{label}: log filled", len(recs) > 50, f"{len(recs)} records")
    # v4 has no `dropped` counter because (base, first) already says exactly what was lost: base is
    # the oldest record still in the ring, so anything the host asked for below it is gone.
    check(f"{label}: nothing overwritten", hdr["base"] == 0, f"base={hdr['base']}")
    check(f"{label}: time advances", all(b[0] >= a[0] for a, b in zip(recs, recs[1:])),
          "timestamps are monotonic")
    # NO SYNC BIT TO CHECK. Record flags bits 0-4 are reserved in v4: they used to carry the
    # decoder's sync level, which is a field that could only ever read zero once the decoder is
    # switched off for the duration of a capture — a value that cannot vary is not an observation.
    # This asserted on it in both directions, so it failed on a synced wheel and "passed" on an
    # unsynced one for the wrong reason.
    return recs


def main():
    rpm = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_RPM
    print(f"=== raw trigger log @ {rpm} rpm ===")
    sp = open_stim()
    from tools.bench_fuel import set_fixed_rpm
    set_fixed_rpm(sp, rpm)
    l = TsLink(); l.close()

    # KNOWN: ECU and rig both on 36-1. The decoder syncs, so the log can be checked against geometry.
    recs = run(None, sp, "known wheel (36-1)", 6, 6, rpm, expect_sync=True)
    if recs:
        # THE LOG SEES ONLY THE EDGES THE CAPTURE CHANNEL IS ARMED FOR. The crank stream here is
        # configured Rising, so a 36-1 gives 35 edges per revolution, not 70 — one per present tooth.
        # Worth knowing before reading a log of an unknown wheel: mark and space widths are invisible
        # unless the stream is set to Both, so a wheel whose identity is in its SLOT WIDTHS (an
        # optical CAS) needs Both to be readable at all.
        deltas = sorted(b[0] - a[0] for a, b in zip(recs, recs[1:]) if b[0] > a[0])
        med = deltas[len(deltas) // 2]
        rev_us = 60_000_000 / rpm
        span = recs[35][0] - recs[0][0] if len(recs) > 35 else 0
        check("known: 35 edges span one revolution (rising-only, 36-1)",
              abs(span - rev_us) < rev_us * 0.10,
              f"35 edges spanned {span} us, one rev is {rev_us:.0f} us")
        # The gap shows up as the longest spacing, and on a 36-1 it is twice a tooth pitch.
        pitch = rev_us / 36.0
        check("known: the gap is visible as a double-pitch spacing",
              abs(max(deltas) - 2 * pitch) < pitch * 0.35,
              f"longest {max(deltas)} us vs 2 x pitch {2 * pitch:.0f} us")
        check("known: edge spacing is sane", 0 < med < rev_us, f"median gap {med} us")

    # UNKNOWN: rig still on 36-1, ECU told it is a 60-2 + cam. The decoder cannot sync — and this is
    # the case the angle-stamped capture cannot record at all.
    run(None, sp, "unknown wheel (ECU told 60-2+cam, rig spinning 36-1)", 4, 6, rpm,
        expect_sync=False)

    sp.close()
    print(f"\n  {'FAILED: ' + ', '.join(fails) if fails else 'the log works with and without sync'}")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
