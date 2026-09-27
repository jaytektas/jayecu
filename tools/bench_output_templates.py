#!/usr/bin/env python3
"""Every output-wizard template, applied and then DRIVEN, on the real ECU.

The wizard's Apply is templateWrites() plus two condition compiles; the companion harness runs that
exact function and emits what it writes, so what lands on the ECU here is what the dialog's button
would land. What this adds is the half a dialog cannot show you: the slot is then driven from both
sides of its own condition, and out_N is read back. A template that compiles is not a template that
works — "uptime_s < 0" compiled perfectly well and meant the pump could never run.

Channels the conditions read are injected at PRIO_LUA (signalWrite), which is how every other bench
here reaches a sensor the rig does not have. rpm is the exception: it comes from the trigger and so
from Ardu-Stim, and templates that need a specific rpm say so rather than pretending.

  python3 tools/bench_output_templates.py --writes <wizard.json> [--slot 21]

--slot is the output ROW, which IS the pin (outputs.output[i] = pin i; 21 = LS10).
"""
import argparse, json, struct, sys, time
sys.path.insert(0, ".")
from tools.ts_bench import TsLink, Meta

try:
    import serial
    from tools.gen_rebench import open_stim, select, configure, WHEELS
    from tools.bench_fuel import set_fixed_rpm, wait_for_sync
except ImportError:
    serial = None

TTL_MS = 500
GENERIC = 3                        # outputs.output[].function: Generic
WHEEL_IDX = 6                      # 36-1: dense, single crank stream, locks quickly
FAN_DUTY = 60.0                    # what Generic Table 1 is filled with for the variable-speed fan

# What each template needs to be TRUE, and what makes it FALSE again. `lua` is injected every tick;
# `note` says what the case is proving. A template whose truth depends on something this rig cannot
# produce says so in `skip` rather than reporting a pass it did not earn.
CASES = {
    "thermo_fan":    dict(on=dict(clt=110.0), off=dict(clt=70.0)),
    # The compressor asking for the fan, with the engine COLD — the term that makes this a different
    # template from thermo_fan. Testing it on coolant alone would pass with the a/c half deleted.
    "thermo_fan_ac": dict(on=dict(clt=70.0, ac_request=1.0), off=dict(clt=70.0, ac_request=0.0)),
    "variable_fan":  dict(on=dict(clt=110.0), off=dict(clt=70.0),
                          note=f"duty comes from Generic Table 1, filled with {FAN_DUTY:.0f}% here"),
    "ac_clutch":     dict(on=dict(ac_request=1.0, tps=10.0, rpm=1500),
                          off=dict(ac_request=0.0, tps=10.0, rpm=1500)),
    # Cranking is BELOW cranking rpm by definition — you cannot crank an engine that is running, and
    # the template says so. The stim must be stopped for the on case, not merely slow.
    "starter":       dict(on=dict(start_sw=1.0, neutral_sw=1.0, clutch_sw=0.0, rpm=0),
                          off=dict(start_sw=0.0, neutral_sw=1.0, clutch_sw=0.0, rpm=0)),
    # "on: 1 / off: 0" — a constant, so there is no input that turns it off and the right assertion is
    # that nothing does. Driven through the widest swing the other templates use.
    "main_relay":    dict(on=dict(clt=110.0, rpm=1200), off=None, always_on=True,
                          note="a constant condition: it must stay on through everything"),
    "shift_light":   dict(on=dict(rpm=7000), off=dict(rpm=1000)),
    "ps_pump":       dict(on=dict(rpm=1500), off=dict(rpm=0)),
    # The prime window is only half of it: the other half is "runs while the wheel turns", so the OFF
    # case has to STOP the wheel. With it spinning the pump is correctly on whatever the prime says.
    # THE CARRIER IS THE SIGNAL, and out_N reports the DUTY — so this proves the gate and the pulse shape
    # (50 %) on the rig, not the frequency, which needs a scope on the pin.
    "tachometer":    dict(on=dict(rpm=1500), off=dict(rpm=0),
                          note="frequency (rpm x ppr / 60) not measured: out_N is the duty"),
    "fuel_pump":     dict(on=dict(rpm=1200), off=dict(rpm=0),
                          note="prime expired AND the wheel stopped"),
}


def set_rpm(stim, rpm):
    """Spin at a fixed rpm, or STOP the wheel outright.

    Zero is not a small number here: 'F' floors at 100 rpm, so a fuel pump asked to prove it switches
    off would sit there watching teeth arrive. 'H'+0x5A masks the timer compare and the wheel stops
    emitting edges at all — a stopped engine, which is the state half of these conditions are about.
    """
    if not stim:
        return False
    stim.reset_input_buffer()
    if rpm <= 0:
        stim.write(b"H\x5A")
        time.sleep(2.5)                              # let age(trigger_teeth) climb past a stop time
    else:
        stim.write(b"h\x5A"); time.sleep(0.3)        # resume, harmless if it was never halted
        stim.write(b"F" + struct.pack("<H", int(rpm)))
        time.sleep(2.0)
    return True


