#!/usr/bin/env python3
"""Live telemetry probe for the lost-trigger watchdog bench test.

Polls the ECU's run-state channels over the Omni framing (via ts_bench.TsLink) and prints a line
whenever rpm / engine_state / sync_level changes — so we can watch the watchdog assert STOPPED when
the DIG1 trigger stimulator stops. Run it, then start/stop the stimulator.

    python3 tools/watchdog_probe.py           # follow changes
    python3 tools/watchdog_probe.py --once     # one snapshot + list channels
"""
import sys, time
from tools.ts_bench import TsLink

RUN = {0: "STOPPED", 1: "CRANKING", 2: "RUNNING"}
SYNC = {0: "NONE", 1: "CRANK", 2: "PHASE"}
WANT = ["rpm", "engine_state", "sync_level", "fuel_cut", "ign_cut", "hw_dig1_freq"]


def snap(link):
    all_t = link.telem_all()
    return {k: all_t.get(k) for k in WANT}, all_t


def main():
    link = TsLink(verbose=False)
    print("signature:", link.hello())
    cur, all_t = snap(link)
    have = [k for k in WANT if k in all_t]
    miss = [k for k in WANT if k not in all_t]
    print("channels present:", have, "| missing:", miss)
    if "--once" in sys.argv:
        print("all telem keys:", sorted(all_t.keys()))
        return
    print("following changes (Ctrl-C to stop)...")
    last = None
    while True:
        cur, _ = snap(link)
        rpm = cur.get("rpm")
        st = RUN.get(int(cur["engine_state"]), cur["engine_state"]) if cur.get("engine_state") is not None else "?"
        sy = SYNC.get(int(cur["sync_level"]), cur["sync_level"]) if cur.get("sync_level") is not None else "?"
        fc = int(cur["fuel_cut"]) if cur.get("fuel_cut") is not None else "?"
        ic = int(cur["ign_cut"]) if cur.get("ign_cut") is not None else "?"
        dig1 = cur.get("hw_dig1_freq")
        # bucket rpm/dig1 so tiny jitter doesn't spam a line every poll
        key = (round(float(rpm) / 25) if rpm is not None else None, st, sy, fc, ic,
               round(float(dig1) / 5) if dig1 is not None else None)
        if key != last:
            print(f"[{time.strftime('%H:%M:%S')}] rpm={rpm:>6}  state={st:<8}  sync={sy:<5}  "
                  f"fuel_cut={fc}  ign_cut={ic}  dig1_freq={dig1}")
            last = key
        time.sleep(0.05)


if __name__ == "__main__":
    main()
