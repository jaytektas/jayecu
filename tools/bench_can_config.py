#!/usr/bin/env python3
"""Live bench validation of the per-bus CAN settings: bitrate, listen-only, enable.

The bitrate used to be a hardcoded 500 kbit in platform_can_init, so a CAN device on any other rate was
unreachable and no tune could say otherwise. These are now tune values, applied by the composition tier
(main) through platform_can — and applied LIVE, so a config write re-inits the bus without a reset.

The slcan adapter on CAN0 is the ideal peer to test against, because ACK is precisely what these settings
change. A frame only completes if another node ACKs it, so counting completions distinguishes every case:

  bus enabled, 500k, adapter 500k   -> completions keep coming        (agreed rate: it ACKs)
  bus at 250k, adapter still 500k   -> mailboxes fill, then nothing   (mismatch never ACKs)
  bus at 250k, adapter moved to 250k-> completions again              (the CONFIG drove the hardware)
  listen-only                       -> nothing transmits at all       (silent: we never drive the wire)
  bus disabled                      -> nothing transmits at all       (bus down)

  python3 tools/bench_can_config.py
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
fails = 0


def check(ok, what, detail=""):
    global fails
    print(f"  [{'PASS' if ok else 'FAIL'}] {what}{('  — ' + detail) if detail else ''}")
    if not ok:
        fails += 1


PROBE = '''
ticks=0 sent=0 ok=0
function onTick()
  ticks=ticks+1
  if ticks>25 and sent<10 then
    sent=sent+1
    if canSend(%d, 0x123, {1,2,3,4,5,6,7,8}) then ok=ok+1 end
    if sent==10 then ecu_print("acked="..ok.."/10") end
  end
end
setTickRate(50)
''' % BUS


def completions(link):
    """How many of ten frames the ECU could hand to the wire. >3 means mailboxes are draining (someone
    ACKed); 3 means they filled and stuck; 0 means the bus never even accepted them.

    Drains the console FIRST: get_debug() accumulates, so reading the first "acked=" it returns can hand
    you the PREVIOUS probe's answer — which silently reports the last case's result as this one's."""
    for _ in range(20):
        if not link.get_debug():
            break
        time.sleep(0.05)
    link.set_script(PROBE)
    t0, out = time.time(), ''
    while time.time() - t0 < 4.0 and 'acked=' not in out:
        out += link.get_debug()
        time.sleep(0.2)
    for tok in out.replace('\r', '\n').split():
        if tok.startswith('acked='):
            return int(tok.split('=')[1].split('/')[0])
    return -1


def adapter_at(code):
    """Point the slcan adapter at a bitrate (S5 = 250k, S6 = 500k)."""
    s = serial.Serial(ADAPTER, 2000000, timeout=0.3)
    time.sleep(0.3)
    for c in (b'C\r', code.encode() + b'\r', b'O\r'):
        s.write(c)
        time.sleep(0.3)
        s.read(256)
    s.close()


def set_bus(link, field, value):
    off = link.meta.array_offset('can', 'bus', BUS, field)
    link.write_raw(off, struct.pack('<B', value))
    time.sleep(0.8)          # the config generation moves; can_task re-applies within a tick


def main():
    link = TsLink()
    print("signature:", link.hello())
    saved = {f: link.read_config_raw(link.meta.array_offset('can', 'bus', BUS, f), 1)[0]
             for f in ('enabled', 'bitrate', 'listen_only')}
    saved_script = link.get_script()
    print(f"saved bus{BUS} config: {saved}")

    try:
        link.execute(f"canloop {BUS} 0")            # on the wire, not loopback

        print("\n--- baseline: enabled, 500 kbit, adapter at 500 kbit ---")
        set_bus(link, 'enabled', 1); set_bus(link, 'listen_only', 0); set_bus(link, 'bitrate', 2)
        adapter_at('S6')
        n = completions(link)
        print(f"  completions = {n}")
        check(n > 3, "the adapter ACKs, so frames keep completing", f"{n}/10")

        print("\n--- bitrate 250 kbit, adapter still at 500 kbit (mismatch) ---")
        set_bus(link, 'bitrate', 1)
        n = completions(link)
        print(f"  completions = {n}")
        check(n <= 3, "a rate mismatch never ACKs — the config really re-timed the bus", f"{n}/10")

        print("\n--- adapter moved to 250 kbit to match ---")
        adapter_at('S5')
        # The mismatch left frames stuck in the mailboxes, retransmitting for ever. Nothing on the ECU
        # side changed, so no re-init cleared them — bounce the bitrate to force one, or this measures
        # the previous case's wreckage instead of the wire.
        set_bus(link, 'bitrate', 2); set_bus(link, 'bitrate', 1)
        n = completions(link)
        print(f"  completions = {n}")
        check(n > 3, "they agree again at the CONFIGURED rate", f"{n}/10")

        print("\n--- listen-only ---")
        set_bus(link, 'bitrate', 2); adapter_at('S6')
        set_bus(link, 'listen_only', 1)
        n = completions(link)
        print(f"  completions = {n}")
        check(n <= 3, "silent mode never drives the wire", f"{n}/10")
        set_bus(link, 'listen_only', 0)

        print("\n--- bus disabled ---")
        set_bus(link, 'enabled', 0)
        n = completions(link)
        print(f"  completions = {n}")
        check(n == 0, "a disabled bus accepts nothing at all", f"{n}/10")

        print("\n--- re-enabled ---")
        set_bus(link, 'enabled', 1)
        n = completions(link)
        print(f"  completions = {n}")
        check(n > 3, "and it comes straight back, live, with no reset", f"{n}/10")

    finally:
        for f, v in saved.items():
            set_bus(link, f, v)
        link.set_script(saved_script if saved_script.strip() else "function onTick()\nend\n")
        print(f"\nrestored bus{BUS} config: {saved}")
        link.close()

    print(f"\n[can-config] {'FAILURES' if fails else 'all passed'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
