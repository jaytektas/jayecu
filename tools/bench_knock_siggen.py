#!/usr/bin/env python3
"""Knock detection against a REAL analog signal, driven end to end.

Everything here is measured through the actual chain — generator, attenuator, ECU input network,
ADC3 burst, Biquad, learned floor, classifier — with nothing injected and nothing simulated. That is
the half `bench_knock.py` cannot reach: it proves the firmware PATH by handing the classifier a
synthetic profile, this proves the path is fed by something real and that the numbers mean what they
claim.

Needs the DSO2D15 on USBTMC (see tools/dso2d15.py) with its output into KNOCK1, and the Ardu-Stim
spinning a cam-bearing wheel. The generator's burst is manual-trigger-only from the front panel, but
:DDS:BURSt:TRIGger is a remote press, which is what makes any of this automatic.

    python3 -m tools.bench_knock_siggen [rpm]

Sections:
  1. FLOOR         with the generator silent, every cylinder converges on the same quiet level
  2. LADDER        amplitude swept down until detection stops; the cliff must land on the configured
                   threshold, which is the only test that says the dB scale means what it says
  3. NEVER DEAF    a sustained loud tone must NOT be learned as normal — the floor has to stay put
  4. ROUTING       cylinders split across KNOCK1/KNOCK2; only those on the driven input may report
"""
import math
import re
import sys
import time

from tools.dso2d15 import Dso2d15
from tools.ts_bench import TsLink
from tools.gen_rebench import open_stim, select, configure, WHEELS
from tools.bench_fuel import set_fixed_rpm, wait_for_sync, INJECT, TTL_MS
from tools.bench_cycle import configure_firing

WHEEL_IDX = 4            # "60-2 crank and cam" — segment/knock work needs PHASE sync
DEFAULT_RPM = 1500
FREQ = 7000

fails = []


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}: {detail}")
    if not cond:
        fails.append(name)


def script():
    lines = [f'  signalWrite("{n}", {v}, {TTL_MS})' for n, v in INJECT.items()]
    return "function onTick()\n" + "\n".join(lines) + "\nend\nsetTickRate(200)\n"


def window_db(l, n=8):
    """Mean level (dB) of the next n classified windows — what the input is hearing NOW, unlike the
    learned floor, which is built to refuse a sustained loud tone (section 3)."""
    out, seen, t0 = [], set(), time.time()
    while len(out) < n and time.time() - t0 < 8.0:
        m = re.search(r"last#(\d+) .*? db_x10=(-?\d+)", l.execute("knk"))
        if m and m.group(1) not in seen:
            seen.add(m.group(1)); out.append(int(m.group(2)) / 10.0)
        time.sleep(0.15)
    return sum(out) / len(out) if out else float("nan")


def knk(l):
    o = l.execute("knk")
    fl = re.findall(r"(-?\d+)/(\d+)/(\d+)", o)
    return dict(knocks=int(re.search(r"knocks=(\d+)", o).group(1)),
                retard=int(re.search(r"retard_x10=(-?\d+)", o).group(1)) / 10.0,
                floors=[(int(a) / 10.0, int(b)) for a, b, _ in fl])


