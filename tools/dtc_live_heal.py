#!/usr/bin/env python3
"""Live raise->heal proof: move batt_min_mv below the actual battery reading and
watch P0562 + its category bit clear, then restore and watch it re-raise. RAM-only
config writes (no burn), so the change is transient."""
import sys, time, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from ts_bench import TsLink

L = TsLink()
def snap():
    t = L.telem_all()
    return int(t["dtc_active"]), int(t["dtc_indicators"]), int(t["dtc_worst_code"])

vbat = L.telem("battery_mv")
thr  = L.get_config("engine_protection_batt_min_mv")
print(f"battery_mv={vbat}  batt_min_mv(threshold)={thr}")
a, ind, code = snap()
print(f"start:           active={a} indicators=0x{ind:08X} worst=P{code:04X}  (bit14={'set' if ind&(1<<14) else 'clear'})")

# 1) Drop the threshold well below the actual reading -> battery-low condition clears -> heal.
L.set_config("engine_protection_batt_min_mv", max(0, int(vbat) - 2000))
time.sleep(0.3)
a, ind, code = snap()
print(f"thr<vbat:        active={a} indicators=0x{ind:08X} worst=P{code:04X}  (bit14={'set' if ind&(1<<14) else 'clear'})  <- expect healed/clear")

# 2) Raise the threshold above the reading again -> condition true -> re-raise.
L.set_config("engine_protection_batt_min_mv", int(vbat) + 2000)
time.sleep(0.3)
a, ind, code = snap()
print(f"thr>vbat:        active={a} indicators=0x{ind:08X} worst=P{code:04X}  (bit14={'set' if ind&(1<<14) else 'clear'})  <- expect re-raised")

# Restore the original threshold (still RAM-only; a power cycle reloads the flash tune).
L.set_config("engine_protection_batt_min_mv", int(thr))
time.sleep(0.3)
a, ind, code = snap()
print(f"restored thr:    active={a} indicators=0x{ind:08X} worst=P{code:04X}")
L.close()
