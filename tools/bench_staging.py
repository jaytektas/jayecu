#!/usr/bin/env python3
"""Bench rig for the per-stage INJECTION STAGING refactor — validate it on a SPINNING wheel.

The host test (tests/test_injection_modes.cpp) proves build_inj_events() flattens to one event per
(stage, group) and groups by cyl[].bank. What it cannot prove is that the REAL scheduler, on a real
decoder at a real rpm, DELIVERS those injector edges where the event says. This rig does.

This bench has only the crank wire (dig3/cam is unwired — every '+cam' registry wheel stalls at
CRANK), so it runs on the 36-1 crank wheel at CRANK sync. That folds the 720 deg cycle onto 360, so
absolute angles are ambiguous — but the GROUPING (which injector outputs share one open event) is
not, and grouping is the whole of the staging behaviour. Every check below is on grouping, and the
banks are chosen {1,1,2,2} so the two banks' earliest TDCs (0 and 180) do NOT fold together.

  SEQUENTIAL   : one event per cylinder, on its own output, at its own TDC  -> grouped by TDC
  BANK {1,1,2,2}: outputs grouped by cyl[].bank, each bank fired together   -> grouped by BANK
  BANK {1,1,1,1}: one bank = the whole engine                               -> one group
  MULTI_POINT  : every output together off the earliest TDC                 -> one group

The two headline proofs:
  * BANK grouping FOLLOWS cyl[].bank, not the natural TDC pairing (SEQUENTIAL and BANK, same engine,
    give DIFFERENT injector groups).
  * each STAGE honours its OWN mode: a second stage in BANK mode groups by bank even while the
    primary stage in SEQUENTIAL mode groups per-cylinder, in the SAME captured cycle. Under the old
    code the scheduler honoured only stage 0's mode, so stage 1 would have grouped per-cylinder too.

Usage:  python3 -m tools.bench_staging [rpm] [seq|bank|bank1|mp|stage|all]   (default: all)
"""
import struct, sys, time

from tools.ts_bench import TsLink
from tools.gen_rebench import open_stim, select, configure, WHEELS
from tools.bench_fuel import set_fixed_rpm, wait_for_sync, lua_script
from tools.output_rows import write_engine_rows

WHEEL_IDX = 6            # '36-1' — dense crank stream, locks quickly (CRANK sync, folds to 360)
DEFAULT_RPM = 1200

# 4-cylinder four-stroke firing 1-3-4-2 (cylinder index -> firing position, 1-based). The injectors are
# OUTPUT ROWS laid out as the wizard does (tools/output_rows.py): each stage a block from LS1, a
# per-cylinder stage LS(base+j) -> cylinder j, a grouped stage split between the banks. banks {1,1,2,2}:
# cyl0,1 -> bank1; cyl2,3 -> bank2. Their earliest TDCs are 0 and 180, which stay distinct under the
# 360 fold.
FIRE_POS = [1, 4, 2, 3]
N = len(FIRE_POS)
BANKS = [1, 1, 2, 2]

SEQUENTIAL, SEMI_SEQUENTIAL, MULTI_POINT, BANK = 0, 1, 2, 3

fails = []
def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}: {detail}")
    if not cond:
        fails.append(name)


def a(l, arr, i, f):
    return l.meta.array_offset("engine", arr, i, f)


_banks = list(BANKS)      # what setup()/set_banks() last wrote, for the row layout


def order_by_position():
    order = [0] * N
    for cyl0, pos in enumerate(FIRE_POS):
        order[pos - 1] = cyl0 + 1
    return order


def set_banks(l: TsLink, banks):
    global _banks
    _banks = list(banks)
    for c, b in enumerate(banks):
        l.write_raw(a(l, "cyl", c, "bank"), bytes([b]))