def main():
    rpm = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_RPM
    print(f"=== knock vs signal generator @ {rpm} rpm ===")

    d = Dso2d15()
    print(f"  gen: {d.idn()}")
    d.burst_off(); d.wave("SINE"); d.freq(FREQ); d.offset(0); d.output(False)

    sp = open_stim()
    l = TsLink(); configure(l, WHEELS[WHEEL_IDX]); configure_firing(l)
    l.set_config("knock_enabled", 1)
    l.set_config("knock_knock_frequency", FREQ)
    cs = l.meta.config["knock"]["cyl_sensor"]
    base, stride = cs["base_offset"], cs["stride"]
    # SAVE IT, because section 4 below deliberately SPLITS the routing and this suite used to walk
    # away leaving it split. That lives in ECU RAM and outlives the run: the next bench_knock read
    # floors of [-43.0, -42.6, -64.6, -64.4] and reported five failures, because two of its
    # cylinders were listening to a quieter input while it asserted they all shared one.
    routing_saved = [l.read_config_raw(base + c * stride, 1) for c in range(4)]
    for c in range(4):                       # everything on KNOCK1 for sections 1-3
        l.write_raw(base + c * stride, bytes([0]))
    l.close()
    select(sp, WHEEL_IDX, reps=4); set_fixed_rpm(sp, rpm)

    l = TsLink()
    if not wait_for_sync(l):
        print("  [FAIL] sync"); return 1
    l.execute("key on"); l.set_script(script())

    # ---- 1. the floor, with nothing driving it -------------------------------------------------
    d.output(False)
    time.sleep(14)
    s = knk(l)
    floors = [f for f, _ in s["floors"]]
    quiet = sum(floors) / len(floors)
    print(f"  silent floor {quiet:.1f} dB   per-cyl {floors}")
    check("every cylinder learned a floor", all(n > 0 for _, n in s["floors"]),
          f"samples {[n for _, n in s['floors']]}")
    check("and they agree within 3 dB", max(floors) - min(floors) < 3.0,
          f"spread {max(floors) - min(floors):.1f} dB")

    # ---- PRECONDITION: is the generator actually REACHING the knock input? ----------------------
    # Everything below measures where detection stops as the level falls. That is only a statement
    # about the ECU if the level is arriving at all, and on 2026-09-06 it stopped arriving: 1 Vpp
    # continuous moved the KNOCK1 floor by 1.8 dB and KNOCK2 by 0.2 dB, where the same rig earlier
    # the same day sat at -39/-41 dB driven against a -63 dB idle line. With no signal the ladder
    # cannot cross the 12 dB threshold at any amplitude and the sustained-tone section has nothing
    # loud to gate on — so BOTH remaining checks fail, correctly, and say nothing about the module.
    #
    # This is the same discipline bench_dtr_noise applies to sync: refuse to measure when the thing
    # being measured cannot be present, and say which it is. A dead coax reads exactly like a knock
    # module that will not trigger, and the difference is worth one 12 s check.
    #
    # MEASURED ON THE WINDOWS, NOT THE FLOOR. This used to look for the learned floor rising under the
    # tone — but the floor is built NOT to learn a sustained loud tone (section 3 asserts exactly that),
    # so on firmware that does its job the floor barely moves (+0.4 dB on 2026-10-01) and this blocked the
    # suite with a wiring diagnosis while the tone was arriving at +60 dB over quiet.
    quiet_win = window_db(l)
    d.burst_off(); d.amp(1.0); d.output(True)
    time.sleep(2)
    driven_win = window_db(l)
    d.output(False)
    rise = driven_win - quiet_win
    print(f"  1 Vpp continuous lifts the window level by {rise:+.1f} dB  ({quiet_win:.1f} -> {driven_win:.1f})")
    if not rise >= 6.0:
        print("  THE GENERATOR IS NOT REACHING THE KNOCK INPUT. 1 Vpp should lift the window level by tens")
        print("  of dB, not a couple. Check the coax from the generator to the ECU knock pin and the")
        print("  20:1 divider. Everything below would fail for that reason and prove nothing, so it")
        print("  is not run.")
        d.output(False); d.close()
        for c in range(4):
            l.write_raw(base + c * stride, routing_saved[c])
        l.restore_script(); l.execute("key auto"); l.cmd(bytes([0x22])); l.close(); sp.close()
        print("\n=== BLOCKED: signal generator not reaching the ECU (wiring), no verdict on knock ===")
        return 2
    time.sleep(8)                            # let the floor settle back down before the ladder

    # ---- 2. the ladder: where does detection actually stop? -------------------------------------
    d.burst_setup(200)                       # 200 cycles @7k = 28.6 ms ~ 1.4 windows at 1500 rpm
    # The level column is PREDICTED, not measured, and that is not laziness: a gated burst cannot be
    # read by the free-running `knock` capture, which samples whenever it is asked and therefore
    # catches the silence between bursts. An earlier version measured it anyway and printed ~-58 dB
    # at every amplitude — the idle line, dressed up as a signal level.
    #
    # The model is the one anchored on the bench earlier: dB = 20*log10(Vpp at the ADC) - 9, with the
    # generator-to-ADC path measured at 0.475x through the 20:1 divider. Predicting it and watching
    # where detection stops is what tests the model; measuring it here would not.
    # KNOWN OPEN, 2026-09-06 — this suite ran for the first time today (/dev/usbtmc0 was root-only
    # until now, so every previous sweep tracebacked in 0 s). Two checks fail and the cause is NOT
    # yet established. What is established, by direct measurement:
    #
    #   * every rung of the ladder below fires 112-114 events, from 120 mVpp down to 12 mVpp — a 10:1
    #     range whose last rung is 2 dB BELOW the learned floor. There is no cliff because nothing
    #     changes with amplitude.
    #   * the ECU does not knock spuriously: generator output OFF gives 0 knocks over 3 s.
    #   * the amplitude command takes: 120/60/20/12 mVpp read back as 1.2e-1/6.0e-2/2.0e-2/1.2e-2.
    #   * the burst IS gated. Armed via burst_setup() with ZERO trigger() calls: 0 knocks over 3 s.
    #     (An earlier note here claimed the tone was continuous and that was WRONG — this test
    #     disproves it. Left in as the correction, because the wrong version was committed once.)
    #
    # And the part that says stop guessing: the ladder CANNOT BE REPRODUCED OUTSIDE THIS SUITE. Set
    # the same wheel, firing, knock config, routing, rpm, script and key state by hand, then fire ten
    # 200-cycle bursts at 120 mVpp — the level that scores 113 events here — and the ECU counts 0.
    # Starting fresh at 12 mVpp also counts 0, where the descending ladder counts 113 at that same
    # rung. So the events are produced by state that sections 1-2 establish and a reconstruction does
    # not, and until that state is identified neither failure can be attributed to the knock module,
    # the instrument, or this suite's own setup.
    #
    # This needs a scope on the generator output and on the ECU's knock input, together, while the
    # ladder runs. That is bench work, not something more telemetry will settle.
    print("\n  amplitude ladder (10 bursts each):")
    print("    gen mVpp   events   predicted dB   over floor")
    cliff_hi = cliff_lo = None
    for mv in (120, 80, 60, 40, 30, 25, 20, 16, 12):
        d.amp(mv / 1000.0); time.sleep(2.0)
        k0 = knk(l)["knocks"]
        for _ in range(10):
            d.trigger(); time.sleep(0.30)
        k1 = knk(l)["knocks"]
        n = k1 - k0
        pred = 20.0 * math.log10(mv * 0.475 / 1000.0) - 9.03
        print(f"     {mv:6d}    {n:4d}      {pred:7.1f}      {pred - quiet:6.1f}")
        if n > 0:
            cliff_hi = mv
        elif cliff_hi is not None and cliff_lo is None:
            cliff_lo = mv
    check("detection has a clean cliff", cliff_hi is not None and cliff_lo is not None,
          f"fires at {cliff_hi} mV, silent at {cliff_lo} mV")

    # ---- 3. a sustained loud tone must NOT become the new normal --------------------------------
    print("\n  sustained tone — the floor must NOT learn it:")
    d.burst_off(); d.amp(0.5); d.output(True)
    time.sleep(3)
    before = knk(l)["floors"][0]
    time.sleep(12)
    after = knk(l)["floors"][0]
    print(f"    floor {before[0]:.1f} -> {after[0]:.1f} dB, samples {before[1]} -> {after[1]}")
    check("the floor did not chase the tone", abs(after[0] - before[0]) < 3.0,
          f"moved {after[0] - before[0]:+.1f} dB")
    check("and it stopped counting the loud cycles as evidence", after[1] - before[1] < 20,
          f"+{after[1] - before[1]} samples over 12 s (a learning cell gains ~600)")

    # ---- 4. per-cylinder routing ----------------------------------------------------------------
    print("\n  routing — cyl 1,2 on KNOCK1 (driven), cyl 3,4 on KNOCK2 (idle):")
    l.restore_script(); l.close()
    l = TsLink()
    for c, inp in ((0, 0), (1, 0), (2, 1), (3, 1)):
        l.write_raw(base + c * stride, bytes([inp]))
    l.execute("key on"); l.set_script(script())
    time.sleep(14)
    s = knk(l)
    f = s["floors"]
    print(f"    floors {[x for x, _ in f]}  samples {[n for _, n in f]}")
    # The driven pair sits far above the idle pair, or is frozen because it is permanently knocking.
    driven_hot = (f[0][0] - f[2][0] > 5.0) or (f[0][1] - f[2][1] < -50)
    check("the driven cylinders behave differently from the idle ones", driven_hot,
          f"knock1 pair {f[0]}, knock2 pair {f[2]}")

    d.output(False); d.close()
    for c in range(4):                       # and put the routing back before anyone else runs
        l.write_raw(base + c * stride, routing_saved[c])
    # GIVE THE CARD BACK — hygiene, matching what bench_datalog has always done at its own exit.
    # "key on" above sets the SD-ownership override and "key auto" does not clear it; SD_RELEASE
    # (0x22) returns ownership to key-driven.
    #
    # BE CLEAR ABOUT WHAT THIS DOES NOT FIX. bench_learned runs immediately after this suite
    # alphabetically, and its first run after this one FAILS — the learned region restores as zeros
    # instead of the totem just written, and the totem store reinitialises (the sequence counter was
    # seen dropping 67 -> 3 and 5 -> 1). It then self-heals: three consecutive runs after that pass
    # with the sequence climbing normally. Adding this release did NOT stop it, and the two obvious
    # mechanisms are both ruled out by direct test:
    #   - the key override surviving the reset: setting "key on", resetting, and reading the region
    #     back gives 0xA5 and an unchanged sequence. So does "key auto". Not it.
    #   - the engine still spinning (this suite closes the stim port without halting it): running
    #     bench_learned with the rig at 1200 rpm passes, and passes again with it halted. Not it.
    # Unresolved, and reproducible only as "the first learned after this suite".
    l.restore_script(); l.execute("key auto")
    l.cmd(bytes([0x22]))                     # SD_RELEASE: back to key-driven
    l.close(); sp.close()
    print()
    print("=== " + ("ALL PASS" if not fails else f"{len(fails)} FAILED: {fails}") + " ===")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
