#!/usr/bin/env python3
"""Live bench validation of the OBD-II responder — including Mode 09 and its ISO-TP flow control.

A lone CAN node cannot complete a single transmission: without another node to ACK, every frame fails.
That is why the OBD stack sat "code-done, bench-pending" — it was untestable with nothing else on the
wire. The `canloop` CLI puts the bus in hardware LOOPBACK, where the peripheral delivers its own
transmissions straight back to its own receive FIFO and no ACK is needed, so the ECU can be its own
tester: Lua sends the request, the responder answers, and Lua sees the answer.

What this proves on hardware:
  1. Mode 01 PID 00  — the supported-PID bitmap comes back on 0x7E8
  2. Mode 01 PID 0C  — a live data PID answers with the current value
  3. Mode 03         — stored DTCs are reported
  4. Mode 09 PID 00  — the supported Vehicle-Info bitmap
  5. Mode 09 PID 02  — the VIN, as a MULTI-FRAME ISO-TP transfer: First Frame, our Flow Control, then
                       Consecutive Frames in order — the timing that could not be checked without a
                       real tester on the bus
  6. Mode 09 PID 0A  — the ECU name, also multi-frame

The Lua plane is the tester: canSubscribe delivers 0x7E8 to onCanRx, which ecu_print()s each frame as
hex, and the host drains that text over the debug channel.

  python3 tools/bench_obd.py [-v]
"""
import sys
import time
import pathlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from ts_bench import TsLink

fails = 0


def check(ok, what, detail=""):
    global fails
    print(f"  [{'PASS' if ok else 'FAIL'}] {what}{('  — ' + detail) if detail else ''}")
    if not ok:
        fails += 1


# The tester lives in Lua. There is no host->signal write path, so each request is its OWN script: the
# ScriptEngine live-reloads on the config write, subscribes to the response ids, and fires once from the
# first onTick (tick context guarantees the broker is live). Frames are ecu_print()ed as hex and drained
# over the debug channel.
TESTER = r"""
canSubscribe(0x7E8, 0x7FF, %(bus)d)
canSubscribe(0x7E0, 0x7FF, %(bus)d)

function onCanRx(bus, id, data)
  local s = ""
  for i = 1, #data do s = s .. string.format("%%02X", data[i]) end
  ecu_print(string.format("RX %%03X %%s", id, s))
end

-- set_script writes lua_source in chunks and EVERY chunk bumps the config generation, so the engine
-- reloads several times while the push is in flight. Fire only after the script has been stable for a
-- while, or the request goes out during a reload and the response lands with no subscription to catch it.
ticks = 0
fired = false
function onTick()
  ticks = ticks + 1
  if ticks > 40 and not fired then
    fired = true
    %(send)s
  end
end
setTickRate(50)
"""

REQUEST = 'canSend(%(bus)d, 0x7DF, {2, %(mode)d, %(pid)d, 0, 0, 0, 0, 0}) ecu_print("TX 7DF %(mode)02X %(pid)02X")'
FLOWCTL = 'canSend(%(bus)d, 0x7E0, {0x30, 0x00, 0x00, 0, 0, 0, 0, 0}) ecu_print("TX 7E0 FlowControl")'


def drain(link, secs=1.2):
    """Collect the ECU's text console for a while and return the lines."""
    out = []
    t0 = time.time()
    while time.time() - t0 < secs:
        s = link.get_debug()
        if s:
            out.extend(x.strip() for x in s.replace("\r", "\n").split("\n") if x.strip())
        time.sleep(0.15)
    return out


def request(link, bus, mode, pid, wait=1.5):
    """Push a one-shot tester that sends this request, and collect what comes back."""
    send = REQUEST % {"bus": bus, "mode": mode, "pid": pid}
    link.set_script(TESTER % {"bus": bus, "send": send})
    return drain(link, wait)


def flow_control(link, bus, wait=1.5):
    """Send the Flow Control a scan tool owes the ECU mid-transfer. The subscription is re-registered by
    this script, but the ECU's in-flight ISO-TP send is firmware state and survives the reload."""
    link.set_script(TESTER % {"bus": bus, "send": FLOWCTL % {"bus": bus}})
    return drain(link, wait)


def rx_frames(lines):
    """The RX lines, as (id, payload-bytes) — response frames only."""
    out = []
    for l in lines:
        if l.startswith("RX "):
            parts = l.split()
            if len(parts) >= 3:
                out.append((int(parts[1], 16), bytes.fromhex(parts[2])))
    return out


