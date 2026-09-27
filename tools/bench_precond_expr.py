#!/usr/bin/env python3
"""Live bench validation of the sensor precondition EXPRESSION — on real hardware.

A sensor's operating-range checks (Reading Low / Reading High) only raise a DTC while its
precondition holds. That gate used to be four flat {signal, op, combine, value} slots folded
sum-of-products, which could express "A AND B OR C" but never "A AND (B OR C)". It is now a
compiled program run by the firmware's stack VM (firmware/Signal/Expr.h).

What this proves on the ECU itself, not in a host test:

  1. an EMPTY program means always armed — the reading check raises with no gate at all
  2. a compiled gate DISARMS the check when it is false, and ARMS it when true
  3. GROUPING is honoured: `map >= 50 and (tps > 80 or rpm > 4000)` stays shut with MAP high and
     both OR terms low — the case no sum-of-products folding of those three tests could represent,
     and therefore the case that proves the VM is really running
  4. a program that fails validation fails ARMED and raises its own per-sensor code, so a broken
     gate can never silently switch detection off

Method: the target sensor is absent on the bench, so its reading and the gate's channels are
injected at PRIO_LUA via signalWrite (the trick bench_fuel.py and bench_egt_dtc.py use), and the
sensor's diagnostics are armed by a RAM config write. The bytecode is assembled here from
firmware/Signal/ExprIsa.h's opcode numbers — the same numbers the studio compiler emits and the
firmware executes. Nothing is burned; a reset restores the tune exactly.

  python3 tools/bench_precond_expr.py [-v]
"""
import struct
import sys
import time
import pathlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from ts_bench import TsLink

TTL_MS = 500           # injected signals must outlive the gap between onTicks

# The sensor under test must be one whose pipeline actually RUNS on the bench, or its reading
# checks are never evaluated and every result is noise. `battery` is the bootstrap sensor: it is
# assigned, it reads a real ADC, and it runs even when the system is inactive. An absent sensor
# (clt, iat) cannot be used — injecting its CHANNEL via Lua does not feed its pipeline, which
# decodes its own input and aborts when it has none.
SENSOR = "battery"
OP_MAX = 1.0           # Reading High threshold (volts) — below the real reading, so the check
                       # trips whenever the gate ARMS it, with no stimulus needed.

# The gate's channels must be INJECTABLE — i.e. nothing on the ECU produces them at a higher
# priority. map/tps/clt have no bench sensor, so a Lua signalWrite owns them outright. `rpm` does
# NOT qualify: the trigger decoder owns it, the priority gate rejects the write, and a clause on it
# can never go true here. (Checked, not assumed: injecting rpm leaves it reading 0.)

# --- opcodes, from firmware/Signal/ExprIsa.h (values are the file format; append, never renumber)
OP_END, OP_PUSH_SIG, OP_PUSH_CFG, OP_PUSH_BCFG = 0, 1, 2, 3
OP_PUSH_F, OP_PUSH_I8, OP_PUSH_I16, OP_PUSH_ZERO, OP_PUSH_ONE = 4, 5, 6, 7, 8
OP_GT, OP_GE, OP_LT, OP_LE, OP_EQ, OP_NE = 9, 10, 11, 12, 13, 14
OP_AND, OP_OR, OP_NOT = 15, 16, 17

fails = 0
OP_MAX_CODE = 0
PRECOND_DTC = 0


def dtc_code_for(sensor_id, check_label):
    """Resolve a sensor+check to its P-code from the GENERATED reference, so the harness cannot
    drift from what codegen allocated (a standard OBD code wins over the manufacturer block)."""
    import re
    doc = pathlib.Path(__file__).resolve().parent.parent / "docs" / "dtc-codes.md"
    for line in doc.read_text().splitlines():
        m = re.match(r"\|\s*`P([0-9A-F]{4})`\s*\|[^|]*\(`([^`]+)`\)\s*\|\s*([^|]+?)\s*\|", line)
        if m and m.group(2) == sensor_id and m.group(3) == check_label:
            return int(m.group(1), 16)
    return None


def check(ok, what, detail=""):
    global fails
    print(f"  [{'PASS' if ok else 'FAIL'}] {what}{('  — ' + detail) if detail else ''}")
    if not ok:
        fails += 1


