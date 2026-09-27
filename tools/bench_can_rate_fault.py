#!/usr/bin/env python3
"""Live bench validation of the bit-rate-mismatch diagnostic.

A CAN frame only completes when another node acknowledges it. Point the ECU at a bit rate the peer
is not using and nothing completes: AutoRetransmission holds the frame in its mailbox and retries
for ever, all three mailboxes fill, send_frame starts refusing, and the LOAD reads zero because
nothing ever finished to be counted. Every one of those numbers was published long before anything
said what had happened.

This proves the ECU now says it, AND that it says the right thing:

  control    both ends at 500k  -> frames complete, no code, state Active, last error None
  treatment  ECU moved to 1M    -> nothing completes, P1657 raised, and the controller reports a
                                   STUFF error (the bit rate disagrees), not an ACK error (nobody
                                   there) — the distinction the fault text sends you to read
  recovery   peer moved to 1M   -> completes again and the code heals

The control matters: without it a green "no code at 500k" proves nothing, because a check that never
fires also never fires when it should.

  python3 tools/bench_can_rate_fault.py
"""
import struct
import sys
import time
import pathlib

import serial

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from ts_bench import TsLink

ADAPTER = '/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0'
BUS = 0
P_CAN1_NO_COMPLETE = 0x1657

RATE_IDX = {125_000: 0, 250_000: 1, 500_000: 2, 1_000_000: 3}
SLCAN    = {125_000: b'S4', 250_000: b'S5', 500_000: b'S6', 1_000_000: b'S8'}
STATE    = {0: 'Active', 1: 'Warning', 2: 'Error Passive', 3: 'Bus Off'}
LASTERR  = {0: 'None', 1: 'Stuff (bit rate disagrees)', 2: 'Form (bit rate disagrees)',
            3: 'No acknowledgement (nobody there)', 4: 'Bit recessive', 5: 'Bit dominant', 6: 'CRC'}
fails = 0


def check(ok, what, detail=""):
    global fails
    print(f"  [{'PASS' if ok else 'FAIL'}] {what}{('  - ' + detail) if detail else ''}")
    if not ok:
        fails += 1


def adapter_at(rate):
    """Point the slcan peer at a bit rate. It is the only other node on this wire, so what it is
    set to IS what the ECU has to agree with."""
    s = serial.Serial(ADAPTER, 2000000, timeout=0.3)
    time.sleep(0.3)
    for c in (b'C\r', SLCAN[rate] + b'\r', b'O\r'):
        s.write(c)
        time.sleep(0.3)
        s.read(4096)
    return s                                   # kept OPEN: a closed controller does not ACK either


def bus_set(link, field, value, fmt='<B'):
    link.write_raw(link.meta.array_offset('can', 'bus', BUS, field), struct.pack(fmt, value))
    time.sleep(1.2)                            # can_task re-applies on the config generation


def active_codes(link):
    """Every ACTIVE P-code, read from the DTC table itself.

    dtc_worst_code is the highest-SEVERITY code, not the newest — on a bench with six unrelated
    faults already raised it names one of those and never mentions the one under test. Asking the
    table which codes are active is the only assertion that can actually fail for the right reason.
    Paged: the ECU answers with as many 36-byte records as fit one frame.
    """
    out, first = set(), 0
    while True:
        _flag, img = link.cmd(b'G', struct.pack('<H', first))
        if len(img) < 10:
            break
        n = struct.unpack_from('<H', img, 8)[0]
        if n == 0:
            break
        for k in range(n):
            code, _src, _sev, status = struct.unpack_from('<HBBB', img, 10 + k * 36)
            if status & 0x01:                       # DTC_ACTIVE
                out.add(code)
        first += n
        if first > 64:
            break
    return out


def state_of(link):
    t = link.telem_all()
    return (t['can1_load_pct'] * link.meta.t('can1_load_pct')['scale'],
            int(t['can1_tx_fail']), int(t['can1_state']), int(t['can1_last_err']),
            int(t['dtc_active']), active_codes(link))


def settle(link, secs=3.0):
    time.sleep(secs)
    return state_of(link)


