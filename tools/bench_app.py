#!/usr/bin/env python3
"""Live bench validation of the APP (accelerator pedal) module.

The APP sensors are HARDWIRED to this module: app_1 IS the pedal value, app_2 cross-checks it. There is
no selector, so there is nothing to mis-point — but that also means every failure mode is the module's
own, and each one ends in the same place: pedal_demand pinned to 0, which leaves the engine idling
rather than opening the throttle on bad data.

What this proves on hardware:
  1. app_1 drives pedal_demand through the pedal->throttle table
  2. app_2 disagreeing past the debounce forces demand to 0 (fail safe)
  3. agreement restores demand
  4. app_1 going invalid (stale TTL) forces demand to 0
  5. pedalcal REJECTS a calibration whose captured span is too small, and reports FAIL

NOT covered here: pedalcal's success path. It reads the raw ADC of the physical pins, which Lua
injection cannot reach (signalWrite writes the value bus, not the converter), so proving it needs a real
pedal or a pot across the APP inputs. Item 5 is the guard that stops a bad cal being written.

  python3 tools/bench_app.py [-v]
"""
import struct
import sys
import time
import pathlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from ts_bench import TsLink

TTL_MS = 500
fails = 0


def check(ok, what, detail=""):
    global fails
    print(f"  [{'PASS' if ok else 'FAIL'}] {what}{('  — ' + detail) if detail else ''}")
    if not ok:
        fails += 1


def scaled(link, name):
    return link.telem(name) * link.meta.t(name)["scale"]


def hold(link, a, b, secs=1.2):
    """Hold app_1 / app_2 at given percentages."""
    link.set_script(
        f'function onTick()\n'
        f'  signalWrite("app_1", {a}, {TTL_MS})\n'
        f'  signalWrite("app_2", {b}, {TTL_MS})\n'
        f'end\nsetTickRate(50)\n')
    time.sleep(secs)


