#!/usr/bin/env python3
"""An output slot reading a GENERIC table, live on the bench ECU.

The point of the exercise: a slot no longer owns a duty map, it NAMES one out of the pool. So this
writes a shape into generic table 1, points row 21 (LS10)'s duty at it by id, drives the channel its axis
reads, and checks the commanded output follows the table — the whole path (selector -> registry ->
descriptor -> interpolation -> arbitration -> emit -> out_22 telemetry), on the hardware.
"""
import os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ts_bench import TsLink

L = TsLink(verbose=False)
M = L.meta
print(f"layout {M.layout_hash}  config {M.config_size}B")

# --- the table's id, resolved the way the studio does: by name, out of the registry --------------
reg = [o for o in M.config["outputs"]["output"]["fields"]["duty_table_sel"]["option_ids"]]
TID = reg.index("generic_tables_table_1")
print(f"generic_tables_table_1 = id {TID} of {len(reg)}")

# The output ROW under test — which IS the pin (outputs.output[i] = pin i). LS10, NOT IGN1: a bench
# firing map makes IGN1-4 and LS1-4 coil and injector rows, and a row cannot be both.
ROW = 21

SIG = M.signals
def sig(n): return SIG[n]

def w(name, val, fmt=None):
    c = M.c(name); L.write_raw(c["offset"], struct.pack(fmt or M.fmt(c["datatype"]), val))

def wa(idx, field, val):
    a = M.config["outputs"]["output"]
    off = M.array_offset("outputs", "output", idx, field)
    dt = a["fields"][field]["datatype"]
    L.write_raw(off, struct.pack(M.fmt(dt), val))

# --- 1. the table: 0 % at 0 C, 100 % at 100 C, read against CLT ----------------------------------
w("generic_tables_table_1_x_src", sig("clt"))
w("generic_tables_t1_x_axis_n", 2)
w("generic_tables_table_1_y_en", 0)
ax = M.c("generic_tables_t1_x_axis")
L.write_raw(ax["offset"], struct.pack("<ff", 0.0, 100.0))
cell = M.c("generic_tables_table_1")
L.write_raw(cell["offset"], struct.pack("<ff", 0.0, 100.0))

# --- 2. row 21 (LS10): on always, duty from that table, on a pin ----------------------------------------
# "1" as a program: PUSH_CONST 1, END. The gate has to be true for the value to reach the pin.
OP_PUSH_ZERO, OP_PUSH_ONE, OP_END = 7, 8, 0   # ExprIsa.h — "0" / "1", then terminate
on_expr = M.config["outputs"]["output"]["fields"]["on_expr"]
L.write_raw(M.array_offset("outputs", "output", ROW, "on_expr"),
            bytes([OP_PUSH_ONE, OP_END]).ljust(on_expr["size"], b"\x00"))
# AND THE OFF CONDITION, which is a SEPARATE program. Writing only the on side left whatever the tune
# happened to carry as the off side — here a leftover "clt < 86" from an output-wizard template — and
# a gate asked to turn on and off at the same time chatters. out_22 then alternated between the right
# duty and 0, which reads as an unstable table path rather than as two conditions disagreeing. This
# bench owns the slot for the duration, so it has to state BOTH halves of the gate.
off_expr = M.config["outputs"]["output"]["fields"]["off_expr"]
L.write_raw(M.array_offset("outputs", "output", ROW, "off_expr"),
            bytes([OP_PUSH_ZERO, OP_END]).ljust(off_expr["size"], b"\x00"))
wa(ROW, "function", 3)          # Generic
wa(ROW, "kind", 0)                 # PWM
wa(ROW, "active_high", 1)
wa(ROW, "value_source", 1)         # Table
wa(ROW, "duty_table_sel", TID)
wa(ROW, "scale_x1000", 1000)
wa(ROW, "offset_x10", 0)
wa(ROW, "clamp_lo_x10", 0)
wa(ROW, "clamp_hi_x10", 1000)
wa(ROW, "pwm_freq_hz", 250)
wa(ROW, "freq_source", 0)
# NEUTRALISE THE DWELL TIMERS. They are slot state this bench does not own: whatever the tune happens
# to carry stays there, and the rig was holding min_off_ms = 10000 from some earlier configuration. A
# slot inside its minimum-off dwell commands 0 % no matter what its table says — which is the feature
# working, and it read here as "the generic table path is dead". Everything the measurement depends on
# has to be SET, not inherited.
wa(ROW, "min_on_ms", 0)
wa(ROW, "min_off_ms", 0)
wa(ROW, "max_on_ms", 0)
wa(ROW, "rearm_ms", 0)
time.sleep(0.4)

# --- 3. drive the axis channel and read the slot's own output ------------------------------------
# CLT is a real sensor here, so inject through Lua's signalWrite (the bench's usual way in — see
# bench-live-module-validation): the module reads the bus, not the wire.
def read_out():
    time.sleep(0.35)
    t = L.telem_all()
    # RAW off the wire — every channel is stored scaled (out_22 is percent at 0.5, clt is C at 0.1),
    # and comparing a raw 200 against "100 %" is how a passing test reports a doubled duty.
    return t["out_22"] * M.telem["out_22"]["scale"], t["clt"] * M.telem["clt"]["scale"]

L.set_script("""
function onTick()
  signalWrite("clt", CLT_SET)
end
""".replace("CLT_SET", "0"))
time.sleep(0.6)
lo, clt_lo = read_out()

L.set_script("""
function onTick()
  signalWrite("clt", 100)
end
""")
time.sleep(0.6)
hi, clt_hi = read_out()

L.set_script("""
function onTick()
  signalWrite("clt", 50)
end
""")
time.sleep(0.6)
mid, clt_mid = read_out()

print(f"clt {clt_lo:.0f}/{clt_mid:.0f}/{clt_hi:.0f} C -> out_22 {lo:.1f} / {mid:.1f} / {hi:.1f} %")
ok = abs(lo) < 1.0 and abs(mid - 50.0) < 2.0 and abs(hi - 100.0) < 1.0
print("PASS: the slot reads the generic table it names" if ok else "FAIL")

# --- 4. and the carrier out of the pool, on the same slot -----------------------------------------
TID2 = reg.index("generic_tables_table_2")
w("generic_tables_table_2_x_src", sig("clt"))
w("generic_tables_t2_x_axis_n", 2)
w("generic_tables_table_2_y_en", 0)
ax2 = M.c("generic_tables_t2_x_axis"); L.write_raw(ax2["offset"], struct.pack("<ff", 0.0, 100.0))
c2 = M.c("generic_tables_table_2");    L.write_raw(c2["offset"], struct.pack("<ff", 500.0, 500.0))
wa(ROW, "freq_source", 1)
wa(ROW, "freq_table_sel", TID2)
time.sleep(0.5)
fo, fc = read_out()
print(f"carrier from table 2 (500 Hz): clt {fc:.0f} C, out_22 still {fo:.1f} %")

# leave the bench as we found it: slot off, script cleared
wa(ROW, "function", 0)
L.restore_script()
print("row 21 (LS10) disabled again")
sys.exit(0 if ok else 1)