def lua_for(vals):
    body = "\n".join(f'  signalWrite("{k}", {v}, {TTL_MS})' for k, v in vals.items())
    return f"function onTick()\n{body}\nend\nsetTickRate(200)\n" if body else \
           "function onTick()\nend\nsetTickRate(200)\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--writes", required=True)
    # LS10, a row no bench firing map uses — a coil or injector row is the scheduler's, not a slot's.
    ap.add_argument("--slot", type=int, default=21)
    a = ap.parse_args()

    tpl = {t["id"]: t for t in json.load(open(a.writes))}
    L, M = TsLink(), Meta()
    print("ECU:", L.hello())
    # THE TRIGGER, FROM DEFAULTS. Three of these templates gate on rpm, and rpm is a MEASUREMENT the
    # decoder makes from real edges — signalWrite cannot reach it (tried: it reads back 0). A flash
    # leaves bank A at defaults with no wheel configured, so the stim can be spinning happily and
    # trigger_teeth still reads 0. Same setup the cycle-capture rig does, for the same reason.
    stim = None
    if serial:
        try:
            stim = open_stim()
            configure(L, WHEELS[WHEEL_IDX])
            select(stim, WHEEL_IDX)
            set_fixed_rpm(stim, 1200)
            e = wait_for_sync(L, timeout_s=15.0)
            print(f"  stim: {WHEELS[WHEEL_IDX][0]} — " +
                  (f"sync_level={e['sync_level']} rpm={e['rpm']:.0f}" if e else "NO SYNC (rpm cases will fail)"))
        except Exception as ex:
            print(f"  (no stim: {ex} — rpm cases will be skipped)")
            stim = None

    # GENERIC TABLE 1, FILLED. The variable-speed fan reads it for its duty and it ships all zeros, so
    # the template as the wizard leaves it commands 0% however wide open its gate is. That is the
    # template being honest ("edit the cells there"), and it is also why a bench that only asked
    # "is it above zero" learned nothing about the gate.
    gt = M.config["generic_tables"]["table_1"]
    L.write_raw(gt["offset"], struct.pack("<%df" % (gt["rows"] * gt["cols"]),
                                          *([FAN_DUTY] * (gt["rows"] * gt["cols"]))))
    arr = M.config["outputs"]["output"]
    base, F = arr["base_offset"] + a.slot * arr["stride"], arr["fields"]
    ch = f"out_{a.slot + 1}"

    SUB = arr.get("arrays") or {}

    def field(f):
        """A field of the row, or of one of its sub-arrays ("cand[0].sig") — (rel_offset, descriptor)."""
        if f in F:
            return F[f]["rel_offset"], F[f]
        name, rest = f.split("[", 1)
        k, sub = rest.split("].", 1)
        sa = SUB[name]
        return sa["rel_offset"] + int(k) * sa["stride"] + sa["fields"][sub]["rel_offset"], sa["fields"][sub]

    def off(f):  return base + field(f)[0]
    def put(f, v):
        d = field(f)[1]; dt = d["datatype"]
        L.write_raw(off(f), struct.pack(M.fmt(dt), int(v) if dt[0] in "US" else v))
    def out():   return L.telem(ch) * M.t(ch)["scale"]

    results = []
    for tid, t in tpl.items():
        case = CASES.get(tid, {})
        L.write_raw(off("function"), b"\x00")           # rebuild from a known-off row
        time.sleep(0.3)
        for f, v in t["values"].items():
            if f in ("enabled", "function"):  continue   # armed last, so the rebuild sees a whole row
            put(f, v)
        for f, key in (("on_expr", "on_expr"), ("off_expr", "off_expr"), ("freq_expr", "freq_expr")):
            blob = bytes.fromhex(t["code"].get(key, ""))
            L.write_raw(off(f), blob.ljust(F[f]["size"], b"\0"))
        put("function", GENERIC)
        time.sleep(0.4)

        row = {"id": tid, "note": case.get("note", "")}
        for phase in ("on", "off"):
            want = case.get(phase)
            if want is None:
                row[phase] = None
                continue
            want = dict(want)                            # per-phase copy: rpm is popped out of it
            if "rpm" in want:                            # rpm is a TRIGGER measurement, not a signal
                set_rpm(stim, want.pop("rpm"))
            L.set_script(lua_for(want))
            # THE SLOT'S OWN ANTI-CHATTER TIME, not a flat guess. A thermo fan holds each state for
            # five seconds by design — "a relay that chatters is a relay that welds" — so a bench that
            # waits a fraction of that reads the state the gate is still refusing to leave, and calls
            # a correct refusal a stuck output. Which is exactly what it did on the first run.
            hold = max(t["values"].get("min_on_ms", 0), t["values"].get("min_off_ms", 0))
            time.sleep(max(1.0, hold / 1000.0 + 1.0))
            row[phase] = out()
            if case.get("always_on") and phase == "on":
                # Swing everything the other templates swing and check it does not budge.
                set_rpm(stim, 0); L.set_script(lua_for(dict(clt=20.0))); time.sleep(2.0)
                row["held"] = out()
                set_rpm(stim, 1200)
        results.append(row)
        L.write_raw(off("function"), b"\x00")

    L.restore_script()
    L.write_raw(off("function"), b"\x00")
    if stim:
        set_rpm(stim, 0)
        stim.close()
    L.close()

    print(f"\n{'template':16s} {'ON':>7s} {'OFF':>7s}  verdict")
    bad = 0
    for r in results:
        on, of = r["on"], r["off"]
        if r.get("held") is not None:
            v = ("holds through every input" if on > 0 and r["held"] == on
                 else f"CONSTANT CONDITION MOVED (on={on} then {r['held']})")
            if not (on > 0 and r["held"] == on): bad += 1
        elif on is None or of is None:
            v = "partial — " + (r["note"] or "not drivable on this rig")
        elif on > 0 and of == 0:
            v = "gates both ways"
        elif on == of:
            v = "STUCK — condition never changed the output"; bad += 1
        else:
            v = f"UNEXPECTED (on={on} off={of})"; bad += 1
        print(f"{r['id']:16s} {('-' if on is None else f'{on:.0f}%'):>7s} "
              f"{('-' if of is None else f'{of:.0f}%'):>7s}  {v}")
    print(f"\n{len(results)} templates, {bad} failing")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
