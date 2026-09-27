#!/usr/bin/env python3
"""What is a stim wheel's TRUE repeat period — measured, not assumed.

`repeats` in the trigger config is how many times a stream's pattern recurs per engine cycle, and
guessing it is how a symmetrical wheel gets configured as something it is not. This derives it from
the rig's own sample table: the smallest rotation under which the channel's sample pattern is
invariant. repeats = span / that rotation.

    python3 -m tools.stim_repeats [name-substring]
"""
import sys
from tools.extract_stim_wheels import load_arrays, load_table


def repeats_of(bits):
    """How many times the pattern recurs over the array — the LARGEST count that is invariant.

    Searched as the SMALLEST non-zero shift, not the smallest repeat count: a shift of n is the whole
    array and is invariant by definition, so counting up from r=1 returns 1 for everything. That
    tautology made every wheel on the rig look non-repeating.
    """
    n = len(bits)
    for shift in range(1, n):
        if n % shift:
            continue
        if all(bits[i] == bits[(i + shift) % n] for i in range(n)):
            return n // shift
    return 1


def main():
    want = sys.argv[1].lower() if len(sys.argv) > 1 else ""
    arrays, table = load_arrays(), load_table()
    for idx, r in enumerate(table):
        if want and want not in r["name"].lower():
            continue
        s = arrays.get(r["sym"])
        # Use the ARRAY length, not the declared count — several tables disagree with their
        # own arrays, and skipping those hid the wheels most worth looking at.
        if not s:
            continue
        chans = max(max(s).bit_length(), 2)
        for bit in range(chans):
            bits = [(v >> bit) & 1 for v in s]
            if len(set(bits)) < 2:
                continue
            rep = repeats_of(bits)
            rise = sum(1 for i in range(len(bits)) if bits[i] and not bits[i - 1])
            lbl = "crank" if bit == 0 else f"cam{bit}"
            print(f"[{idx:3d}] {r['name'][:36]:38s} {lbl:6s} span={r['deg']:4d} rise={rise:3d} "
                  f"repeats={rep:2d}  period={r['deg']/rep:7.2f} deg")


if __name__ == "__main__":
    raise SystemExit(main())