class Asm:
    """Assemble bytecode exactly as the studio's ExprCompiler emits it."""

    def __init__(self, meta):
        self.b = bytearray()
        self.sig = meta.signals

    def channel(self, name):
        self.b += bytes([OP_PUSH_SIG]) + struct.pack("<H", self.sig[name] + 1)  # selector = id + 1
        return self

    def const(self, v):
        self.b += bytes([OP_PUSH_F]) + struct.pack("<i", int(round(v * 100)))   # x100 fixed point
        return self

    def op(self, o):
        self.b.append(o)
        return self

    def end(self):
        self.b.append(OP_END)
        return bytes(self.b)


def inject(link, values, hold_s=1.2):
    """Hold a set of channels at given values via Lua signalWrite (they are absent on the bench)."""
    body = "\n".join(f'  signalWrite("{k}", {v}, {TTL_MS})' for k, v in values.items())
    link.set_script(f"function onTick()\n{body}\nend\nsetTickRate(50)\n")
    time.sleep(hold_s)


def settle(link, values, hold_s=1.2):
    """Inject, let the OLD values expire, THEN clear the table, then let the gate act.

    Order matters. An operating-range DTC is not healed when its gate closes — Sensors.cpp leaves
    op codes alone while disarmed, deliberately — so a code raised under the PREVIOUS values stays
    raised and would be read as if the new gate had armed it. Clearing after the injection settles
    is what makes each step measure its own gate.
    """
    inject(link, values, hold_s)
    link.execute("dtc clear")
    time.sleep(hold_s)


# DtcRecord (Dtc.h, #pragma pack(1), 36 bytes) — same layout sd_dtc_dump.py parses.
DTC_REC = "<HBBBBHHIHI4f"
DTC_ACTIVE = 0x01


def dtc_active(link, want_code):
    """Is exactly THIS code currently ACTIVE?

    Deliberately not the global active COUNT: that drifts on its own as unrelated codes heal, so a
    test watching it reports whatever the rest of the ECU happened to be doing. The first version of
    this harness did that and 'passed' three checks on noise.
    """
    _rtype, data = link.cmd(b"G")
    if len(data) < 10:
        return False
    _magic, _ver, _boot, n = struct.unpack_from("<IHHH", data, 0)
    off, rec = 10, struct.calcsize(DTC_REC)
    for _ in range(n):
        if off + rec > len(data):
            break
        code, _src, _sev, status = struct.unpack_from("<HBBB", data, off)[:4]
        off += rec
        if code == want_code:
            return bool(status & DTC_ACTIVE)
    return False