def setup(l: TsLink, nstages=1, banks=BANKS):
    """4 cylinders, wasted spark, firing 1-3-4-2, `nstages` injection stages (rows written by set_modes)."""
    l.set_config("engine_cylinder_count", N)
    l.set_config("engine_num_inj_stages", nstages)
    for st in range(nstages):
        l.write_raw(a(l, "inj_stage", st, "injections_per_cycle"), bytes([1]))
    firing_order = [0] * 12
    for cyl0, pos in enumerate(FIRE_POS):
        firing_order[pos - 1] = cyl0 + 1
    for i, cyl in enumerate(firing_order):
        l.write_raw(a(l, "firing_order", i, "cyl"), bytes([cyl]))
    set_banks(l, banks)
    # Rows for a single sequential stage, so the map is valid (wasted-spark coils, CRANK sync) from the
    # start; set_modes() re-lays them for whatever it asks for.
    write_engine_rows(l, order_by_position(), coils="wasted", stages=[(SEQUENTIAL, None)] * nstages, banks=banks)


def set_modes(l: TsLink, *modes, grouped=N):
    """Set each stage's mode AND lay its injector rows for it: a grouped stage shares `grouped`
    injector rows between the banks, a per-cylinder stage has one per cylinder."""
    for st, m in enumerate(modes):
        l.write_raw(a(l, "inj_stage", st, "mode"), bytes([m]))
    write_engine_rows(l, order_by_position(), coils="wasted",
                      stages=[(m, grouped) for m in modes], banks=_banks)
    l.execute("reconfig")
    time.sleep(1.0)
    l.cycle_capture(timeout=6.0)          # throwaway: let the fresh config own a whole cycle first


def force_staging(l):
    """Drop the PRIMARY stage's Staging Duty Cycle cap to ~2% so it saturates immediately and the
    leftover charge spills into the secondary stage — otherwise the secondary carries 0 fuel and
    never opens. stage1_staging_duty_table is 1x1, U16, 0.1%/count (default 500 = 50%)."""
    # At 1200 rpm the 100%-duty squirt spans the whole ~100 ms cycle, so the cap must be tiny for
    # idle fuel to exceed it and spill into the secondary: 0.2% caps the primary at ~200 us.
    off = l.meta.c("fuel_calculator_stage1_staging_duty_table")["offset"]
    l.write_raw(off, struct.pack("<H", 2))             # 0.2%
    l.execute("reconfig"); time.sleep(0.3)


def inj_open_groups(cap, tol=8.0):
    """Group delivered injector OPEN edges by angle. Returns [(angle, sorted[channels])] — one entry
    per distinct open cluster. Outputs that share an event open at the same angle, so the group
    structure is the mode's fingerprint. De-dups a channel repeated inside one cluster."""
    opens = sorted((e["angle"], e["channel"]) for e in cap["edges"]
                   if e["signal"] == "Injector" and e["active"])
    groups = []
    for ang, ch in opens:
        for g in groups:
            if min(abs(ang - g["a"]), cap["cycle_angle"] - abs(ang - g["a"])) <= tol:
                g["ch"].add(ch); g["angs"].append(ang); break
        else:
            groups.append({"a": ang, "ch": {ch}, "angs": [ang]})
    return [(round(sum(g["angs"]) / len(g["angs"]), 1), sorted(g["ch"])) for g in groups]


def grab(l, tag):
    cap = l.cycle_capture(timeout=6.0)
    groups = inj_open_groups(cap)
    inj = sorted({e["channel"] for e in cap["edges"] if e["signal"] == "Injector"})
    print(f"  [{tag}] span={cap['cycle_angle']:.0f} sync={cap['sync_level']} rpm={cap['rpm']:.0f} "
          f"inj_chans={inj}")
    for ang, chs in groups:
        print(f"       open @ {ang:6.1f} : injectors {chs}")
    return groups


def groupsets(groups):
    return sorted(tuple(chs) for _, chs in groups)


