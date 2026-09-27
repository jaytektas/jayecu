#!/usr/bin/env python3
"""The MULTI-POSITION SWITCH input on the real ECU: a monitor for a hand-driven pin, and a scripted suite
driven by the DSO2D15's generator.

Borrows Auxiliary Input 1 as a multi_switch on a FREE AV pin with four voltage bands (in ECU volts):

    0.40 - 0.80      4   CANCEL
    1.20 - 1.80      3   RES
    2.20 - 2.80      2   SET
    3.20 - 3.80      1   REST (1, not 0, so it can be told from invalid in telemetry)
    below 0.20       fault: raw low (the floor — a dead reference or a short to ground)
    anywhere else    held for 100 ms (crossing a gap), then fault: raw high

Telemetry carries a value but not its validity (an invalid reading packs as 0), so a borrowed Lua script
republishes the position with -1 for INVALID.

MONITOR (default): prints a line whenever the position, either 5 V reference's power-good or the active
DTC set changes. It asserts nothing — it cannot know what the knob is doing.

--gen: drives the pin from the DSO2D15 generator (scope CH1 on the same pin reads it back) and asserts:
DC levels in every band, a gap, above the floor, under it; SET presses of 10/20/30 ms never believed
and 80/150 ms always; 30-80 ms in a gap held, 150-300 ms a fault; 30 ms dips under the floor invalid.

THE PIN MUST BE FREE — the input it borrows would otherwise lose pin arbitration to whatever owns it.
THE ECU READS ITS AV PINS ~4 % LOW (board file: the MCP6004 front end's resistor gain is 0.628 against
the modelled 0.66), so pin volts and ECU volts differ; the levels here sit well inside their bands either way.
GENERATOR QUIRKS, found on the bench: |offset| + amplitude/2 is limited to 3.5 V and a setting that
breaks it is REFUSED silently (so the order of settings matters, and everything is read back); it drops to
0 V and bursts for ~0.8 s after ANY change; duty is :DDS:DUTY (the HIGH fraction).

Everything it changes — the sensor element, the Lua slot, the generator output — is put back on exit.

    python3 tools/bench_multi_switch.py --av 5 [--secs 600] [--scope]
    python3 tools/bench_multi_switch.py --av 5 --gen
"""
import argparse, struct, sys, time

sys.path.insert(0, ".")
from tools.ts_bench import TsLink, Meta

BANDS = [(0.40, 0.80, 4), (1.20, 1.80, 3), (2.20, 2.80, 2), (3.20, 3.80, 1)]
NAMES = {4: "CANCEL", 3: "RES", 2: "SET", 1: "REST", -1: "INVALID"}
FULL_V, FULL_COUNTS = 5.0, 4095
LUA = """function onTick()
  local v = signalRead("aux_1")
  if v == nil then v = -1 end
  signalWrite("lua_gauge_1", v, 500)
end
setTickRate(200)
"""


# Counting script for the generator tests: what the ECU REPORTED, sample by sample, rather than what a
# 20 ms telemetry poll happens to catch. Running totals — gauge_1 = position now (-1 invalid), gauge_2 =
# invalid samples, gauge_3..6 = samples at REST / SET / RES / CANCEL — and a test reads the DIFFERENCE
# across its window. Pushed ONCE: pushing the script is a config write, and every config write rebuilds
# every sensor pipeline, which throws away what the decoder had settled on.
LUA_COUNT = """local n = {[-1] = 0, [1] = 0, [2] = 0, [3] = 0, [4] = 0}
function onTick()
  local v = signalRead("aux_1")
  local p = -1
  if v ~= nil then p = math.floor(v + 0.5) end
  if n[p] ~= nil then n[p] = n[p] + 1 end
  signalWrite("lua_gauge_1", p, 500)
  signalWrite("lua_gauge_2", n[-1], 500)
  signalWrite("lua_gauge_3", n[1], 500)
  signalWrite("lua_gauge_4", n[2], 500)
  signalWrite("lua_gauge_5", n[3], 500)
  signalWrite("lua_gauge_6", n[4], 500)
end
setTickRate(200)
"""


def counts(v, up):
    x = v / FULL_V * FULL_COUNTS
    return int(x + 0.999) if up else int(x)


