#!/usr/bin/env python3
"""Park the ETB for the duration of a bench run, and put it back afterwards.

WHY A SWEEP NEEDS THIS. Nothing in the suite set tests the throttle, but two things drive it anyway:

  1. Twelve suites issue `key on` / `key auto`, and a key-on edge arms the ETB's verify sweep
     (adopt_stored_cal -> cal_pending_ -> start_autocal(verify=true)), which takes the plate to BOTH
     stops. That is a full travel per key cycle, a dozen times a run, on a real throttle body — and
     each sweep writes relax_pct and the sensor cal back into g_config, so it churns the tune too.
  2. Since the engine-running interlock was removed (correctly — it was killing the throttle in the
     one state a drive-by-wire throttle exists for), the ETB servos continuously whenever the stim
     claims the engine is running. Measured at rest: the bridge energised at -16.6 % duty holding the
     plate at 3 %, against its own return spring, for the whole session.

Neither is a fault. Both are the throttle doing its job for a bench that never asked it to.

RAM ONLY, never burnt: the disable lasts until the next reset or restore, so a run that dies half way
leaves nothing permanent behind — reset the ECU and the tune's own setting is back.

  python3 tools/bench_etb_park.py off   -> disable every ETB slot, print what it saved
  python3 tools/bench_etb_park.py on    -> restore what was saved
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from tools.ts_bench import TsLink

STATE = "/tmp/claude-1000/bench_etb_park.state"


def slots(meta):
    a = meta.config["electronic_throttle"]["etb"]
    d = a["fields"]["enabled"]
    return [(a["base_offset"] + i * a["stride"] + d["rel_offset"], d["size"])
            for i in range(a["count"])]


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "off"
    L = TsLink(verbose=False)
    marks = slots(L.meta)

    if what == "off":
        saved = [L.read_config_chunked(off, n, chunk=8)[0] for off, n in marks]
        os.makedirs(os.path.dirname(STATE), exist_ok=True)
        with open(STATE, "w") as f:
            f.write(",".join(str(v) for v in saved))
        for off, _ in marks:
            L.write_raw(off, b"\x00")
        print(f"  ETB parked for the run (was {saved}) — RAM only, a reset undoes it")
        return 0

    if not os.path.exists(STATE):
        print("  nothing saved; leaving the ETB as it is")
        return 0
    saved = [int(x) for x in open(STATE).read().split(",") if x != ""]
    for (off, _), v in zip(marks, saved):
        L.write_raw(off, bytes([v]))
    os.remove(STATE)
    print(f"  ETB restored to {saved}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
