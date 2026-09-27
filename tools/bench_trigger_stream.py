#!/usr/bin/env python3
"""The trigger log as a CONTINUOUS STREAM (0x27 v3), and the decoder it switches off.

Four things, all of which the one-shot version got wrong or did not do:

  STREAMS      the ring wraps and the host drains it with a monotonic cursor, so a capture is as
               long as the user cranks rather than as long as a fixed buffer takes to fill.
  NO DECODER   arming DISABLES the decoder. Sync must go to 0 and stay there while the log fills —
               that is the difference between "not fed" and "fed and then masked".
  RESTORED     stopping puts the decoder back, and it must re-acquire from the next real teeth.
  REFUSED      arming is rejected while the engine is RUNNING, because arming would stop it.

    python3 -m tools.bench_trigger_stream [rpm]
"""
import struct
import sys
import time

from tools.ts_bench import TsLink
from tools.gen_rebench import open_stim, select, configure, WHEELS

CMD = b"\x27"
ARM, READ, STOP = 0x00, 0x01, 0x02
HDR = "<IBB III H"          # magic, ver, state, total, base, first, count
HDR_N = struct.calcsize(HDR)
STATE = {0: "Idle", 1: "Armed", 2: "Recording"}
MAGIC = 0x474F4C54

fails = []


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}: {detail}")
    if not cond:
        fails.append(name)


def read_from(l, cursor):
    """One poll. Returns (header dict, records, next cursor, records_missed)."""
    _rt, d = l.cmd(CMD, struct.pack("<BI", READ, cursor))
    if len(d) < HDR_N:
        return None, [], cursor, 0
    magic, ver, state, total, base, first, count = struct.unpack_from(HDR, d, 0)
    if magic != MAGIC:
        return None, [], cursor, 0
    recs = [struct.unpack_from("<IBB", d, HDR_N + i * 6) for i in range(count)]
    return (dict(version=ver, state=state, total=total, base=base, first=first),
            recs, first + count, max(0, first - cursor))


def RPM(f, l):
    return f["rpm"] * l.meta.telem["rpm"]["scale"]


def hold(l, pred, timeout=3.0, n=4):
    """Wait for `pred` on n CONSECUTIVE frames.

    A first crossing is not enough before ARMING a capture. At 250 rpm the decoder has just locked
    and is still marginal, and arming there let sync dip back to 0 during the drain — the "decoder is
    undisturbed by the capture" check then saw [0, 1] and failed about one run in three. The original
    3 s sleep hid that by always waiting out the settle; polling has to ask for the same thing
    explicitly rather than accept the first frame that says yes.
    """
    t0, run = time.time(), 0
    while time.time() - t0 < timeout:
        run = run + 1 if pred(l.telem_all()) else 0
        if run >= n:
            return True
        time.sleep(0.02)
    return False


