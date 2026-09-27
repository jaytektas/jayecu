#!/usr/bin/env python3
"""LOAD THE ECU UP AND SEE WHERE THE TIME GOES.

Builds a deliberately heavy tune in RAM — twelve cylinders, coil-on-plug, one sequential injection
stage, every module that has a switch turned on, eight sensors each on their own analog pin — spins
the stim through the rev range and reports what the MCU is doing at each step.

It is a WORST CASE, not a typical car: nothing here is plumbed to anything, so the DTC list is long
and half the modules are judging absent inputs. That is the point. The number to watch is the engine
FRAME load, because that is the one that decides whether spark is ever late; MCU load says how much
of the chip is spoken for, which is a different question (see DtcManager/EngineTask for the note).

Nothing is burned: a reset puts the flashed tune back.

    python3 tools/bench_cpu_load.py
    python3 tools/bench_cpu_load.py --rpm 8000      # one step only
"""
import argparse, json, struct, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ts_bench import TsLink, Meta
from gen_rebench import open_stim, select, configure, WHEELS
from output_rows import write_engine_rows, SEQUENTIAL

WHEEL = 4                       # 60-2 + cam: dense crank plus a cam, so COP has phase to find
FIRING = [1, 7, 5, 11, 3, 9, 6, 12, 2, 8, 4, 10]        # a V12 order
STEPS = (800, 2000, 4000, 6000, 8000)


def build(l, m, D):
    def setf(name, v):
        c = m.c(name)
        fmt = {'U08': '<B', 'S08': '<b', 'U16': '<H', 'S16': '<h',
               'U32': '<I', 'S32': '<i', 'F32': '<f'}[c['datatype']]
        l.write_raw(c['offset'], struct.pack(fmt, v))

    setf('engine_cylinder_count', 12)
    setf('engine_cycle_type', 1)        # Four-Stroke (720)
    setf('engine_num_inj_stages', 1)
    fo = m.config['engine']['firing_order']
    for i, c in enumerate(FIRING):
        l.write_raw(fo['base_offset'] + i * fo['stride'], bytes([c]))
    l.write_raw(m.array_offset('engine', 'inj_stage', 0, 'mode'), bytes([SEQUENTIAL]))
    l.write_raw(m.array_offset('engine', 'inj_stage', 0, 'injections_per_cycle'), bytes([1]))
    write_engine_rows(l, FIRING, coils="cop", stages=[(SEQUENTIAL, None)])   # a coil + injector per cylinder

    mods = [k for k, v in m.config.items() if isinstance(v, dict) and isinstance(v.get('enabled'), dict)]
    for mod in mods:
        try: setf(f'{mod}_enabled', 1)
        except Exception: pass

    # EVERY PHYSICAL INPUT THE BOARD HAS, each to a different sensor — twenty analog pins and eight
    # digital ones. A sensor with no source builds no pipeline and costs nothing, and two sensors on
    # one pin lose arbitration and also cost nothing, so "a lot of sensors enabled" only means anything
    # if each one has a pin to itself. On-board sensors (battery, ECU temperature) are locked to their
    # own hardware and need no assignment.
    AV_PINS, DIG_PINS = 20, 8
    sen = m.config['sensors']['sensor']
    ids = D['config']['sensors']['sensor']['element_ids']
    ifl = D['config']['sensors']['sensor']['element_interfaces']
    IF_ANALOG, IF_DIGFREQ = 0, 2                 # SensorIfaceSel order (analog_voltage, …, digital_freq)
    on, av, dig, onboard = [], 0, 0, 0
    for i, (sid, ifs) in enumerate(zip(ids, ifl)):
        off = sen['base_offset'] + i * sen['stride']
        if 'on_board' in ifs:                                    # locked: enable and leave the rest
            l.write_raw(off + 0, bytes([1])); onboard += 1; on.append(sid); continue
        if 'analog_voltage' in ifs and av < AV_PINS:
            l.write_raw(off + 0, bytes([1]))
            l.write_raw(off + 2, bytes([IF_ANALOG]))
            l.write_raw(off + 3, bytes([av])); av += 1; on.append(sid); continue
        if 'digital_freq' in ifs and dig < DIG_PINS:
            l.write_raw(off + 0, bytes([1]))
            l.write_raw(off + 2, bytes([IF_DIGFREQ]))
            l.write_raw(off + 3, bytes([dig])); dig += 1; on.append(sid); continue
    print(f"sensors: {av} on analog pins, {dig} on digital pins, {onboard} on-board")
    return len(mods), on


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rpm", type=int, action="append")
    ap.add_argument("--tasks", action="store_true", help="per-task rows at every step, not just the last")
    args = ap.parse_args()
    steps = args.rpm or list(STEPS)

    m = Meta()
    D = json.JSONDecoder().raw_decode(Path('shared/tuneit-meta.json').read_bytes()
                                      .decode('utf-8', 'surrogateescape'))[0]
    sp = open_stim()
    l = TsLink()
    # THE WHEEL FIRST — a config write rebuilds every pipeline, so it happens before the tune goes in.
    configure(l, WHEELS[WHEEL]); l.cmd(b"E", b"reconfig"); time.sleep(0.5)
    select(sp, WHEEL, reps=4)
    nmod, sensors = build(l, m, D)
    l.cmd(b"E", b"reconfig"); time.sleep(1.5)
    print(f"12 cyl COP, 1-stage sequential, {nmod} modules on, {len(sensors)} sensors live, "
          f"{WHEELS[WHEEL][0]}\n")

    for rpm in steps:
        sp.reset_input_buffer(); sp.write(b"F" + struct.pack("<H", rpm)); time.sleep(3.0)
        l.execute('cpu reset'); time.sleep(3.0)      # the peak is THIS step's, not the way up
        out = l.execute('cpu').splitlines()
        t = l.telem_all()
        print(f"{rpm:5d} rpm : {out[0]:<34} ecu rpm={t['rpm']:.0f} sync={t['sync_level']:.0f} "
              f"dtc={t.get('dtc_active', 0):.0f}")
        if args.tasks or rpm == steps[-1]:
            for r in out[1:7]:
                print("             " + r.strip())
    sp.close(); l.close()
    print("\nnothing burned — a reset puts the flashed tune back")


if __name__ == "__main__":
    sys.exit(main())
