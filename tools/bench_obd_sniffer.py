#!/usr/bin/env python3
"""OBD-II over the WIRE, with the CAN adapter as the tester — no root, no loopback.

bench_obd.py proves the responder using the ECU's own hardware loopback, where the ECU is both asker and
answerer. This is the real thing: the slcan adapter on CAN0 sends the requests and reads the replies, so
every frame crosses two transceivers, is ACKed by the other node, and the ISO-TP Flow Control is issued
by a genuinely separate device. That FC handshake is what the gap audit said "needs a scan-tool pass".

Talks slcan straight over the tty, so it needs no slcand and no root.

  python3 tools/bench_obd_sniffer.py
"""
import sys
import time

import serial

PORT = '/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0'
TTY_BAUD = 2000000        # matches ~/can0up.sh: slcand -S2000000
REQ_FUNC = 0x7DF
REQ_PHYS = 0x7E0
RESP = 0x7E8

fails = 0


def check(ok, what, detail=""):
    global fails
    print(f"  [{'PASS' if ok else 'FAIL'}] {what}{('  — ' + detail) if detail else ''}")
    if not ok:
        fails += 1


class Slcan:
    def __init__(self, port=PORT, baud=TTY_BAUD):
        self.s = serial.Serial(port, baud, timeout=0.2)
        time.sleep(0.3)
        self.s.reset_input_buffer()
        self._cmd(b"C\r")          # close first: the channel may be left open from a previous run
        self._cmd(b"S6\r")         # 500 kbit
        self._cmd(b"O\r")          # open
        # The adapter needs a moment after O before it starts delivering RX frames; without this the
        # first request's reply is simply gone and the run looks like the ECU never answered.
        time.sleep(1.5)
        self.s.reset_input_buffer()
        self.buf = b""

    def _cmd(self, c, wait=0.25):
        self.s.write(c)
        time.sleep(wait)
        return self.s.read(256)

    def send(self, can_id, data):
        frame = f"t{can_id:03X}{len(data)}{''.join(f'{b:02X}' for b in data)}\r".encode()
        self.s.write(frame)

    def recv(self, want=RESP, timeout=1.0):
        """Collect frames addressed to `want` until the bus goes quiet."""
        out, end = [], time.time() + timeout
        while time.time() < end:
            self.buf += self.s.read(4096)
            while b"\r" in self.buf:
                line, self.buf = self.buf.split(b"\r", 1)
                # The adapter emits status bytes (BEL 0x07 = error, and bare CRs) that are NOT
                # newline-terminated, so a frame often arrives glued behind one: b"\x07t7E88...".
                # Anchor on the frame-start character rather than assuming the line begins with it —
                # checking line[0] silently threw away every reply the ECU sent.
                start = min([i for i in (line.find(c) for c in (b"t", b"T", b"r", b"R")) if i >= 0],
                            default=-1)
                if start < 0:
                    continue
                line = line[start:].strip()
                try:
                    if line[:1] == b"t":
                        cid = int(line[1:4], 16)
                        dlc = int(line[4:5], 16)
                        data = bytes.fromhex(line[5:5 + dlc * 2].decode())
                    else:
                        cid = int(line[1:9], 16)
                        dlc = int(line[9:10], 16)
                        data = bytes.fromhex(line[10:10 + dlc * 2].decode())
                except Exception:
                    continue
                if cid == want:
                    out.append(data)
        return out

    def close(self):
        self._cmd(b"C\r")
        self.s.close()


def ask(bus, data, want=RESP, tries=3, timeout=1.0):
    """Send a request and read the reply, retrying — one lost frame on a shared wire is not a verdict."""
    for _ in range(tries):
        bus.send(REQ_FUNC, data)
        r = bus.recv(want, timeout=timeout)
        if r:
            return r
        time.sleep(0.3)
    return []


def hexs(b):
    return " ".join(f"{x:02X}" for x in b)


def main():
    bus = Slcan()
    print("slcan adapter open on CAN0 @500k — it is the tester; the ECU is the other node.\n")

    print("--- Mode 01 PID 00: supported PIDs ---")
    r = ask(bus, [0x02, 0x01, 0x00])
    for f in r:
        print("   RX 7E8", hexs(f))
    check(bool(r), "the ECU answered a request from a SEPARATE device on the wire", f"{len(r)} frames")
    if r:
        check(r[0][1] == 0x41 and r[0][2] == 0x00, "…mode 0x41 / PID 0x00", hexs(r[0]))

    print("\n--- Mode 01 PID 0C: engine RPM ---")
    r = ask(bus, [0x02, 0x01, 0x0C])
    for f in r:
        print("   RX 7E8", hexs(f))
    check(bool(r) and r[0][1] == 0x41, "live data over the wire", hexs(r[0]) if r else "none")

    print("\n--- Mode 03: stored DTCs ---")
    r = ask(bus, [0x02, 0x03])
    for f in r:
        print("   RX 7E8", hexs(f))
    check(bool(r) and r[0][1] == 0x43, "stored DTCs reported", hexs(r[0]) if r else "none")

    print("\n--- Mode 09 PID 00: supported Vehicle Info ---")
    r = ask(bus, [0x02, 0x09, 0x00])
    for f in r:
        print("   RX 7E8", hexs(f))
    check(bool(r) and r[0][1] == 0x49, "Mode 09 answers", hexs(r[0]) if r else "none")

    print("\n--- Mode 09 PID 02: VIN, multi-frame, OUR Flow Control ---")
    first = ask(bus, [0x02, 0x09, 0x02], timeout=0.8)
    for f in first:
        print("   RX 7E8", hexs(f))
    ff = [f for f in first if (f[0] & 0xF0) == 0x10]
    check(bool(ff), "a First Frame arrives", hexs(ff[0]) if ff else "none")

    # The handshake the gap audit wanted a scan tool for: a real Flow Control from the other device.
    bus.send(REQ_PHYS, [0x30, 0x00, 0x00])
    print("   TX 7E0 30 00 00   (Flow Control: clear to send)")
    cfs = bus.recv(timeout=1.5)
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

    bus.close()
    print(f"\n[obd-sniffer] {'FAILURES' if fails else 'all passed'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
