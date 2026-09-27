#!/usr/bin/env python3
"""The starter's CRANK CYCLE, on the real ECU, as a timeline rather than two static cases.

bench_output_templates.py drives every template from both sides of its condition and reads the pin
back. That cannot see this one, because every rule the starter obeys is about TIME:

  a tap latches   — the press is 200 ms and the crank is ten seconds, so "condition true -> pin high"
                    is not the claim being made. The claim is that the pin stays high after the
                    condition has gone, which is the gate's deadband and nothing to do with the VM.
  it self-limits  — maximum-on releases it whatever the button says, then locks it out to cool.
  a hold re-cranks— and only after the rest, which a single sample cannot distinguish from a stuck
                    output or from a gate that never tripped at all.
  RUNNING ends it — from the run STATE, not from rpm, so a stumble under cranking_rpm cannot re-engage
                    the starter on a spinning engine.

So this samples out_N continuously and asserts the EDGES and their times.

  python3 tools/bench_starter_cycle.py --writes <wizard.json> [--slot 23]

--slot is the output ROW, which IS the pin (outputs.output[i] = pin i; 23 = LS12).

`wizard.json` is the studio's own apply output (tests/template_emit), so what is under test is what
the wizard's OK button would write — not a re-implementation of it.
"""
import argparse, json, struct, sys, time

sys.path.insert(0, ".")
from tools.ts_bench import TsLink, Meta

try:
    from tools.gen_rebench import open_stim, select, configure, WHEELS
    from tools.bench_fuel import set_fixed_rpm
except ImportError:
    open_stim = None

TTL_MS = 500
WHEEL_IDX = 6            # 36-1: dense, single crank stream, locks quickly
SAMPLE_HZ = 20


def lua(vals):
    body = "\n".join(f'  signalWrite("{k}", {v}, {TTL_MS})' for k, v in vals.items())
    return f"function onTick()\n{body}\nend\nsetTickRate(200)\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--writes", required=True)
    ap.add_argument("--slot", type=int, default=23)
    a = ap.parse_args()

    M, L = Meta(), TsLink()
    print(L.hello())
    tpl = {t["id"]: t for t in json.load(open(a.writes))}
    t = tpl["starter"]
    print(f"  on : {t['src']['on']}")
    print(f"  off: {t['src']['off']}")

    arr = M.config["outputs"]["output"]
    base, F = arr["base_offset"] + a.slot * arr["stride"], arr["fields"]
    ch = f"out_{a.slot + 1}"
    scale = M.t(ch)["scale"]

    def off(f):
        return base + F[f]["rel_offset"]

    def put(f, v):
        d = F[f]["datatype"]
        L.write_raw(off(f), struct.pack(M.fmt(d), int(v) if d[0] in "US" else v))

    def out():
        return L.telem(ch) * scale

    # ---- apply the template exactly as the wizard would, then arm it ----
    L.write_raw(off("function"), b"\x00")            # rebuild from a known-off row
    time.sleep(0.3)
    for f, v in t["values"].items():
        if f not in ("enabled", "function"):
            put(f, v)
    for f in ("on_expr", "off_expr"):
        blob = bytes.fromhex(t["code"][f])
        L.write_raw(off(f), blob.ljust(F[f]["size"], b"\0"))

    # THE INTERLOCK THIS RIG DOES NOT HAVE. The shipped choice is "in neutral or clutch down" and
    # neither switch is wired here, so the condition would answer no for ever — which is the bug the
    # options exist to fix, not the thing under test. Inject one, the way every other bench here
    # reaches a sensor the rig lacks.
    held = {"neutral_sw": 1.0, "start_sw": 0.0}
    L.set_script(lua(held))
    time.sleep(0.5)
    put("function", 3)                               # Generic
    time.sleep(0.5)

    fails = []

    def check(ok, what, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {what}" + (f"   — {detail}" if detail else ""))
        if not ok:
            fails.append(what)

    def press(v):
        held["start_sw"] = float(v)
        L.set_script(lua(held))

    def trace(secs, label):
        """Sample out_N for `secs`, returning [(t, on)] edges and the raw series."""
        t0, series = time.time(), []
        while time.time() - t0 < secs:
            series.append((time.time() - t0, out() > 50.0))
            time.sleep(1.0 / SAMPLE_HZ)
        edges = [(round(tt, 2), on) for (tt, on), (_, prev)
                 in zip(series[1:], series) if on != prev]
        print(f"    [{label}] start={'on' if series[0][1] else 'off'} edges={edges}")
        return series, edges

    print("\n-- born off --")
    check(out() <= 50.0, "a freshly built slot is not driving its pin", f"out={out():.0f}%")

    print("\n-- a 200 ms tap latches a 10 s crank --")
    press(1)
    time.sleep(0.35)
    press(0)                                          # let go: the crank must outlive this
    series, edges = trace(14.0, "tap")
    on_edges = [tt for tt, on in edges if on]
    off_edges = [tt for tt, on in edges if not on]
    check(series[0][1] or bool(on_edges), "the tap started a crank")
    check(bool(off_edges), "…which ended on its own")
    if off_edges:
        # measured from the press, which was ~0.35 s before the trace began
        length = off_edges[0] + 0.35
        check(8.5 < length < 11.5, "…after about ten seconds, not when the button was released",
              f"{length:.1f} s")
    check(len(on_edges) <= 1, "…and the re-arm expiring did not start another",
          f"{len(on_edges)} rising edges")

    print("\n-- a held button cranks, rests, cranks again --")
    press(1)
    series, edges = trace(26.0, "hold")
    rise = [tt for tt, on in edges if on]
    fall = [tt for tt, on in edges if not on]
    check(len(fall) >= 1 and len(rise) >= 1,
          "it tripped and then re-cranked while the button stayed down",
          f"rises={rise} falls={fall}")
    if fall and rise:
        rest = min((r - f for r in rise for f in fall if r > f), default=None)
        check(rest is not None and 2.0 < rest < 4.5,
              "…after the three second rest, during which it did nothing",
              f"{rest:.1f} s" if rest else "no rest measured")

    print("\n-- RUNNING ends it, whatever the button says --")
    stim = None
    if open_stim:
        try:
            stim = open_stim()
            configure(L, WHEELS[WHEEL_IDX])
            select(stim, WHEEL_IDX)
            set_fixed_rpm(stim, 1200)                  # well past cranking_rpm -> RUNNING
            time.sleep(3.0)
            st = L.telem("engine_state")
            check(st == 2, "the engine reached RUNNING on the stim", f"engine_state={st}")
            time.sleep(1.0)
            check(out() <= 50.0, "…and the starter let go with the button still held",
                  f"out={out():.0f}%")
        except Exception as ex:
            print(f"    (no stim: {ex} — RUNNING case skipped)")
    else:
        print("    (pyserial/stim unavailable — RUNNING case skipped)")

    # ---- leave the rig as it was found ----
    press(0)
    L.restore_script()
    L.write_raw(off("function"), b"\x00")
    if stim:
        set_fixed_rpm(stim, 0)
        stim.close()

    print(f"\n{'ALL PASSED' if not fails else str(len(fails)) + ' FAILED: ' + ', '.join(fails)}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