def main():
    link = TsLink(verbose="-v" in sys.argv)
    print("signature:", link.hello())

    saved = {k: link.get_config(k) for k in ("app_enabled", "app_match_err_pct", "app_match_ms")}
    saved_script = link.get_script()
    print(f"saved config: {saved}")

    # The SHIPPED pedal->throttle table is all zeros (schema default: 0), so out of the box the pedal
    # commands no throttle at all. That is a safe default for drive-by-wire, but it means demand can only
    # be exercised against a map we install ourselves. Save the bytes, write a 1:1 pedal->throttle ramp,
    # and put the original back on the way out — RAM only, never burned.
    tbl = link.meta.c("app_pedal_to_throttle_table")
    saved_table = link.read_config_chunked(tbl["offset"], tbl["size"])
    x_pct = [0, 5, 10, 15, 20, 30, 40, 50, 60, 70, 80, 85, 90, 95, 98, 100]   # the x axis, from the schema
    cols = tbl["cols"]
    cells = tbl["size"] // 2
    ramp = b"".join(struct.pack("<H", int(x_pct[i % cols] * 10)) for i in range(cells))

    try:
        for off in range(0, len(ramp), 200):          # chunked: one frame per slice
            link.write_raw(tbl["offset"] + off, ramp[off:off + 200])
        time.sleep(0.3)
        print(f"installed a 1:1 pedal map ({cells} cells)")

        link.set_config("app_match_err_pct", 50)     # 5.0 % (scale 0.1)
        link.set_config("app_match_ms", 200)
        link.set_config("app_enabled", 1)
        time.sleep(0.3)

        print("\n--- pedal at 0 %, sensors agreeing ---")
        hold(link, 0, 0)
        d0 = scaled(link, "pedal_demand")
        print(f"  app_1={scaled(link,'app_1'):.1f}  app_2={scaled(link,'app_2'):.1f}  demand={d0:.1f}")

        print("\n--- pedal at 40 %, sensors agreeing ---")
        hold(link, 40, 40)
        a1, b1, d1 = scaled(link, "app_1"), scaled(link, "app_2"), scaled(link, "pedal_demand")
        print(f"  app_1={a1:.1f}  app_2={b1:.1f}  demand={d1:.1f}")
        check(abs(a1 - 40) < 2.0, "app_1 reads the injected pedal position", f"{a1:.1f} %")
        check(d1 > d0, "pedal_demand follows the pedal through the table", f"{d0:.1f} -> {d1:.1f}")

        print("\n--- pedal at 80 % ---")
        hold(link, 80, 80)
        d2 = scaled(link, "pedal_demand")
        print(f"  demand={d2:.1f}")
        check(d2 > d1, "…and keeps following it", f"{d1:.1f} -> {d2:.1f}")

        print("\n--- app_2 disagrees (80 vs 10) past the debounce ---")
        hold(link, 80, 10, secs=1.5)
        d3 = scaled(link, "pedal_demand")
        print(f"  app_1={scaled(link,'app_1'):.1f}  app_2={scaled(link,'app_2'):.1f}  demand={d3:.1f}")
        check(d3 == 0.0, "a correlation fault pins demand to 0 (engine keeps idling)", f"demand={d3:.1f}")

        print("\n--- sensors agree again ---")
        hold(link, 80, 80, secs=1.5)
        d4 = scaled(link, "pedal_demand")
        print(f"  demand={d4:.1f}")
        check(d4 > 0.0, "demand is restored once they agree", f"demand={d4:.1f}")

        print("\n--- app_1 with NOTHING publishing it: the no-signal fail-safe ---")
        # STOPPING THE INJECTION IS NOT LOSING THE SIGNAL. app_1 is a real configured sensor on this
        # rig, so when the Lua write's TTL expires the sensor simply resumes publishing — an unwired
        # input reading ~0.1 %, perfectly valid. The old check stopped the script and asserted demand
        # == 0; it got 0.1, and called the fail-safe broken when what it had actually built was a
        # pedal at rest. To test "no signal" the sensor has to go, not the injection.
        link.set_script("function onTick()\nend\nsetTickRate(50)\n")
        time.sleep(1.0)
        SA = link.meta.config["sensors"]["sensor"]
        a1 = SA["base_offset"] + SA["element_ids"].index("app_1") * SA["stride"] \
             + SA["fields"]["enabled"]["rel_offset"]
        a1_was = link.read_config_chunked(a1, 1, chunk=8)
        link.write_raw(a1, b"\x00")
        time.sleep(2.0)
        d5 = scaled(link, "pedal_demand")
        st = scaled(link, "app_state")
        print(f"  demand={d5:.1f}  app_state={st:.0f}")
        check(d5 == 0.0, "with no app_1 at all, demand is pinned to 0", f"demand={d5:.1f}")
        check(st == 3.0, "…and the module says WHICH fault it is (3 = no signal)", f"app_state={st:.0f}")
        link.write_raw(a1, a1_was)
        time.sleep(1.0)
        check(scaled(link, "app_state") == 0.0, "…and it recovers when the sensor comes back",
              f"app_state={scaled(link, 'app_state'):.0f}")

        print("\n--- pedalcal with no pedal wired: the span guard must REJECT it ---")
        # cmdstate: the studio watches command_state until OK/FAIL. Nothing is moving on the physical
        # APP pins, so the captured span is ~0 and the calibration must be refused, not written.
        # command_state is (op << 2) | phase, LEVEL-valued: RUNNING for the whole routine, then OK/FAIL.
        PEDALCAL, RUNNING, OK, FAIL = 7, 1, 2, 3
        running, ok, fail = (PEDALCAL << 2) | RUNNING, (PEDALCAL << 2) | OK, (PEDALCAL << 2) | FAIL
        out = link.execute("pedalcal")
        print(f"  pedalcal -> {out.strip()!r}")
        t0, state = time.time(), None
        while time.time() - t0 < 10.0:
            state = link.telem("command_state")
            if state in (ok, fail):
                break
            time.sleep(0.4)
        names = {running: "RUNNING", ok: "OK", fail: "FAIL"}
        print(f"  command_state settled at {state} ({names.get(state, '?')})")
        check(state == fail, "a cal with no pedal movement is REJECTED, not written",
              f"want {fail} (PEDALCAL FAIL), got {state}")

    finally:
        link.set_script(saved_script if saved_script.strip() else "function onTick()\nend\n")
        for off in range(0, len(saved_table), 200):
            link.write_raw(tbl["offset"] + off, saved_table[off:off + 200])
        for k, v in saved.items():
            link.set_config(k, v)
        print(f"\nrestored config + original pedal map: {saved}")
        link.close()

    print(f"\n[app] {'FAILURES' if fails else 'all passed'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
