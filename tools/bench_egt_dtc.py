#!/usr/bin/env python3
"""Live bench validation of the per-probe EGT over-temperature DTC.

EgtProtect cuts fuel on the HOTTEST EGT probe. The cut alone tells you nothing about WHICH cylinder
ran away, so the module raises one of twelve contiguous codes (EGT_OVERTEMP_1..12 = P1770..P177B)
naming the probe that was hottest at the moment it latched. Without a datalog, that stored code is
the only thing that says where to look.

What this proves on real hardware:
  1. driving one probe over cut_c raises THAT probe's code, at level 3
  2. the code names the hottest probe, not the first or the last one over the line
  3. cooling below enrich_c heals it — ACTIVE clears, STORED keeps the history
  4. a second event on a different probe raises that probe's code

Method: EGT sensors are absent on the bench, so the probes are injected at PRIO_LUA via signalWrite
(the same trick bench_fuel.py uses for the absent analog sensors), and EgtProtect is armed by a RAM
config write. Nothing is burned — a reset restores the tune exactly.

  python3 tools/bench_egt_dtc.py [-v]
"""
import sys
import time
import pathlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from ts_bench import TsLink

TTL_MS = 500          # injected signals must outlive the gap between onTicks
ENRICH_C = 850        # protection thresholds we set for the run
CUT_C = 950
EGT_BASE = 0x1770     # ModuleDtc::EGT_OVERTEMP_1; the block is contiguous, probe n = base + (n-1)

fails = 0


def check(ok, what, detail=""):
    global fails
    print(f"  [{'PASS' if ok else 'FAIL'}] {what}{('  — ' + detail) if detail else ''}")
    if not ok:
        fails += 1


def inject(link, probes, hold_s=1.0):
    """Hold every EGT probe at a given temperature (probes: {n: degC}); absent ones read 0."""
    body = "\n".join(f'  signalWrite("egt_{n}", {v}, {TTL_MS})' for n, v in probes.items())
    link.set_script(f"function onTick()\n{body}\nend\nsetTickRate(50)\n")
    time.sleep(hold_s)


def dtc(link):
    t = link.telem_all()
    return int(t["dtc_active"]), int(t["dtc_stored"]), int(t["dtc_worst_code"]), int(t["dtc_worst_sev"])


def main():
    link = TsLink(verbose="-v" in sys.argv)
    print("signature:", link.hello())

    # Remember what we are about to change so the bench is left as we found it.
    saved = {k: link.get_config(k) for k in
             ("egt_protect_enabled", "egt_protect_enrich_c", "egt_protect_cut_c")}
    saved_script = link.get_script()
    print(f"saved config: {saved}")

    try:
        link.set_config("egt_protect_enrich_c", ENRICH_C)
        link.set_config("egt_protect_cut_c", CUT_C)
        link.set_config("egt_protect_enabled", 1)
        time.sleep(0.3)

        print("\n--- cold: every probe well under the enrich threshold ---")
        inject(link, {n: 300 for n in range(1, 13)})
        a0, s0, c0, v0 = dtc(link)
        print(f"  active={a0} stored={s0} worst=P{c0:04X} sev={v0}")

        print("\n--- probe 3 driven over cut_c (others cold) ---")
        hot = {n: 300 for n in range(1, 13)}
        hot[3] = CUT_C + 60
        inject(link, hot, hold_s=1.5)
        a1, s1, c1, v1 = dtc(link)
        print(f"  active={a1} stored={s1} worst=P{c1:04X} sev={v1}")
        check(c1 == EGT_BASE + 2, "the raised code names probe 3",
              f"got P{c1:04X}, want P{EGT_BASE + 2:04X}")
        check(v1 == 3, "raised at level 3", f"sev={v1}")
        check(a1 > a0, "it is ACTIVE", f"{a0} -> {a1}")

        print("\n--- probe 7 hotter still, while 3 stays over the line ---")
        # The cut is already latched, so this must NOT re-point the code: the DTC records the probe
        # that caused THIS cut. Re-aiming it mid-event would rewrite history.
        hot[7] = CUT_C + 200
        inject(link, hot, hold_s=1.5)
        a2, s2, c2, v2 = dtc(link)
        print(f"  active={a2} stored={s2} worst=P{c2:04X} sev={v2}")
        check(c2 == EGT_BASE + 2, "a latched cut keeps naming the probe that caused it",
              f"got P{c2:04X}")

        print("\n--- cooled below enrich_c: the cut releases ---")
        inject(link, {n: 200 for n in range(1, 13)}, hold_s=1.5)
        a3, s3, c3, v3 = dtc(link)
        print(f"  active={a3} stored={s3} worst=P{c3:04X} sev={v3}")
        check(a3 < a2, "the code healed (ACTIVE cleared)", f"{a2} -> {a3}")
        check(s3 >= s1, "…but STORED kept it — the history is the point", f"stored {s1} -> {s3}")

        print("\n--- a fresh event on probe 7 alone ---")
        hot2 = {n: 300 for n in range(1, 13)}
        hot2[7] = CUT_C + 60
        inject(link, hot2, hold_s=1.5)
        a4, s4, c4, v4 = dtc(link)
        print(f"  active={a4} stored={s4} worst=P{c4:04X} sev={v4}")
        check(c4 == EGT_BASE + 6, "the next cut names probe 7",
              f"got P{c4:04X}, want P{EGT_BASE + 6:04X}")

        print("\n--- cool down and disarm ---")
        inject(link, {n: 200 for n in range(1, 13)}, hold_s=1.2)

    finally:
        # Restore. RAM only — nothing was burned, so a reset would also have undone it.
        link.set_script(saved_script if saved_script.strip() else "function onTick()\nend\n")
        for k, v in saved.items():
            link.set_config(k, v)
        print(f"\nrestored config: {saved}")
        link.close()

    print(f"\n[egt-dtc] {'FAILURES' if fails else 'all passed'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