def main():
    rpm = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else DEFAULT_RPM
    which = next((x for x in sys.argv[1:] if not x.isdigit()), "all")
    spec = WHEELS[WHEEL_IDX]
    print(f"=== Bench injection staging — {spec[0]} @ {rpm} rpm — {which} ===")

    sp = open_stim()
    l = TsLink(); configure(l, spec); setup(l, nstages=1); l.close()
    select(sp, WHEEL_IDX, reps=4); set_fixed_rpm(sp, rpm)

    l = TsLink()
    if not wait_for_sync(l):
        print("  [FAIL] sync: decoder never reached CRANK sync — stim wired to the crank input?")
        l.close(); sp.close(); sys.exit(1)
    l.execute("key on"); l.set_script(lua_script(tps=8.0)); time.sleep(1.5)

    seq_groups = None
    if which in ("seq", "bank", "all"):
        set_modes(l, SEQUENTIAL)
        seq_groups = grab(l, "SEQUENTIAL")
        check("SEQUENTIAL: every injector opens exactly once",
              sorted(c for _, chs in seq_groups for c in chs) == [0, 1, 2, 3],
              str(seq_groups))

    if which in ("bank", "all"):
        set_modes(l, BANK)
        g = grab(l, "BANK {1,1,2,2}")
        check("BANK: grouped by cyl[].bank -> {0,1} and {2,3}",
              groupsets(g) == [(0, 1), (2, 3)], str(groupsets(g)))
        if seq_groups is not None:
            check("BANK grouping DIFFERS from the per-cylinder (SEQUENTIAL) grouping",
                  groupsets(g) != groupsets(seq_groups),
                  f"bank {groupsets(g)} vs seq {groupsets(seq_groups)}")

    if which in ("bank1", "all"):
        set_banks(l, [1] * N)                                    # one bank
        set_modes(l, BANK)
        g = grab(l, "BANK {1,1,1,1}")
        check("BANK single-bank collapses to one group of all four",
              groupsets(g) == [(0, 1, 2, 3)], str(groupsets(g)))
        set_banks(l, BANKS)                                      # restore two banks

    if which in ("mp", "all"):
        set_modes(l, MULTI_POINT)
        g = grab(l, "MULTI_POINT")
        check("MULTI_POINT: every output together in one group",
              groupsets(g) == [(0, 1, 2, 3)], str(groupsets(g)))

    if which in ("fold", "all"):
        # The rows ARE the injectors: a multi-point stage with two injector rows drives exactly those two
        # pins (a throttle body with fewer injectors than cylinders), and a sequential stage with a row
        # per cylinder drives all four.
        set_modes(l, MULTI_POINT, grouped=2)
        g = grab(l, "MULTI_POINT, 2 injector rows")
        chans = sorted(c for _, chs in g for c in chs)
        check("a multi-point stage of two injector rows drives exactly LS1,LS2",
              chans == [0, 1], str(chans))
        set_modes(l, SEQUENTIAL)
        g = grab(l, "SEQUENTIAL, 4 injector rows")
        chans = sorted(c for _, chs in g for c in chs)
        check("a sequential stage with a row per cylinder drives all four",
              chans == [0, 1, 2, 3], str(chans))

    if which in ("stage", "all"):
        # THE headline: two stages, DIFFERENT modes. stage0 SEQUENTIAL (per-cyl, LS0..3), stage1
        # BANK (per-bank, LS4..7). Under the old scheduler stage1 inherited stage0's mode and would
        # group per-cylinder ({4,7}/{5,6}); the fix makes it group by bank ({4,5}/{6,7}).
        setup(l, nstages=2)
        force_staging(l)
        set_modes(l, SEQUENTIAL, BANK)
        g = grab(l, "STAGE0=SEQ  STAGE1=BANK")
        s0 = groupsets([(ang, [c for c in chs if c < 4]) for ang, chs in g if any(c < 4 for c in chs)])
        s1 = groupsets([(ang, [c for c in chs if c >= 4]) for ang, chs in g if any(c >= 4 for c in chs)])
        check("both stages delivered (8 outputs, LS0..7)",
              sorted(c for _, chs in g for c in chs) == [0, 1, 2, 3, 4, 5, 6, 7], str(g))
        check("stage 0 (SEQUENTIAL) groups per-cylinder {0,3}/{1,2}",
              s0 == [(0, 3), (1, 2)], str(s0))
        check("stage 1 (BANK) groups by bank {4,5}/{6,7}, NOT stage-0's per-cyl {4,7}/{5,6}",
              s1 == [(4, 5), (6, 7)], str(s1))
        setup(l, nstages=1)     # restore single stage
        l.execute("reconfig")

    l.restore_script(); l.execute("key auto"); l.close(); sp.close()
    print(f"\n=== {'ALL PASS' if not fails else 'FAILED: ' + ', '.join(fails)} ===")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