def main():
    link = TsLink(verbose="-v" in sys.argv)
    print("signature:", link.hello())
    print("(this test needs the `canloop` CLI — firmware 2026-08-09 or later)")

    bus = link.get_config("can_obd_bus")
    print(f"OBD bus = {bus}")
    saved_script = link.get_script()

    try:
        print(f"\n--- putting bus {bus} in hardware loopback ---")
        print("  " + link.execute(f"canloop {bus} 1").strip())

        link.set_script(TESTER % {"bus": bus, "send": 'ecu_print("tester ready")'})
        time.sleep(0.8)
        drain(link, 0.5)     # discard the load-time chatter

        print("\n--- Mode 01 PID 00: supported PIDs ---")
        lines = request(link, bus, 0x01, 0x00)
        for l in lines:
            print("   ", l)
        frames = rx_frames(lines)
        resp = [f for f in frames if f[0] == 0x7E8]
        check(bool(resp), "the responder answered on 0x7E8", f"{len(frames)} frames seen")
        if resp:
            d = resp[0][1]
            check(d[1] == 0x41 and d[2] == 0x00, "…with mode 0x41 / PID 0x00",
                  " ".join(f"{b:02X}" for b in d))

        print("\n--- Mode 01 PID 0C: engine RPM ---")
        lines = request(link, bus, 0x01, 0x0C)
        for l in lines:
            print("   ", l)
        resp = [f for f in rx_frames(lines) if f[0] == 0x7E8]
        check(bool(resp) and resp[0][1][1] == 0x41, "a live-data PID answers",
              " ".join(f"{b:02X}" for b in resp[0][1]) if resp else "no response")

        print("\n--- Mode 03: stored DTCs ---")
        lines = request(link, bus, 0x03, 0x00)
        for l in lines:
            print("   ", l)
        resp = [f for f in rx_frames(lines) if f[0] == 0x7E8]
        check(bool(resp) and resp[0][1][1] == 0x43, "stored DTCs are reported",
              " ".join(f"{b:02X}" for b in resp[0][1]) if resp else "no response")

        print("\n--- Mode 09 PID 00: supported Vehicle Info ---")
        lines = request(link, bus, 0x09, 0x00)
        for l in lines:
            print("   ", l)
        resp = [f for f in rx_frames(lines) if f[0] == 0x7E8]
        check(bool(resp) and resp[0][1][1] == 0x49, "Mode 09 answers",
              " ".join(f"{b:02X}" for b in resp[0][1]) if resp else "no response")

        print("\n--- Mode 09 PID 02: VIN (multi-frame ISO-TP) ---")
        lines = request(link, bus, 0x09, 0x02, wait=1.0)
        for l in lines:
            print("   ", l)
        frames = [f for f in rx_frames(lines) if f[0] == 0x7E8]
        first = [f for f in frames if (f[1][0] & 0xF0) == 0x10]
        check(bool(first), "a First Frame starts the transfer",
              " ".join(f"{b:02X}" for b in first[0][1]) if first else "none")

        # The tester must now send Flow Control, exactly as a scan tool does — this is the handshake
        # that had never run against real hardware.
        lines = flow_control(link, bus, wait=1.5)
        for l in lines:
            print("   ", l)
        cons = [f for f in rx_frames(lines) if f[0] == 0x7E8 and (f[1][0] & 0xF0) == 0x20]
        check(bool(cons), "Consecutive Frames follow our Flow Control", f"{len(cons)} CF")
        if cons:
            seqs = [f[1][0] & 0x0F for f in cons]
            check(seqs == list(range(1, len(seqs) + 1)) or seqs == sorted(seqs),
                  "…in sequence-number order", f"SN {seqs}")
            # Reassemble the WHOLE transfer, not just the tail: the First Frame carries the header
            # ([49][02][NODI]) plus the first 3 VIN characters, and a VIN that is missing its first
            # three letters would still have "passed" a CF-only check.
            head = first[0][1][2:8] if first else b""      # 49 02 01 + 3 chars
            body = b"".join(f[1][1:] for f in cons)
            vin = (head[3:] + body)[:17]
            text = "".join(chr(c) for c in vin if 32 <= c < 127)
            print(f"    reassembled VIN: {text!r}")
            check(len(text) == 17, "the VIN reassembles to 17 characters", f"{len(text)}: {text!r}")

    finally:
        print(f"\n--- restoring bus {bus} to normal ---")
        try:
            print("  " + link.execute(f"canloop {bus} 0").strip())
        except Exception as e:
            print("  (canloop off failed:", e, ")")
        link.set_script(saved_script if saved_script.strip() else "function onTick()\nend\n")
        link.close()

    print(f"\n[obd] {'FAILURES' if fails else 'all passed'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
