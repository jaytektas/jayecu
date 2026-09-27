#!/usr/bin/env python3
"""THE LUA EDITOR, AGAINST REAL FIRMWARE.

The studio's editor no longer names a config field. It asks the loaded definition which one holds the
script (`lua.source` here) and takes its capacity from the same place
(apps/studio-jf/src/model/Cache.cpp, scriptPath/scriptCapacity). test_script_path proves it resolves
the right name out of the definition. That is a statement about a JSON file.

This is the other half: does the field it resolves actually hold the script ON THE ECU? A wrong
answer there does not look like an error — setConfigString writes bytes somewhere, the studio says
"applied", and the engine runs what it was already running.

So: write a script through the resolved field, read the bytes back off the ECU, and check the engine
RAN what was written — plus the capacity, at the last byte, which is where an off-by-one in the cap
hides (it eats the final character of a full script and nothing says so).

    python3 tools/bench_lua_editor.py

Needs: the ECU on USB, and the studio NOT connected to it (it holds the port).
"""
import argparse
import glob
import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ROOT = Path(__file__).resolve().parents[1]
# The studio's own resolver order, copied from kScriptPaths (Cache.cpp).
SCRIPT_PATHS = ["lua.source", "ts.luaScript"]

fails = []


def ck(ok, what, note=""):
    print(f"  {what:<62} {'PASS' if ok else 'FAIL'}{'' if ok or not note else '  (' + str(note) + ')'}")
    if not ok:
        fails.append(what)
    return ok


def load_meta(path):
    raw = Path(path).read_bytes()
    d, _ = json.JSONDecoder().raw_decode(raw.decode("utf-8", "surrogateescape"))
    return d


def resolve_script_field(meta):
    """What Cache::scriptPath()/scriptCapacity() would answer for this definition: the first path in
    the studio's order that resolves to a string field bigger than one byte, and its capacity."""
    for path in SCRIPT_PATHS:
        mod, _, field = path.partition(".")
        entry = (meta.get("config", {}).get(mod) or {}).get(field)
        if not entry:
            continue
        size = int(entry.get("size") or entry.get("length") or 0)
        if size > 1:
            cap = int(entry.get("maxChars") or 0) or size - 1
            return path, int(entry["offset"]), size, cap
    return None, 0, 0, 0


# ----------------------------------------------------------------------------- jayecu (native link)
def bench_jayecu(port):
    from ts_bench import TsLink

    print(f"\n=== jayecu, native link on {port} ===")
    meta = load_meta(ROOT / "shared/tuneit-meta.json")
    path, off, size, cap = resolve_script_field(meta)
    ck(path == "lua.source", "the studio's resolver picks this ECU's own field", path)
    ck(cap == size - 1, "…and its cap is the field less its terminator", f"{cap} of {size}")

    link = TsLink(port=port)
    try:
        ident = link.hello()
        print(f"  {ident}")
        ck(ident.split()[0] == "jayecu", "the ECU on the other end is a jayecu", ident)

        # A script the ECU can be SEEN to be running: a gauge channel is a signal like any other, so
        # if the write landed in the script field and the engine reloaded it, this number appears.
        MARK = 42.5
        script = f'function onTick()\n  signalWrite("lua_gauge_1", {MARK}, 500)\nend\n'
        ok, dt = link.set_script_wait(script, expect={"lua_gauge_1": MARK}, tol=0.05, timeout=5.0)
        ck(ok, "the ECU RAN the script that was written", f"lua_gauge_1 after {dt * 1000:.0f} ms")
        ck(int(link.telem("lua_state")) == 0, "…with no load or runtime error",
           f"lua_state={int(link.telem('lua_state'))}")

        back = link.read_config_chunked(off, size, chunk=200).split(b"\x00", 1)[0].decode("ascii", "ignore")
        ck(back == script, "…and the field reads back byte for byte", f"{len(back)} of {len(script)} chars")

        # THE LAST BYTE OF A FULL SCRIPT. The cap is the one number the editor takes from the
        # definition and enforces itself, so a cap one too large silently loses the final character
        # to the terminator — a truncation that only shows up in a script long enough to hit it.
        tail = "-- Z"
        rem = cap - len(script) - len(tail)          # what a comment line has to fill exactly
        pad = "-- " + "x" * (rem - 4) + "\n"         # "-- " + xs + newline == rem
        full = script + pad + tail
        assert len(full) == cap, f"{len(full)} != {cap}"
        link.set_script(full)
        time.sleep(0.2)
        back = link.read_config_chunked(off, size, chunk=200).split(b"\x00", 1)[0].decode("ascii", "ignore")
        ck(len(back) == cap, "a script filling the field exactly survives", f"{len(back)} of {cap} chars")
        ck(back.endswith(tail), "…including its LAST character", repr(back[-6:]))
        ck(int(link.telem("lua_state")) == 0, "…and the ECU still loads it", f"lua_state={int(link.telem('lua_state'))}")
    finally:
        if link.restore_script():
            time.sleep(0.3)
            print(f"  restored the script that was on the ECU (lua_state={int(link.telem('lua_state'))})")
        link.close()


def looks_like(port):
    """True when a jayecu answers on this port."""
    try:
        from ts_bench import TsLink
        link = TsLink(port=port, timeout=0.6)
        try:
            ident = link.hello()
            return bool(ident) and ident.split()[0] == "jayecu"
        finally:
            link.close()
    except Exception:
        pass
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", action="append", default=None,
                    help="a port to test (repeatable); default: probe every /dev/ttyACM*")
    args = ap.parse_args()

    ports = args.port or sorted(glob.glob("/dev/ttyACM*"))
    if not ports:
        print("no /dev/ttyACM* — plug an ECU in")
        return 1

    port = None
    for p in ports:
        ok = looks_like(p)
        print(f"{p}: {'jayecu' if ok else 'no ECU answered'}")
        if ok and port is None:
            port = p
    if port is None:
        print("\nno jayecu found")
        return 1

    bench_jayecu(port)

    if fails:
        print(f"\nFAILED: {', '.join(fails)}")
        return 1
    print("\nall passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