def active_dtcs(L):
    """Active codes from the DTC table image ('G'): 36-byte records from offset 10, status bit0 = active."""
    _, img = L.cmd(b"G")
    n = struct.unpack_from("<H", img, 8)[0]
    out = []
    for i in range(n):
        off = 10 + 36 * i
        if off + 36 > len(img):
            break
        code = struct.unpack_from("<H", img, off)[0]
        if code and img[off + 4] & 1:
            out.append(f"P{code:04X}")
    return tuple(sorted(out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--av", type=int, required=True, help="a FREE AV input, 1-16 (not 12, the battery)")
    ap.add_argument("--secs", type=float, default=600.0)
    ap.add_argument("--scope", action="store_true",
                    help="also log the DSO2D15's CH1 average (read-only query) — the pin's real voltage")
    ap.add_argument("--gen", action="store_true",
                    help="drive the pin from the DSO2D15's generator and run the scripted tests (implies --scope)")
    a = ap.parse_args()
    scope = None
    if a.scope or a.gen:
        sys.path.insert(0, "tools")
        from dso2d15 import Dso2d15
        scope = Dso2d15(settle=0.05)

    M, L = Meta(), TsLink()
    print(L.hello())
    arr = M.array("sensors", "sensor")
    F, T = arr["fields"], arr["tables"]["cal"]
    idx = arr["element_ids"].index("aux_1")
    base = arr["base_offset"] + idx * arr["stride"]
    saved = L.read_config_chunked(base, arr["stride"])
    multi = F["type"]["option_ids"].index("multi_switch")

    el = bytearray(saved)
    el[F["enabled"]["rel_offset"]] = 0                      # enable LAST, once the element is whole
    el[F["type"]["rel_offset"]] = multi
    el[F["interface"]["rel_offset"]] = F["interface"]["option_ids"].index("analog_voltage")
    struct.pack_into("b", el, F["source"]["rel_offset"], a.av - 1)
    el[F["diag_enable"]["rel_offset"]] = 0                  # the floor is forced on regardless
    struct.pack_into("<H", el, F["diag_severity"]["rel_offset"], 0x5)   # raw low / raw high = Low
    struct.pack_into("<H", el, F["diag_delay_ms"]["rel_offset"], 0)
    edges, pos = [], []
    for lo, hi, p in BANDS:
        edges += [counts(lo, True), counts(hi, False)]
        pos += [p, p]
    el[T["n"]["rel_offset"]] = len(edges)
    for k, (e, p) in enumerate(zip(edges, pos)):
        struct.pack_into("<H", el, T["axis"]["rel_offset"] + 2 * k, e)
        struct.pack_into("<h", el, T["value"]["rel_offset"] + 2 * k, p)
    L.write_raw(base, bytes(el))
    L.write_raw(base + F["enabled"]["rel_offset"], b"\x01")
    print(f"aux_1 = multi_switch on AV{a.av}, bands (counts) {list(zip(edges[::2], edges[1::2]))}")
    if a.gen:
        try:
            run_gen(L, M, scope, a.av)
        finally:
            scope.output(False)
            L.write_raw(base + F["enabled"]["rel_offset"], b"\x00")
            L.write_raw(base, saved)
            L.restore_script()
            print("generator off; restored aux_1 and the Lua slot")
        return
    L.set_script(LUA)

    print("   t(s)    pin V   position        5V-1  5V-2   active DTCs" + ("   [scope avg V]" if scope else ""))
    hw = f"hw_av{a.av}"
    t0, last = time.time(), None
    try:
        while time.time() - t0 < a.secs:
            f = L.telem_all()
            g = round(f["lua_gauge_1"] * M.t("lua_gauge_1")["scale"])
            s1 = int(f["sensor_supply_1"]); s2 = int(f["sensor_supply_2"])
            d = active_dtcs(L)
            state = (g, s1, s2, d)
            if state != last:
                v = f[hw] * FULL_V / FULL_COUNTS
                sv = ""
                if scope:
                    try:
                        sv = f"   [{float(scope.query(':MEASure:CHANnel1:ITEM? VAVG')):.3f}]"
                    except ValueError:
                        sv = "   [?]"
                print(f"{time.time() - t0:7.2f}   {v:5.2f}   {g:2d} {NAMES.get(g, '?'):9s}   "
                      f"{'OK' if s1 else 'FAULT':5s} {'OK' if s2 else 'FAULT':5s}  {' '.join(d) or '-'}{sv}",
                      flush=True)
                last = state
            time.sleep(0.02)
    except KeyboardInterrupt:
        pass
    finally:
        L.write_raw(base + F["enabled"]["rel_offset"], b"\x00")
        L.write_raw(base, saved)
        L.restore_script()
        print("restored aux_1 and the Lua slot")


def run_gen(L, M, g, av):
    """Scripted tests with the generator on the pin. Levels are in volts AT THE PIN (the generator's
    setting, confirmed on the scope); the ECU reads its AV inputs ~4 % low (board file: the MCP6004
    front end's resistor gain is 0.628 against the modelled 0.66), so every level is chosen well inside
    its band either way.

    Timing tests are one SHORT pulse of the level under test inside a LONG rest (a square wave with the
    duty set so), so each measures one thing: whether a pulse of that width is believed. The sensor is
    sampled at 50 Hz, so a pulse is guaranteed a sample only every 20 ms — widths are chosen clear of that
    quantisation: <=30 ms can never span the 40 ms settle, >=80 ms always does."""
    hw = f"hw_av{av}"
    G = [f"lua_gauge_{k}" for k in range(1, 7)]
    sc = {k: M.t(k)["scale"] for k in G}
    fails = []

    def check(ok, what, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {what}" + (f"   — {detail}" if detail else ""), flush=True)
        if not ok:
            fails.append(what)

    def snap():
        f = L.telem_all()
        return [round(f[k] * sc[k]) for k in G], f[hw] * FULL_V / FULL_COUNTS

    def window(secs):
        """Sample counts per outcome over `secs`: {invalid, REST, SET, RES, CANCEL}."""
        a, _ = snap(); time.sleep(secs); b, _ = snap()
        return dict(zip(["invalid", "REST", "SET", "RES", "CANCEL"], [y - x for x, y in zip(a[1:], b[1:])]))

    def dc(v):
        # A waveform the generator does not know is ignored silently and it keeps whatever it had — a
        # 1 Vpp sine would then ride on the "DC" level. So the type is read back, not assumed.
        g.burst_off(); g.wave("DC")
        if not g.query(":DDS:TYPE?").upper().startswith("DC"):
            raise SystemExit("generator refused the DC waveform")
        g.amp(0.01); g.offset(v); g.output(True)

    def pulses(base_v, pulse_v, pulse_ms, rest_ms=200.0):
        """`pulse_v` for `pulse_ms`, then `base_v` for `rest_ms`, repeating. Duty is the HIGH fraction."""
        lo, hi = min(base_v, pulse_v), max(base_v, pulse_v)
        t = (pulse_ms + rest_ms) / 1000.0
        high_ms = pulse_ms if pulse_v > base_v else rest_ms
        # The generator bounds |offset| + amplitude/2 by 3.5 V and REFUSES a setting that breaks it,
        # silently keeping the old one — so the amplitude is shrunk first, then the offset moved, then the
        # amplitude set, and all of it read back rather than assumed.
        duty = 100.0 * high_ms / (pulse_ms + rest_ms)
        g.burst_off(); g.wave("SQUAre"); g.freq(1.0 / t); g.write(f":DDS:DUTY {duty:.1f}")
        g.amp(0.01); g.offset((hi + lo) / 2.0); g.amp(hi - lo); g.output(True)
        got = (float(g.query(":DDS:AMP?")), float(g.query(":DDS:OFFSet?")), float(g.query(":DDS:DUTY?")))
        if abs(got[0] - (hi - lo)) > 0.01 or abs(got[1] - (hi + lo) / 2.0) > 0.01 or abs(got[2] - duty) > 1.0:
            raise SystemExit(f"generator refused the pulse settings: amp/offset/duty {got}")
        # …and it drops to 0 V and bursts for ~0.8 s after ANY change (traced on the bench) before the
        # new waveform is steady. The ECU is right to call that invalid; a test window must not include it.
        time.sleep(1.5)

    def scope_avg(want, tol=0.05, timeout=3.0):
        t0 = time.time()
        while True:
            try:
                v = float(g.query(":MEASure:CHANnel1:ITEM? VAVG"))
            except ValueError:
                v = float("nan")
            if abs(v - want) <= tol or time.time() - t0 >= timeout:
                return v
            time.sleep(0.1)

    found, found_duty = g.state(), g.query(":DDS:DUTY?")
    print("\nthe generator, as found:", found, " duty", found_duty)
    L.set_script(LUA_COUNT)
    dc(3.50); scope_avg(3.50); time.sleep(0.5)

    # ---- 1. DC levels: one per band, a gap, just above the floor, under it ----
    print("\n1. DC levels (pin volts from the scope; the ECU reads ~4 % low)")
    for v, want, what in [(3.50, 1, "REST"), (2.50, 2, "SET"), (1.50, 3, "RES"), (0.60, 4, "CANCEL"),
                          (2.00, -1, "a gap -> invalid"), (0.25, -1, "above the floor, in no band -> invalid"),
                          (4.50, -1, "above every band -> invalid"),
                          (0.10, -1, "under the floor -> invalid"), (3.50, 1, "REST again")]:
        dc(v)
        sv = scope_avg(v)
        if not abs(sv - v) <= 0.05:
            check(False, f"{v:.2f} V: the scope reads {sv:.3f} V at the pin", "generator range or wiring")
            continue
        time.sleep(1.0)                                    # the generator's change glitch, then settle + gap
        w = window(0.5)                                    # and it must STAY there, not flicker
        (p, *_), ev = snap()
        dt = [c for c in active_dtcs(L) if c.startswith("P13")]
        steady = (w["invalid"] == 0) if want >= 0 else (w["invalid"] > 0 and sum(w.values()) == w["invalid"])
        check(p == want and steady, f"{v:.2f} V (scope {sv:.3f}, ECU {ev:.3f}) -> {NAMES.get(p, p)}",
              what + (f"; DTCs {' '.join(dt) or 'none'}" if want < 0 else ""))

    # ---- 2. settle: a SET press shorter than the settle is never believed; a longer one always is ----
    print("\n2. settle time: SET (2.50 V) pulses inside REST (3.50 V), 200 ms of rest between")
    for ms, believed in [(10, False), (20, False), (30, False), (80, True), (150, True)]:
        dc(3.50); time.sleep(0.3)
        pulses(3.50, 2.50, ms)
        time.sleep(0.3)
        w = window(3.0)
        check((w["SET"] > 0) == believed and w["invalid"] == 0,
              f"{ms} ms SET pulse -> {'believed' if w['SET'] else 'never reported'}", str(w))

    # ---- 3. the gap allowance: crossing a gap holds the position; sitting in one is a fault ----
    print("\n3. gap allowance: pulses into the gap (3.00 V) inside REST (3.50 V)")
    for ms, faults in [(30, False), (60, False), (80, False), (150, True), (300, True)]:
        dc(3.50); time.sleep(0.3)
        pulses(3.50, 3.00, ms)
        time.sleep(0.3)
        w = window(3.0)
        check((w["invalid"] > 0) == faults and w["SET"] == 0,
              f"{ms} ms in the gap -> {'invalid' if w['invalid'] else 'REST held'}", str(w))

    # ---- 4. the floor: a dip under 0.20 V is a fault at once — no allowance, no position ----
    print("\n4. floor: 30 ms dips to 0.10 V inside REST (3.50 V)")
    dc(3.50); time.sleep(0.3)
    pulses(3.50, 0.10, 30)
    time.sleep(0.3)
    w = window(3.0)
    check(w["invalid"] > 0 and w["SET"] == w["RES"] == w["CANCEL"] == 0,
          "dips under the floor go invalid and never read as the positions they sweep through", str(w))

    dc(3.50)
    g.write(f":DDS:DUTY {found_duty}")                     # put back what the front panel had
    print(f"\n{'ALL PASS' if not fails else f'{len(fails)} FAILED: ' + '; '.join(fails)}")


if __name__ == "__main__":
    main()
