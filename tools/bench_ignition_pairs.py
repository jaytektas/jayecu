#!/usr/bin/env python3
"""Which coil actually fires, and where — wasted-spark coils checked at the pin.

The studio writes the coil rows: one per companion pair, naming the pair's LOWER cylinder, in cylinder
order (tools/output_rows.py mirrors it). The firmware fires each row's coil for that cylinder AND its
companion, which it finds from the firing order at run time. Getting a pair wrong is not cosmetic — a
wasted spark on a non-companion lands mid-compression on a charged cylinder.

So this does not trust the arithmetic. It spins a real wheel, captures one delivered engine cycle, and
reads the coil lanes back: every spark that came out of a pin, at the angle it came out at. Then it
changes the FIRING ORDER WITHOUT TOUCHING THE ROWS and captures again: the rows name the same cylinders,
and every coil must now pair its cylinder with the companion the NEW order gives it.

A crank-only wheel reaches CRANK sync and no further, which is correct — wasted spark is the
arrangement that does not need phase. Both cylinders of a pair sit at the same crank angle, so each
coil fires ONCE per revolution and its two sparks are recorded at the SAME angle. That is the answer,
not a fold error.

  python3 -m tools.bench_ignition_pairs                                  # 2JZ 1-5-3-6-2-4, then 1-4-2-5-3-6
  python3 -m tools.bench_ignition_pairs --order 1,8,4,3,6,5,7,2 --then 1,5,4,8,6,3,7,2   # a V8
  python3 -m tools.bench_ignition_pairs --rpm 2400
"""
import argparse, sys, time
from tools.ts_bench import TsLink
from tools.gen_rebench import open_stim, select, configure, WHEELS
from tools.bench_fuel import set_fixed_rpm, wait_for_sync, lua_script
from tools.output_rows import write_engine_rows, layout, SEMI_SEQUENTIAL, FN_IGNITION, IGN_BASE

WHEEL_IDX = 6                       # '36-1' — dense, single crank stream, locks quickly
CYCLE = 720.0


def write_order(l, order):
    for i in range(12):
        l.write_raw(l.meta.array_offset("engine", "firing_order", i, "cyl"),
                    bytes([order[i] if i < len(order) else 0]))


def setup(l, ORDER, N):
    l.set_config("engine_cylinder_count", N)
    l.set_config("engine_cycle_type", 1)          # four-stroke, 720 deg
    l.set_config("engine_odd_fire", 0)
    l.set_config("engine_num_inj_stages", 1)
    l.write_raw(l.meta.array_offset("engine", "inj_stage", 0, "mode"), bytes([SEMI_SEQUENTIAL]))
    write_order(l, ORDER)
    # WASTED SPARK AS ROWS: one coil row per companion pair, naming the pair's lower cylinder; the ECU
    # fires the companion from the firing order. Injector rows too, so every cylinder has fuel.
    write_engine_rows(l, ORDER[:N], coils="wasted", stages=[(SEMI_SEQUENTIAL, None)])
    l.execute("reconfig")


