#!/usr/bin/env python3
"""Passively record everything on a CAN bus, and report what each message ID actually DOES.

Built to capture ground truth off a real Haltech before transcribing its broadcast protocol, but it
knows nothing about Haltech — it is a plain recorder, useful against any bus (the jayecu's own
broadcast included, which is the comparison this exists to enable).

WHAT IT REPORTS, and why each column is there:

  rate      Measured from arrival times, not assumed. A spec's stated rate and a unit's real rate are
            two different claims, and the second is the one a dash actually sees.
  bytes     Per-byte MIN and MAX across the whole capture. This is the column that answers the
            question the spec does not: a byte that never moves off 00 across thousands of frames is
            a field with nothing behind it, and a byte that never moves off some OTHER constant is a
            field being sent a deliberate "no data" value. Those are different answers and they
            matter — 0-filling a pressure encodes as -101.3 kPa through its offset.
  sample    The most recent payload, for eyeballing against the spec's byte map.

WE ACK ON PURPOSE. The adapter is opened normally (O), not in listen-only (L): a CAN transmitter
needs another node to acknowledge, and a silent listener leaves the sender retransmitting into an
unacknowledged bus until it goes error-passive. Listen-only is right for tapping a bus that already
has two nodes on it; it is wrong when the adapter IS the only other node, which is the case here.

  python3 tools/can_capture.py [seconds] [--bitrate 1000|500|250|125] [--port DEV] [--json FILE]

Defaults to 10 s at 1 Mbit, which is what the Haltech broadcast protocol runs at.
"""
import argparse
import json
import sys
import time
from collections import defaultdict

import serial

DEFAULT_PORT = '/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0'
TTY_BAUD = 2000000        # matches ~/can0up.sh: slcand -S2000000

# slcan bitrate selectors. S8 = 1 Mbit, which is what Haltech runs and what can0up.sh does NOT set
# (it uses S6/500k for the OBD work) — a mismatch here looks exactly like an unplugged cable.
RATE_CODE = {1000: b"S8\r", 800: b"S7\r", 500: b"S6\r", 250: b"S5\r", 125: b"S4\r"}


class Slcan:
    def __init__(self, port, bitrate):
        self.s = serial.Serial(port, TTY_BAUD, timeout=0.05)
        time.sleep(0.3)
        self.s.reset_input_buffer()
        self._cmd(b"C\r")                      # a previous run may have left the channel open
        self._cmd(RATE_CODE[bitrate])
        self._cmd(b"O\r")                      # normal mode: we ACK. See the module docstring.
        time.sleep(0.5)
        self.s.reset_input_buffer()
        self.buf = b""

    def _cmd(self, c, wait=0.25):
        self.s.write(c)
        time.sleep(wait)
        return self.s.read(256)

    def read_frames(self):
        """Yield (can_id, data) for everything buffered. Non-blocking."""
        self.buf += self.s.read(65536)
        while b"\r" in self.buf:
            line, self.buf = self.buf.split(b"\r", 1)
            # Status bytes (BEL 0x07 on error, bare CRs) arrive glued to the front of a frame, so
            # anchor on the frame-start character instead of assuming the line begins with one.
            starts = [i for i in (line.find(c) for c in (b"t", b"T")) if i >= 0]
            if not starts:
                continue
            line = line[min(starts):].strip()
            try:
                if line[:1] == b"t":
                    cid, dlc, body = int(line[1:4], 16), int(line[4:5], 16), line[5:]
                else:
                    cid, dlc, body = int(line[1:9], 16), int(line[9:10], 16), line[10:]
                data = bytes.fromhex(body[:dlc * 2].decode())
            except Exception:
                continue
            if len(data) == dlc:
                yield cid, data

    def close(self):
        self._cmd(b"C\r")
        self.s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('seconds', nargs='?', type=float, default=10.0)
    ap.add_argument('--bitrate', type=int, default=1000, choices=sorted(RATE_CODE))
    ap.add_argument('--port', default=DEFAULT_PORT)
    ap.add_argument('--json', help='also write the report here, for diffing against another capture')
    a = ap.parse_args()

    try:
        bus = Slcan(a.port, a.bitrate)
    except serial.SerialException as e:
        print(f"cannot open {a.port}: {e}")
        return 1

    print(f"capturing {a.seconds:g}s at {a.bitrate} kbit on {a.port} …")
    count = defaultdict(int)
    first, last = {}, {}
    lo, hi = {}, {}
    sample = {}
    dlcs = {}

    t0 = time.time()
    end = t0 + a.seconds
    while time.time() < end:
        got = False
        for cid, data in bus.read_frames():
            got = True
            now = time.time()
            count[cid] += 1
            if cid not in first:
                first[cid] = now
                lo[cid] = list(data)
                hi[cid] = list(data)
            else:
                # Widen per byte. A byte whose min == max never moved in the whole capture.
                for i, b in enumerate(data):
                    if i < len(lo[cid]):
                        lo[cid][i] = min(lo[cid][i], b)
                        hi[cid][i] = max(hi[cid][i], b)
            last[cid] = now
            sample[cid] = data
            dlcs[cid] = len(data)
        if not got:
            time.sleep(0.002)
    bus.close()

    if not count:
        print("\nNOTHING RECEIVED.\n"
              "  - wrong bitrate is indistinguishable from an unplugged cable; try --bitrate 500\n"
              "  - is the sender powered and on THIS bus?\n")
        return 1

    print(f"\n{len(count)} message ids, {sum(count.values())} frames in {time.time()-t0:.1f}s\n")
    hdr = f"{'ID':>6} {'dlc':>3} {'n':>6} {'rate':>7}   {'sample':<24} {'per-byte min..max (── = static)'}"
    print(hdr)
    print("-" * len(hdr))
    report = {}
    for cid in sorted(count):
        span = last[cid] - first[cid]
        rate = (count[cid] - 1) / span if span > 0 else 0.0
        smp = " ".join(f"{b:02X}" for b in sample[cid])
        cols = []
        for i in range(dlcs[cid]):
            cols.append("──" if lo[cid][i] == hi[cid][i] else f"{lo[cid][i]:02X}-{hi[cid][i]:02X}")
        static = all(lo[cid][i] == hi[cid][i] for i in range(dlcs[cid]))
        print(f"0x{cid:03X} {dlcs[cid]:>4} {count[cid]:>6} {rate:>6.1f}Hz   {smp:<24} "
              f"{' '.join(cols)}{'   [ENTIRELY STATIC]' if static else ''}")
        report[f"0x{cid:03X}"] = {
            "dlc": dlcs[cid], "count": count[cid], "rate_hz": round(rate, 2),
            "sample": smp,
            "min": [f"{b:02X}" for b in lo[cid][:dlcs[cid]]],
            "max": [f"{b:02X}" for b in hi[cid][:dlcs[cid]]],
        }

    print("\nA byte shown as ── never changed. Across a capture of any length that means the field "
          "\nbehind it is not being driven — which is the 'no sensor fitted' answer this looks for.")

    if a.json:
        with open(a.json, 'w') as f:
            json.dump(report, f, indent=2, sort_keys=True)
        print(f"\nwritten to {a.json}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
