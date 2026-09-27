#!/usr/bin/env python3
"""Find out whether ANYTHING is listening on an ECU CAN bus, and at what bitrate.

A CAN transmitter needs another node to ACK. With nobody there, frames stick in the three bxCAN TX
mailboxes and the fourth send fails — and a node listening at the WRONG bitrate behaves identically, so
"no ACK" alone cannot tell a wiring fault from a settings mismatch. Sweeping the ECU's own bitrate can:
if some rate suddenly drains the mailboxes, a device is out there and that is its rate.

  python3 tools/can_probe.py [bus]          (default bus 0)
"""
import sys
import time
import pathlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from ts_bench import TsLink

RATES = [1000, 800, 500, 250, 125, 100, 50, 33, 20, 10]   # kbit
BURST = 10          # more than the three TX mailboxes, so saturation is unambiguous

PROBE = '''
ticks = 0
sent = 0
ok = 0
function onTick()
  ticks = ticks + 1
  if ticks > 30 and sent < %(burst)d then
    sent = sent + 1
    if canSend(%(bus)d, 0x123, {1,2,3,4,5,6,7,8}) then ok = ok + 1 end
    if sent == %(burst)d then ecu_print("acked=" .. ok .. "/" .. sent) end
  end
end
setTickRate(50)
'''


def main():
    bus = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    link = TsLink()
    print("signature:", link.hello())
    saved = link.get_script()
    print(f"\nProbing bus {bus}. >3 completions means the mailboxes are draining — something ACKed.\n")
    found = []
    try:
        link.execute(f"canloop {bus} 0")          # normal mode: the wire, not ourselves
        for kbit in RATES:
            print("  " + link.execute(f"canbaud {bus} {kbit}").strip(), end="   ")
            link.set_script(PROBE % {"bus": bus, "burst": BURST})
            t0, out = time.time(), ""
            while time.time() - t0 < 3.0 and "acked=" not in out:
                out += link.get_debug()
                time.sleep(0.2)
            line = [l for l in out.replace("\r", "\n").split("\n") if "acked=" in l]
            res = line[0].strip() if line else "no answer"
            n = int(res.split("=")[1].split("/")[0]) if "acked=" in res else -1
            verdict = "  <== A NODE IS LISTENING HERE" if n > 3 else ""
            print(f"{res}{verdict}")
            if n > 3:
                found.append((kbit, n))
    finally:
        link.execute(f"canbaud {bus} 500")        # back to the OBD default
        link.set_script(saved if saved.strip() else "function onTick()\nend\n")
        link.close()

    print()
    if found:
        print(f"Something is on bus {bus} at: " + ", ".join(f"{k} kbit ({n}/{BURST})" for k, n in found))
    else:
        print(f"Nothing ACKed on bus {bus} at any bitrate — no live CAN node on that wire.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