def capture_and_check(l, ORDER, rows, rpm, check):
    """Capture one cycle and check every coil row fires its cylinder and that cylinder's companion under
    ORDER, sparking BTDC of them."""
    N = len(ORDER)
    pos = {cyl: i for i, cyl in enumerate(ORDER)}
    tdc = {cyl: pos[cyl] * CYCLE / N for cyl in ORDER}                 # even-fire
    companion = {cyl: ORDER[(pos[cyl] + N // 2) % N] for cyl in ORDER}
    pairs = {r - IGN_BASE: sorted({row["cylinder"], companion[row["cylinder"]]})
             for r, row in rows.items() if row["function"] == FN_IGNITION}
    print(f"  firing {'-'.join(map(str, ORDER))}: " +
          "   ".join(f"IGN{k+1} = cyl {v[0]} -> fires {'&'.join(map(str, v))}" for k, v in sorted(pairs.items())))
    for k, (a, b) in sorted(pairs.items()):
        check(f"IGN{k+1} pairs cyl{a} & cyl{b} 360 deg apart", abs(abs(tdc[a] - tdc[b]) - 360.0) < 1e-6,
              f"{abs(tdc[a] - tdc[b]):.0f} deg")

    time.sleep(2.5)   # the scheduler needs a couple of cycles to commit before a capture shows coils
    cap = l.cycle_capture(timeout=5.0)
    print(f"  capture: state={cap['state']} span={cap['cycle_angle']:.0f} edges={cap['total']} "
          f"rpm={cap['rpm']:.0f}")
    sparks = {}
    for ed in cap["edges"]:
        if ed["signal"] == "Coil" and not ed["active"]:   # de-energise = the spark
            sparks.setdefault(ed["channel"], []).append(ed["angle"])
    for k in sparks: sparks[k].sort()
    print("  sparks:   " + "   ".join(f"IGN{k+1} @ {[f'{x:.0f}' for x in v]}" for k, v in sorted(sparks.items())))

    check(f"{N // 2} coils fired", sorted(sparks) == sorted(pairs), f"channels {sorted(k + 1 for k in sparks)}")
    check("crank sync (a crank-only wheel reaches no more)", cap["sync_level"] == 1,
          f"sync_level={cap['sync_level']}")
    for k, angs in sorted(sparks.items()):
        cyls = pairs.get(k, [])
        check(f"IGN{k+1} fired once per revolution", len(angs) == 2, f"{len(angs)} spark(s)/cycle")
        if len(angs) != 2 or not cyls: continue
        check(f"IGN{k+1}'s pair share one crank angle", abs(angs[1] - angs[0]) < 1.0,
              f"{angs[0]:.0f} and {angs[1]:.0f}")
        folded = {tdc[c] % 360.0 for c in cyls}
        check(f"IGN{k+1}'s cylinders {'&'.join(map(str, cyls))} fold to one crank angle",
              len(folded) == 1, f"{sorted(folded)}")
        want = folded.pop()
        adv = (want - angs[0]) % 360.0
        check(f"IGN{k+1} sparks BTDC of cyl {'&'.join(map(str, cyls))}", 0 < adv < 60,
              f"{adv:.1f} deg BTDC of {want:.0f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--order", default="1,5,3,6,2,4",
                    help="firing order the rows are laid out for (default: a 2JZ)")
    ap.add_argument("--then", default="1,4,2,5,3,6",
                    help="a second firing order, applied WITHOUT rewriting the rows")
    ap.add_argument("--rpm", type=int, default=1200)
    a = ap.parse_args()
    ORDER = [int(x) for x in a.order.split(",")]
    THEN = [int(x) for x in a.then.split(",")] if a.then else []
    N, rpm = len(ORDER), a.rpm
    if N % 2 or (THEN and sorted(THEN) != sorted(ORDER)):
        print("wasted spark needs an even cylinder count, and --then the same cylinders"); return 1
    spec = WHEELS[WHEEL_IDX]
    fails = []

    def check(name, cond, detail):
        print(f"  [{'PASS' if cond else 'FAIL'}] {name}: {detail}")
        if not cond: fails.append(name)

    rows = layout(ORDER, "wasted", [(SEMI_SEQUENTIAL, None)])
    print(f"=== {N}-cyl wasted spark @ {rpm} rpm ===")
    sp = open_stim()
    l = TsLink(); configure(l, spec); setup(l, ORDER, N); l.close()
    select(sp, WHEEL_IDX, reps=4)
    set_fixed_rpm(sp, rpm)

    l = TsLink()
    if not wait_for_sync(l):
        print("  [FAIL] never synced"); l.close(); sp.close(); sys.exit(1)
    l.execute("key on")
    l.set_script(lua_script(tps=8.0))
    capture_and_check(l, ORDER, rows, rpm, check)

    if THEN:
        # THE FIRING ORDER DOES NOT MOVE OUTPUTS. Same rows, new order: the ECU must re-pair the coils.
        # A firing change applies on a reconfigure, so it is forced here with the engine turning.
        print(f"\n--- firing order -> {'-'.join(map(str, THEN))}, rows untouched ---")
        write_order(l, THEN)
        l.execute("reconfig")
        l.close()
        l = TsLink()
        if not wait_for_sync(l):
            check("re-synced after the firing-order change", False, "never synced")
        else:
            l.execute("key on")
            capture_and_check(l, THEN, rows, rpm, check)

    l.restore_script(); l.execute("key auto"); l.close(); sp.close()
    print(f"\n=== {'FAILED: ' + ', '.join(fails) if fails else 'ALL PASS'} ===")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
