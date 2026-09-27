#!/usr/bin/env python3
"""Turn the bench stim's pattern tables into trigger_wheels entries for the schema.

The geometry is DERIVED, never typed: extract_stim_wheels reads the same angular sample tables the
rig will spin, so every number here can be checked by measurement afterwards. A tooth count copied
off a page cannot be.

Two conversions matter:

  * Our gap indices name the tooth the gap sits BEFORE, and the first must be 0 — the reference is
    the first tooth after sync (see trigger_offset_btdc). The extractor reports the SPACING index
    that is wide, so index i there means our tooth (i+1) mod present, and the whole list is then
    rotated so the smallest becomes 0. Rotating changes which tooth is called zero, which is exactly
    what makes it the reference.
  * A cam is described by the angles at which its edges fall over the 720 cycle, as a SEQUENCE of
    inter-edge spans in 0.1 deg — the same encoding the decoder matches ratios against.

    python3 tools/gen_library_wheels.py            # print YAML for the selected wheels
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract_stim_wheels import load_arrays, load_table, edges, describe_gap_wheel   # noqa: E402

# Stim index -> the name this wheel gets in our library. Engine identification only.
#
# Only wheels that VERIFY on the rig are here. Two were derived, measured and then dropped rather
# than shipped on the strength of the derivation alone:
#
#   29  NGC 36+2-2 + 4cyl cam — the crank decodes exactly (32 teeth) and the cam signal arrives, but
#       the stream never resolves to PHASE at 900 or 1500 rpm. Its 7-span cam pattern contains a
#       repeated sub-pattern (720,720,1800 twice), which is the kind of thing a ratio matcher can
#       take a long time to disambiguate. Needs understanding, not a longer timeout.
#   37  GM 4 even + cam — recorded 16 crank edges per cycle against the 4 the extracted table
#       predicts. Either the table parse is wrong for that entry or the decoder is counting a
#       different channel; either way the number does not agree with itself yet.
#
# A wheel nobody has measured is worse than a wheel nobody has.
# NOT in the sweep: "Daihatsu 3+1 distributor". The harness cannot judge it — expected_crank_edges
# trusts the stim registry's span, which for that wheel says 360 where its array says 720, and the
# lane accounting assumes a crank stream where a distributor has none. It is proven instead by
# tools/bench_distributor.py, which checks the one thing that matters: PHASE on the true geometry.
SELECTED = {
    25: "GM LS 58x + 4x cam",
    26: "Lotus 36-1-1-1-1",
    27: "Honda RC51 12 + cam",
    28: "36-1 + reference pulse",
    30: "Chrysler NGC 36-2+2 + 6cyl cam",
    31: "Chrysler NGC 36-2+2 + 8cyl cam",
    32: "Weber-Marelli 8 + 2 cam",
    35: "Mazda CAS 24-2 + pulse",
    36: "Yamaha R1 8 + cam",
    38: "GM 6 even + cam",
    39: "GM 8 even + cam",
    40: "Volvo D12 60-2-2-2 + 7 cam",
    41: "Mazda 36-2-2-2 + 6 cam",
    58: "Renix 4cyl symmetrical",     # repeats=4 — measured, not assumed
    59: "Renix 6cyl symmetrical",     # repeats=6
}


def our_gaps(g, present):
    """Extractor spacing indices -> our gap indices, rotated so the first is 0."""
    idx = sorted((i + 1) % present for i in g["missing_at"])
    if not idx:
        return []
    shift = idx[0]
    return sorted((i - shift) % present for i in idx)


def cam_cell(rise, span):
    """Cam edge angles -> inter-edge spans in 0.1 deg, summing to the full cycle."""
    if not rise:
        return []
    r = sorted(rise)
    spans = [r[i + 1] - r[i] for i in range(len(r) - 1)] + [r[0] + span - r[-1]]
    cell = [int(round(s * 10)) for s in spans]
    drift = int(round(span * 10)) - sum(cell)        # the decoder takes the period from the sum
    cell[-1] += drift
    return cell


def main():
    arrays, table = load_arrays(), load_table()
    for idx, name in SELECTED.items():
        r = table[idx]
        s = arrays[r["sym"]]
        deg_per = r["deg"] / len(s)
        print(f'  - name: "{name}"')

        crank_rise, _ = edges(s, 0, deg_per)
        period = 360.0 if r["deg"] == 720 else float(r["deg"])
        g = describe_gap_wheel(crank_rise, period)

        # Every non-crank channel is a cam. A "second crank" would have to repeat each revolution,
        # and none of the selected wheels does — the one called a second TRIGGER fires once per 720,
        # which makes it a phase reference (a cam in decoder terms), not a crank-rate stream. Guessing
        # at that from edge spacing produced nonsense, so it is not guessed at.
        cams = []
        for bit in range(1, 4):
            rise, _ = edges(s, bit, deg_per)
            if rise:
                cams.append(rise)
        crank2 = None

        sync = "PHASE" if cams else "CRANK"
        # A WIDTH cam measures a PULSE, so it must capture BOTH edges — with rising only there is no
        # width to measure and the stream never locks. A SEQUENCE cam matches the ratio of consecutive
        # inter-edge spans and takes rising alone.
        single_pulse = any(len(r) == 1 for r in cams)
        print(f'    sync: "{sync}"')
        print(f'    cam_edge: {(2 if single_pulse else 0) if cams else -1}')
        print("    streams:")
        if g:
            print(f'      - rate: 0\n        role: 1\n        kind: "gap"\n        slots: {g["slots"]}'
                  f'\n        ratio: {g["ratio"]}\n        gaps: {our_gaps(g, g["present"])}')
        else:
            # ONE revolution's worth: a 720-span table lists the crank twice, and feeding both
            # revolutions against a 360 period produces a cell that does not close.
            print(f'      - rate: 0\n        role: 1\n        kind: "seq"'
                  f'\n        cell: {cam_cell([a for a in crank_rise if a < period], period)}')
        if crank2:
            print(f'      - rate: 0\n        role: 2\n        kind: "seq"'
                  f'\n        cell: {cam_cell([a for a in crank2 if a < 360.0], period)}')
        for n, rise in enumerate(cams):
            if len(rise) == 1:
                # A single pulse per cycle is a reference WINDOW, not a sequence. A one-entry
                # sequence is uniform, and a uniform stream locks RELATIVE — it would never give the
                # absolute phase the pulse exists to provide.
                print(f'      - rate: 1\n        role: {3 + n}\n        kind: "width"'
                      f'\n        width_min: 0\n        width_max: 7200\n        width_target: 0')
            else:
                print(f'      - rate: 1\n        role: {3 + n}\n        kind: "seq"'
                      f'\n        cell: {cam_cell(rise, 720.0)}')


if __name__ == "__main__":
    main()