def main():
    link = TsLink(verbose="-v" in sys.argv)
    print("signature:", link.hello())
    meta = link.meta

    arr = meta.array("sensors", "sensor")
    ids = arr.get("element_ids") or []
    if SENSOR not in ids:
        print(f"!! sensor '{SENSOR}' not in the catalog — cannot run")
        return 1
    idx = ids.index(SENSOR)

    def off(field):
        return meta.array_offset("sensors", "sensor", idx, field)

    expr_off = off("precond_expr")
    expr_len = arr["fields"]["precond_expr"]["size"]
    print(f"sensor {SENSOR} (index {idx}): precond_expr @ {expr_off}, {expr_len} bytes")

    # Per-sensor codes from the generated catalog block: P1000 + (index * 8) + slot.
    # Slot 3 = operating_max, slot 6 = precondition invalid. A sensor with a STANDARD OBD code for
    # its reading check keeps that instead, so resolve op_max from the DTC reference rather than
    # assuming the manufacturer block.
    global OP_MAX_CODE, PRECOND_DTC
    PRECOND_DTC = 0x1000 + (idx << 3) + 6
    OP_MAX_CODE = dtc_code_for(SENSOR, "Detect Reading High")
    if OP_MAX_CODE is None:
        print(f"!! no Reading High code for {SENSOR} in docs/dtc-codes.md")
        return 1

    saved_expr = link.read_config_raw(expr_off, expr_len)
    saved_script = link.get_script()
    saved = {f: link.read_config_raw(off(f), 2) for f in ("diag_enable", "diag_op_max", "diag_severity")}

    def set_expr(code):
        payload = code + bytes(expr_len - len(code))       # zero-fill the rest of the block
        link.write_raw(expr_off, payload)
        time.sleep(0.4)                                    # let the config generation bump land

    try:
        # --- arm the sensor's Reading High check ---
        link.write_raw(off("enabled"), bytes([1]))
        link.write_raw(off("diag_enable"), struct.pack("<H", 0x08))          # op_max only
        link.write_raw(off("diag_op_max"), struct.pack("<h", int(OP_MAX * 100)))
        link.write_raw(off("diag_severity"), struct.pack("<H", 3 << 6))      # op_max severity 3
        link.write_raw(off("diag_delay_ms"), struct.pack("<H", 0))           # no persistence delay
        time.sleep(0.5)
        link.execute("dtc clear")
        time.sleep(0.3)

        print(f"\n  watching P{OP_MAX_CODE:04X} ({SENSOR} Detect Reading High) specifically")

        print("\n--- 1. EMPTY program = always armed ---")
        set_expr(bytes([OP_END]))
        settle(link, {"map": 0, "tps": 0, "clt": 0})
        check(dtc_active(link, OP_MAX_CODE), "the reading check raises with no gate")

        print("\n--- 2. gate `map >= 50`, MAP low -> DISARMED ---")
        prog = Asm(meta).channel("map").const(50).op(OP_GE).end()
        print(f"  program: {len(prog)} bytes  {prog.hex()}")
        set_expr(prog)
        settle(link, {"map": 20, "tps": 0, "clt": 0})
        check(not dtc_active(link, OP_MAX_CODE), "a false gate disarms the reading check")

        print("\n--- 3. same gate, MAP high -> ARMED ---")
        settle(link, {"map": 80, "tps": 0, "clt": 0})
        check(dtc_active(link, OP_MAX_CODE), "a true gate arms it again")

        print("\n--- 4. GROUPING: map >= 50 and (tps > 80 or clt > 90) ---")
        # The case the four flat slots could not express. MAP is high, but BOTH terms of the OR are
        # low, so the gate must be SHUT. Any sum-of-products folding of these three tests arms here.
        prog = (Asm(meta).channel("map").const(50).op(OP_GE)
                          .channel("tps").const(80).op(OP_GT)
                          .channel("clt").const(90).op(OP_GT)
                          .op(OP_OR).op(OP_AND).end())
        print(f"  program: {len(prog)} bytes  {prog.hex()}")
        set_expr(prog)
        settle(link, {"map": 80, "tps": 10, "clt": 20})
        check(not dtc_active(link, OP_MAX_CODE),
              "MAP high but neither OR term true -> still DISARMED")

        print("\n--- 5. …and CLT alone carries the OR ---")
        settle(link, {"map": 80, "tps": 10, "clt": 95})
        check(dtc_active(link, OP_MAX_CODE), "one live OR term arms the whole gate")

        print("\n--- 6. …and TPS alone carries it too ---")
        settle(link, {"map": 80, "tps": 95, "clt": 20})
        check(dtc_active(link, OP_MAX_CODE), "the other OR term arms it as well")

        print("\n--- 7. AND still gates the whole thing: MAP low, OR term true ---")
        settle(link, {"map": 10, "tps": 95, "clt": 95})
        check(not dtc_active(link, OP_MAX_CODE), "a false AND term shuts it regardless of the OR")

        print("\n--- 8. a BROKEN program fails ARMED and names the sensor ---")
        set_expr(bytes([OP_AND, OP_END]))            # binary op on an empty stack: rejected at load
        settle(link, {"map": 10, "tps": 0, "clt": 0})   # the gate would be FALSE if it ran
        check(dtc_active(link, OP_MAX_CODE),
              "detection stays ON despite the broken gate")
        check(dtc_active(link, PRECOND_DTC),
              f"the broken expression raises its own code P{PRECOND_DTC:04X}")

    finally:
        print("\n--- restoring the bench ---")
        link.write_raw(expr_off, saved_expr)
        for f, v in saved.items():
            link.write_raw(off(f), v)
        link.set_script(saved_script)
        time.sleep(0.5)
        link.close()

    print(f"\n{'FAILED' if fails else 'All precondition-expression bench checks passed'} "
          f"({fails} failure{'' if fails == 1 else 's'})")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
