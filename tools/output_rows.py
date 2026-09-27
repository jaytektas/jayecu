"""The OUTPUT ROWS a bench engine needs — coils and injectors as rows.

outputs.output[i] IS pin i: IGN1-12 are rows 0-11, LS1-22 rows 12-33, HS1-8 rows 34-41. A coil or
injector is a row whose function is Ignition / Injector and whose `cylinder` names ONE cylinder value:
1-12 a cylinder (a rotor on a rotary), 13 All, 14 Bank 1, 15 Bank 2. The ECU allocates nothing — the
studio writes these rows, and this lays them out the way the studio does (and tests/output_rows_helper.h
does for the host tests):

  coils      'cop'         IGN(c) -> cylinder c                        engine.ign_mode = Coil-on-Plug
             'wasted'      one coil per companion pair, naming the pair's lower cylinder, in cylinder
                           order; the ECU fires the companion from the firing order
                                                                        engine.ign_mode = Wasted Spark
             'distributor' IGN1   -> All                                engine.ign_mode = Distributor
  injectors  one contiguous LS block per stage from LS1; a per-cylinder mode LS(base+c) -> cylinder c;
             Bank splits its injectors between the banks present; Multi-Point -> All.

    from tools.output_rows import write_engine_rows
    write_engine_rows(l, firing_order=[1, 3, 4, 2], coils="wasted", stages=[(1, None)])
"""
import struct

FN_NONE, FN_IGNITION, FN_INJECTOR, FN_GENERIC = 0, 1, 2, 3
CYL_ALL, CYL_BANK1, CYL_BANK2 = 13, 14, 15
IGN_BASE, LS_BASE = 0, 12
N_IGN, N_LS = 12, 22
IGN_MODE = {"distributor": 0, "wasted": 1, "cop": 2}

SEQUENTIAL, SEMI_SEQUENTIAL, MULTI_POINT, BANK, SEQUENTIAL_ANY_SYNC = 0, 1, 2, 3, 4
_PER_CYL = (SEQUENTIAL, SEMI_SEQUENTIAL, SEQUENTIAL_ANY_SYNC)


def layout(firing_order, coils="wasted", stages=((SEQUENTIAL, None),), banks=None):
    """rows {row index: {function, cylinder, inj_stage, ign_plug}} for an even-fire four-stroke engine.

    firing_order: cylinders (1-based) in firing order. stages: [(mode, grouped_outputs)] per active
    stage — grouped_outputs is read only for Bank / Multi-Point. banks: per-cylinder bank (default 1).
    """
    n = len(firing_order)
    banks = list(banks) if banks else [1] * n
    rows = {}

    def put(row, fn, cyl, stage=0, plug=0):
        rows[row] = {"function": fn, "cylinder": cyl, "inj_stage": stage, "ign_plug": plug}

    if coils == "cop":
        for c in range(n):
            put(IGN_BASE + c, FN_IGNITION, c + 1)
    elif coils == "wasted":
        # Companion = the cylinder n/2 firing positions later (half the cycle on an even-fire engine).
        pos = {cyl: i for i, cyl in enumerate(firing_order)}
        done, k = set(), 0
        for cyl in range(1, n + 1):
            if cyl in done:
                continue
            done.add(cyl)
            done.add(firing_order[(pos[cyl] + n // 2) % n])
            put(IGN_BASE + k, FN_IGNITION, cyl)
            k += 1
    elif coils == "distributor":
        put(IGN_BASE, FN_IGNITION, CYL_ALL)
    else:
        raise ValueError(coils)

    present = sorted(set(banks))[:2]
    base = 0
    for s, (mode, grouped) in enumerate(stages):
        outs = n if mode in _PER_CYL else (grouped or n)
        for i in range(outs):
            k = base + i
            if k >= N_LS:
                break
            if mode in _PER_CYL:
                v = i + 1
            elif mode == BANK:
                v = CYL_BANK1 - 1 + present[(i * len(present)) // outs]
            else:
                v = CYL_ALL
            put(LS_BASE + k, FN_INJECTOR, v, stage=s)
        base += outs
    return rows


def write_rows(l, rows, coils=None, active_high=1):
    """Write `rows` (and engine.ign_mode for `coils`) and clear every OTHER coil/injector row (a row left
    over from an earlier layout would still be bound). Generic rows are left alone."""
    M = l.meta
    arr = M.array("outputs", "output")
    F = arr["fields"]

    def off(i, f):
        return M.array_offset("outputs", "output", i, f)

    def put(i, f, v):
        l.write_raw(off(i, f), struct.pack(M.fmt(F[f]["datatype"]), v))

    if coils is not None:
        l.set_config("engine_ign_mode", IGN_MODE[coils])
    for i in range(IGN_BASE, LS_BASE + N_LS):
        if i in rows:
            continue
        cur = l.read_config_raw(off(i, "function"), 1)[0]
        if cur in (FN_IGNITION, FN_INJECTOR):
            put(i, "function", FN_NONE)
    for i, r in sorted(rows.items()):
        put(i, "cylinder", r["cylinder"])
        put(i, "inj_stage", r["inj_stage"])
        put(i, "ign_plug", r["ign_plug"])
        put(i, "active_high", active_high)
        put(i, "function", r["function"])


def write_engine_rows(l, firing_order, coils="wasted", stages=((SEQUENTIAL, None),), banks=None):
    rows = layout(firing_order, coils, stages, banks)
    write_rows(l, rows, coils=coils)
    return rows
