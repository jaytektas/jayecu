#!/usr/bin/env python3
"""OBD-II validation ON THE WIRE, with the host as a real external tester.

bench_obd.py proves the responder using the ECU's own hardware loopback — good, but the ECU is both
asker and answerer. This is the other thing: with the CANable up as `can0`, the HOST is a genuine second
node on the bus. Every frame here crosses real transceivers, is ACKed by the other node, and the ISO-TP
Flow Control comes from a different device entirely — which is what the gap audit meant by "needs a real
scan tool".

Prerequisites (root, so run it yourself):
    ~/can0up.sh                # slcand -o -c -s6 -S2000000 <canable> can0 ; ip link set up can0
and the ECU's OBD bus must be the one the CANable is wired to (CAN0 here; can_obd_bus = 0, burned).

    python3 tools/bench_obd_wire.py
"""
import os
import socket
import struct
import subprocess
import sys
import time

CAN_IFACE = os.environ.get("CAN_IFACE", "can0")
REQ_FUNC = 0x7DF        # functional request address
REQ_PHYS = 0x7E0        # physical request address (Flow Control goes here)
RESP = 0x7E8            # the ECU's response address

fails = 0


def check(ok, what, detail=""):
    global fails
    print(f"  [{'PASS' if ok else 'FAIL'}] {what}{('  — ' + detail) if detail else ''}")
    if not ok:
        fails += 1


def open_bus():
    s = socket.socket(socket.PF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    s.bind((CAN_IFACE,))
    s.settimeout(1.0)
    return s


def send(s, can_id, data):
    payload = bytes(data) + b"\x00" * (8 - len(data))
    s.send(struct.pack("=IB3x8s", can_id, 8, payload))


def recv(s, want_id=RESP, timeout=1.0):
    """Collect frames from `want_id` until the bus goes quiet."""
    out = []
    end = time.time() + timeout
    while time.time() < end:
        try:
            s.settimeout(max(0.05, end - time.time()))
            frame = s.recv(16)
        except socket.timeout:
            break
        can_id, dlc, payload = struct.unpack("=IB3x8s", frame)
        can_id &= socket.CAN_EFF_MASK
        if can_id == want_id:
            out.append(payload[:dlc])
    return out


def hexs(b):
    return " ".join(f"{x:02X}" for x in b)


def main():
    if not os.path.exists(f"/sys/class/net/{CAN_IFACE}"):
        print(f"{CAN_IFACE} does not exist — bring the CANable up first:  ~/can0up.sh")
        return 2
    with open(f"/sys/class/net/{CAN_IFACE}/operstate") as f:
        state = f.read().strip()
    print(f"{CAN_IFACE} operstate = {state}")

    s = open_bus()
    print(f"\nHost is now a second node on {CAN_IFACE}. Every exchange below is on the wire.\n")

    print("--- Mode 01 PID 00: supported PIDs ---")
    send(s, REQ_FUNC, [0x02, 0x01, 0x00])
    r = recv(s)
    for f in r:
        print("   RX 7E8", hexs(f))
    check(bool(r), "the ECU answered a request from a DIFFERENT node", f"{len(r)} frames")
    if r:
        check(r[0][1] == 0x41 and r[0][2] == 0x00, "…mode 0x41 / PID 0x00", hexs(r[0]))

    print("\n--- Mode 01 PID 0C: engine RPM ---")
    send(s, REQ_FUNC, [0x02, 0x01, 0x0C])
    r = recv(s)
    for f in r:
        print("   RX 7E8", hexs(f))
    check(bool(r) and r[0][1] == 0x41, "live data answers on the wire", hexs(r[0]) if r else "none")

    print("\n--- Mode 03: stored DTCs ---")
    send(s, REQ_FUNC, [0x02, 0x03])
    r = recv(s)
    for f in r:
        print("   RX 7E8", hexs(f))
    check(bool(r) and r[0][1] == 0x43, "stored DTCs reported", hexs(r[0]) if r else "none")

    print("\n--- Mode 09 PID 02: VIN, multi-frame, OUR Flow Control ---")
    send(s, REQ_FUNC, [0x02, 0x09, 0x02])
    first = recv(s, timeout=0.6)
    for f in first:
        print("   RX 7E8", hexs(f))
    ff = [f for f in first if (f[0] & 0xF0) == 0x10]
    check(bool(ff), "a First Frame arrives", hexs(ff[0]) if ff else "none")

    # THE handshake: a real Flow Control, sent by a real other node, over a real bus.
    send(s, REQ_PHYS, [0x30, 0x00, 0x00])
    print("   TX 7E0 30 00 00   (Flow Control: clear to send)")
    cfs = recv(s, timeout=1.5)
    for f in cfs:
        print("   RX 7E8", hexs(f))
    cons = [f for f in cfs if (f[0] & 0xF0) == 0x20]
    check(bool(cons), "Consecutive Frames follow OUR Flow Control", f"{len(cons)} CF")
    if ff and cons:
        seqs = [f[0] & 0x0F for f in cons]
        check(seqs == list(range(1, len(seqs) + 1)), "…in sequence order", f"SN {seqs}")
        vin = (ff[0][5:8] + b"".join(f[1:] for f in cons))[:17]
        text = "".join(chr(c) for c in vin if 32 <= c < 127)
        print(f"    reassembled VIN: {text!r}")
        check(len(text) == 17, "the VIN reassembles to 17 characters", f"{len(text)}: {text!r}")

    s.close()
    print(f"\n[obd-wire] {'FAILURES' if fails else 'all passed'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