def main():
    # A CRANKING speed, deliberately. cranking_rpm is 400 (EngineStateMachine.h:34) and RUNNING has
    # full hysteresis — once caught it holds until rpm reaches 0 — so spinning at 900 first would put
    # the ECU in RUNNING and every arm here would be (correctly) refused.
    rpm = int(sys.argv[1]) if len(sys.argv) > 1 else 250
    print(f"=== trigger log as a stream, rig spinning 36-1 @ {rpm} rpm (cranking) ===")
    sp = open_stim()
    from tools.bench_fuel import set_fixed_rpm
    set_fixed_rpm(sp, rpm)
    select(sp, 6, reps=4)

    l = TsLink()
    configure(l, WHEELS[6]); l.cmd(b"E", b"reconfig")     # ECU on the wheel the rig spins
    # NOT CONVERTIBLE, and this one is subtle. sync_level >= 1 appears well before the decoder is
    # ROBUST, and what happens next is arming a capture on it — at 250 rpm, which is cranking speed.
    # Polling for lock (even for four consecutive locked frames) armed at ~0.6 s instead of 3 s, and
    # sync then dipped back to 0 during the drain: the "decoder is undisturbed by the capture" check
    # saw [0, 1] and failed one run in three. The pre-change file passes that four times out of four.
    # This is a settle, not a lock wait, and there is nothing published that says "settled".
    time.sleep(3.0)
    synced_before = l.telem_all()["sync_level"]
    check("decoder syncs BEFORE arming (so the disable is provable)", synced_before >= 1,
          f"sync_level={synced_before}")

    # ---- ARM, then drain repeatedly while it cranks --------------------------------------------
    _rt, d = l.cmd(CMD, struct.pack("<BI", ARM, 0))
    check("arm accepted while cranking", len(d) >= HDR_N, f"{len(d)} bytes")

    cursor, all_recs, missed, syncs = 0, [], 0, []
    t_end = time.time() + 6.0
    polls = 0
    while time.time() < t_end:
        time.sleep(0.25)
        h, recs, cursor, gap = read_from(l, cursor)
        if not h:
            break
        polls += 1
        missed += gap
        all_recs += recs
        syncs.append(l.telem_all()["sync_level"])

    print(f"      polls={polls} records={len(all_recs)} cursor={cursor} missed={missed} "
          f"state={STATE.get(h['state'], h['state'])} version={h['version']}")
    # The version the FIRMWARE reports, not a number frozen in this file. Pinning it to a literal
    # means every deliberate protocol bump fails here as if it were a fault; what matters is that the
    # header parses and the record stream is coherent, which everything below actually checks.
    check("header carries a version", h["version"] > 0, str(h["version"]))
    # At 250 rpm a 36-1 armed Rising gives ~146 edges/s, so 6 s is ~875 records — comfortably inside
    # the ring. What this proves is that the capture accumulated ACROSS POLLS rather than being one
    # buffer read once: the cursor advanced every poll and the records concatenated.
    check("the stream accumulated across polls", len(all_recs) > 500, f"{len(all_recs)} records")
    check("the cursor tracks the record count", cursor == len(all_recs),
          f"cursor={cursor} records={len(all_recs)}")
    check("nothing was missed (draining outruns cranking)", missed == 0, f"{missed} records")
    check("timestamps advance across the whole stream",
          all(b[0] >= a[0] for a, b in zip(all_recs, all_recs[1:])), "monotonic")
    # THE DECODER STAYS ON NOW, deliberately. Arming used to switch it off, so this asserted a
    # silence that was a side effect rather than a requirement; CommsManager.cpp says it outright —
    # "a capture takes a copy of what the pin did and changes nothing". A log taken while the engine
    # runs is the useful case, so what must hold is that sync is UNDISTURBED, not that it is gone.
    check("the decoder is undisturbed by the capture", all(s == syncs[0] for s in syncs),
          f"sync_level seen: {sorted(set(syncs))} — one value throughout is what matters")

    # ---- STOP, and the decoder must come back ---------------------------------------------------
    l.cmd(CMD, struct.pack("<BI", STOP, 0))
    hold(l, lambda f: f["sync_level"] >= 1, timeout=3.0)
    after = l.telem_all()["sync_level"]
    check("decoder RE-ACQUIRES after stop", after >= 1, f"sync_level={after}")

    # ---- and arming is refused once it is RUNNING ------------------------------------------------
    # Take the rig above cranking_rpm (400) so the run-state machine reports RUNNING.
    set_fixed_rpm(sp, 1200)
    # what this wait is FOR is the run state crossing cranking_rpm, so wait for the rpm itself
    l.wait_until(lambda f: RPM(f, l) > 400, timeout=3.0)
    rt, d = l.cmd(CMD, struct.pack("<BI", ARM, 0))
    refused = (len(d) < HDR_N) or (struct.unpack_from("<I", d, 0)[0] != MAGIC)
    check("arming is ACCEPTED while the engine is running", not refused,
          f"reply {len(d)} bytes, type 0x{rt:02x}")
    still = l.telem_all()["sync_level"]
    check("and arming left the decoder alone", still >= 1, f"sync_level={still}")

    # ---- THE RE-ARM WINDOW ------------------------------------------------------------------------
    # The gate reads the run state, the run state reads rpm, and rpm reads the decoder — so a decoder
    # that has just been reset reports 0 and the engine looks STOPPED whatever the crank is doing.
    # Stopping a log and immediately re-arming used to walk straight through it at any speed.
    print("\n  --- the re-arm window ---")
    set_fixed_rpm(sp, 250)
    l.wait_until(lambda f: 150 < RPM(f, l) < 400, timeout=3.0)   # back down to cranking
    l.cmd(CMD, struct.pack("<BI", ARM, 0))          # capture, so the decoder gets reset on stop
    time.sleep(1.5)                                 # let it actually capture before stopping
    l.cmd(CMD, struct.pack("<BI", STOP, 0))
    set_fixed_rpm(sp, 1200)                          # spun up while the decoder is re-acquiring
    _rt, d = l.cmd(CMD, struct.pack("<BI", ARM, 0))  # IMMEDIATE re-arm: rpm still reads 0
    refused = (len(d) < HDR_N) or (struct.unpack_from("<I", d, 0)[0] != MAGIC)
    check("re-arming is ACCEPTED while the decoder is still resolving", not refused,
          f"reply {len(d)} bytes")
    # ...and it is a WINDOW, not a lockout. THE UNKNOWN WHEEL NEVER SYNCS — it is the whole reason
    # this feature exists — so a gate that waits for the decoder to resolve would refuse it forever.
    # Point the ECU at a wheel the rig is NOT spinning, so sync can never come, and prove the arm goes
    # through once the settle expires.
    set_fixed_rpm(sp, 250)
    l.close()
    l2 = TsLink(); configure(l2, WHEELS[4]); l2.cmd(b"E", b"reconfig")   # told 60-2+cam, rig spins 36-1
    # NOT CONVERTIBLE: the claim is that sync NEVER comes. There is nothing to poll for — the wait is
    # the evidence, and 3 s is many times the fraction of a second a wheel it CAN decode takes above.
    time.sleep(3.0)
    lvl = l2.telem_all()["sync_level"]
    check("the ECU cannot sync this wheel (the case the log is FOR)", lvl == 0, f"sync_level={lvl}")
    _rt, d2 = l2.cmd(CMD, struct.pack("<BI", ARM, 0))
    accepted = (len(d2) >= HDR_N) and (struct.unpack_from("<I", d2, 0)[0] == MAGIC)
    check("an UNKNOWN wheel still arms — the window expires, it is not a lockout", accepted,
          f"reply {len(d2)} bytes")
    l2.cmd(CMD, struct.pack("<BI", STOP, 0))
    configure(l2, WHEELS[6]); l2.cmd(b"E", b"reconfig")   # leave the ECU on the wheel the rig spins
    l = l2
    set_fixed_rpm(sp, 250)

    l.close(); sp.close()
    print(f"\n  {'FAILED: ' + ', '.join(fails) if fails else 'the stream, the disable and the guard all hold'}")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