def main():
    link = TsLink()
    print("signature:", link.hello())
    saved = {f: link.read_config_raw(link.meta.array_offset('can', 'bus', BUS, f), 1)[0]
             for f in ('enabled', 'bitrate', 'listen_only')}
    saved_script = link.get_script()
    ser = None
    try:
        # A frame to send, so the bus has something to fail at. Nothing completes without traffic,
        # and a bus with nothing to say cannot demonstrate that it cannot say it.
        link.set_script("""
function onTick()
  canSend(%d, 0x7A0, {1,2,3,4,5,6,7,8})
end
setTickRate(50)
""" % BUS)
        link.execute(f"canloop {BUS} 0")       # on the wire, not loopback

        # ---- control: both ends agree -------------------------------------------------------
        # A CLEAN BUS FIRST. The error counters and the error-passive state survive a run, and the
        # refusal count is cumulative for the life of the boot — so a control taken straight after a
        # previous run inherits its wreckage and reports a fault that is not this one's. Switching
        # the bus off and on is a full re-init, which is the only thing that clears them.
        print("\n--- control: ECU 500k, peer 500k ---")
        ser = adapter_at(500_000)
        bus_set(link, 'listen_only', 0)
        bus_set(link, 'bitrate', RATE_IDX[500_000])
        bus_set(link, 'enabled', 0)
        bus_set(link, 'enabled', 1)
        load, fail0, st, le, dtc_n, codes = settle(link, 4.0)
        print(f"  load={load:.1f}%  refused={fail0}  state={STATE.get(st,st)}  "
              f"last_err={LASTERR.get(le,le)}  active={sorted(hex(c) for c in codes)}")
        check(load > 0.0, "frames are completing when both ends agree", f"{load:.1f}% load")
        # NOT "state is Active": the RECEIVE error counter is what pins a controller error-passive
        # after a mismatch, and REC only decays on a frame successfully RECEIVED — so with the peer
        # idle it can sit at 255 long after transmission is healthy again. Measured: TEC 0, REC 255,
        # state Passive, while frames were completing perfectly. What a healthy bus owes us is a
        # current error of None and frames that complete, and those are not sticky.
        check(le == 0, "no current error on the wire", LASTERR.get(le, le))
        check(P_CAN1_NO_COMPLETE not in codes, "no bus fault is raised",
              f"{len(codes)} other code(s) active")

        # ---- treatment: the ECU alone moves ---------------------------------------------------
        print("\n--- treatment: ECU 1 Mbit, peer still 500k ---")
        bus_set(link, 'bitrate', RATE_IDX[1_000_000])
        load, fail1, st, le, dtc_n, codes = settle(link, 5.0)
        print(f"  load={load:.1f}%  refused={fail1}  state={STATE.get(st,st)}  "
              f"last_err={LASTERR.get(le,le)}  active={sorted(hex(c) for c in codes)}")
        check(fail1 > fail0, "the driver starts refusing frames", f"{fail0} -> {fail1}")
        check(load < 0.5, "and the load reads zero, because nothing completes", f"{load:.1f}%")
        check(st >= 2, "the controller goes error-passive or bus-off", STATE.get(st, st))
        check(le in (1, 2), "the error is STUFF or FORM — a bit-rate disagreement, not a missing "
                            "peer", LASTERR.get(le, le))
        check(P_CAN1_NO_COMPLETE in codes, "P1657 is raised",
              f"active={sorted(hex(c) for c in codes)}")

        # ---- recovery: the peer follows --------------------------------------------------------
        print("\n--- recovery: peer moved to 1 Mbit ---")
        ser.close(); ser = adapter_at(1_000_000)
        # Bounce the rate to clear the mailboxes: the stuck frames retransmit for ever on their own,
        # so without a re-init this measures the previous case's wreckage.
        bus_set(link, 'bitrate', RATE_IDX[500_000]); bus_set(link, 'bitrate', RATE_IDX[1_000_000])
        load, fail2, st, le, dtc_n, codes = settle(link, 5.0)
        print(f"  load={load:.1f}%  refused={fail2}  state={STATE.get(st,st)}  "
              f"last_err={LASTERR.get(le,le)}  active={sorted(hex(c) for c in codes)}")
        check(load > 0.0, "frames complete again once both ends agree", f"{load:.1f}% load")
        check(P_CAN1_NO_COMPLETE not in codes, "and the fault heals",
              f"active={sorted(hex(c) for c in codes)}")

    finally:
        print("\n--- restoring ---")
        try:
            link.restore_script()
        except Exception:
            pass
        for f, v in saved.items():
            try:
                link.write_raw(link.meta.array_offset('can', 'bus', BUS, f), struct.pack('<B', v))
            except Exception:
                pass
        if ser:
            try:
                ser.write(b'C\r'); time.sleep(0.1); ser.close()
            except Exception:
                pass
        print(f"\n{'ALL PASS' if not fails else str(fails) + ' FAILURE(S)'}")
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
