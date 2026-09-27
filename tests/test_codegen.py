#!/usr/bin/env python3
"""Verify codegen produces correct struct sizes and INI field offsets."""

import subprocess, sys, re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEN  = ROOT / "generated"
SHARED = ROOT / "shared"

def run_codegen():
    r = subprocess.run([sys.executable, str(ROOT / "codegen" / "codegen.py")],
                       capture_output=True, text=True)
    assert r.returncode == 0, f"codegen failed:\n{r.stderr}"

def check_static_asserts():
    """Compile generated headers on the host and let static_asserts fire."""
    check_src = """
#include <cstdint>
#include <cstring>
"""
    # Build list of headers to check
    headers = list(GEN.glob("modules/*.h")) + [
        GEN / "ecu_telemetry.h",
        GEN / "ecu_config.h",
        GEN / "schema_meta.h",
    ]
    for h in headers:
        check_src += f'#include "{h}"\n'
    check_src += "int main() { return 0; }\n"

    src_file = ROOT / "tests" / "_check_gen.cpp"
    src_file.write_text(check_src)
    r = subprocess.run(
        ["g++", "-std=c++17", "-Werror", "-I", str(GEN.parent / "generated"),
         "-I", str(GEN),
         str(src_file), "-o", "/dev/null"],
        capture_output=True, text=True
    )
    src_file.unlink(missing_ok=True)
    assert r.returncode == 0, f"static_assert compile check failed:\n{r.stderr}"

def parse_ini_offsets(ini_path):
    """Return dict of {field_name: offset} from [OutputChannels] and [Constants]."""
    offsets = {}
    text = ini_path.read_text()
    for line in text.splitlines():
        # match: name = scalar, TYPE, OFFSET, ...
        m = re.match(r'\s*(\w+)\s*=\s*(?:scalar|array),\s*\w+,\s*(\d+)', line)
        if m:
            offsets[m.group(1)] = int(m.group(2))
    return offsets

# Byte size of each scalar telemetry type (the wire layout is packed, no padding).
TYPE_SIZE = {
    "uint8": 1, "int8": 1, "bool": 1,
    "uint16": 2, "int16": 2,
    "uint32": 4, "int32": 4, "float": 4,
}

def load_dict():
    import json
    # The meta carries a trailing 4-byte little-endian CRC32 (see codegen write_bytes); strip it before
    # parsing, the same way MetaModel::loadFile does. read_text() would choke on those raw bytes.
    raw = (SHARED / "tuneit-meta.json").read_bytes()
    return json.loads(raw[:-4].decode())

def read_schema_meta():
    """Parse the JAYECU_* size literals out of generated schema_meta.h."""
    text = (GEN / "schema_meta.h").read_text()
    return {m.group(1): int(m.group(2))
            for m in re.finditer(r'#define\s+(JAYECU_\w+)\s+(\d+)u?', text)}

def channel_size(ch):
    assert ch["type"] in TYPE_SIZE, \
        f"unknown telemetry type {ch['type']!r} for '{ch['name']}' — add it to TYPE_SIZE"
    return TYPE_SIZE[ch["type"]]

def test_telemetry_offsets_contiguous():
    """Telemetry channels pack tightly (no gaps/overlaps) in the resolved dictionary —
    the single app-facing contract, whose offsets are the same ones the C++ structs use."""
    d = load_dict()
    expected = 0
    for name, ch in sorted(d["telemetry"].items(), key=lambda kv: kv[1]["offset"]):
        assert ch["offset"] == expected, (
            f"telemetry '{name}' at offset {ch['offset']}, expected {expected} (gap/overlap)")
        expected += ch["size"]
    assert expected == d["meta"]["telemetry_size"], (
        f"packed telemetry size {expected} != dict telemetry_size {d['meta']['telemetry_size']}")

def test_sizes_consistent():
    """Sizes agree between the resolved dictionary and schema_meta.h — independent emitters,
    so a mismatch means a generator bug."""
    d = load_dict()
    meta = read_schema_meta()
    assert meta["JAYECU_TELEMETRY_SIZE"] == d["meta"]["telemetry_size"], (
        f"schema_meta TELEMETRY_SIZE {meta['JAYECU_TELEMETRY_SIZE']} != {d['meta']['telemetry_size']}")
    assert meta["JAYECU_CONFIG_SIZE"] == d["meta"]["config_size"], (
        f"schema_meta CONFIG_SIZE {meta['JAYECU_CONFIG_SIZE']} != {d['meta']['config_size']}")

def test_streams_capture_index_is_raw_board_index():
    """Capture inputs are bound by streams[].capture_index — a RAW index into the board
    capture pool (s_capture_resources[], itself GENERATED from the board schema). It is
    NOT an enum of pin labels duplicated into the tune schema: the labels are HARDWARE,
    owned by the board schema and surfaced by the studio. After the generic-trigger
    cleanup there is no crank/cam-specific capture config left, so the VR1-vs-DIG1
    hand-drift trap can't recur. nominal_angle rides on the (cam-rate) stream for VVT."""
    import yaml
    board = yaml.safe_load(
        (ROOT / "definition" / "boards" / "jaytek_v1.board.yaml").read_text())
    capture_pool = [p["signal"] for p in board["pins"]
                    if "TRIGGER_INPUT" in p.get("caps", [])]
    assert capture_pool, "no TRIGGER_INPUT pins in board schema"

    schema = yaml.safe_load((ROOT / "definition" / "ecu.schema.yaml").read_text())
    # Modules now carry the nested `config: {scalars, tables, arrays}` form; flatten it to
    # the internal flat keys (config / config_arrays) the way codegen does before reading.
    sys.path.insert(0, str(ROOT / "codegen"))
    import codegen
    codegen.normalize_module_config(schema)
    trig = schema["modules"]["Trigger"]

    # The dead crank/cam-specific binding fields must be gone (binding is via streams[]).
    cfg_names = {f["name"] for f in trig["config"]}
    for dead in ("crank_capture_index", "crank_edge", "crank_used", "cam_count",
                 "strategy", "physical_teeth", "sync_window_pct"):
        assert dead not in cfg_names, f"{dead} should have been removed (legacy decoder is gone)"
    arr_names = {a["name"] for a in trig.get("config_arrays", [])}
    for dead in ("cam_inputs", "cam_pattern", "sync_patterns", "anomaly_table", "pattern_teeth"):
        assert dead not in arr_names, f"{dead} should have been removed (generic streams[] only)"

    streams = next(a for a in trig["config_arrays"] if a["name"] == "streams")
    cap = next(e for e in streams["element"] if e["name"] == "capture_index")
    assert cap.get("type") == "uint8", "streams capture_index must be a raw board-pool index (uint8)"
    # each stream defaults to a DISTINCT pool pin (0,1,2,3…) so the bits dropdown holds a clean
    # in-range value; the firmware pin arbiter prevents any pin being handed out twice.
    dflt = cap.get("default")
    assert isinstance(dflt, list) and dflt[:4] == [0, 1, 2, 3], \
        "streams capture_index must default per-stream to distinct pool pins [0,1,2,3]"
    assert all(0 <= d < len(capture_pool) for d in dflt), "each default must be a valid pool index"
    assert any(e["name"] == "nominal_angle" for e in streams["element"]), \
        "streams must carry nominal_angle (cam/VVT)"

def test_can_bit_addressing_matches_the_spec_notation():
    """The three address forms a published CAN spec uses, parsed the way the document means them.

    This is the piece the whole protocol library rests on: forty frames get transcribed by COPYING
    addresses out of a PDF, so the parser has to agree with the notation exactly. Bit 7 is the most
    significant in a byte, and bits are numbered MSB-first across the frame.
    """
    sys.path.insert(0, str(ROOT / "codegen"))
    from codegen import _parse_can_bits as P

    assert P("0-1")     == (7, 16),  P("0-1")      # two whole bytes, opening at byte 0's MSB
    assert P("2-3")     == (23, 16), P("2-3")
    assert P("6-7")     == (55, 16), P("6-7")
    assert P("4")       == (39, 8),  P("4")        # one whole byte
    assert P("0-3")     == (7, 32),  P("0-3")      # a 4-byte field
    assert P("2:5")     == (21, 1),  P("2:5")      # a single bit
    assert P("1:7")     == (15, 1),  P("1:7")      # MSB of byte 1
    assert P("1:0")     == (8, 1),   P("1:0")      # LSB of byte 1
    assert P("6:3-7:0") == (51, 12), P("6:3-7:0")  # a 12-bit field spanning a byte

    # The spec's own worked example puts a 12-bit throttle at 2:3 - 3:0.
    assert P("2:3-3:0") == (19, 12), P("2:3-3:0")

    # A backwards range is a transcription slip, and must be refused rather than producing a
    # plausible-looking width. SystemExit is codegen's own failure mode.
    try:
        P("3-1")
        assert False, "a backwards bit range should be refused"
    except SystemExit:
        pass


def test_can_templates_are_published_and_sane():
    """Every published template names real signals and lays its fields inside its own frames.

    The templates are the protocol library now — they are not compiled in, so nothing but this check
    stands between a stale signal name and a field that transmits zero for ever on somebody's car.
    """
    import json
    tdir = ROOT / "shared" / "can_templates"
    if not tdir.is_dir():
        return                                    # no templates in this tree
    ids = (GEN / "signal_ids.h").read_text()
    seen = 0
    for path in sorted(tdir.glob("*.json")):
        t = json.loads(path.read_text())
        assert t.get("frames"), f"{path.name} publishes no frames"
        for m in t["frames"]:
            limit = 0x1FFFFFFF if m.get("ext") else 0x7FF
            assert 0 <= m["id"] <= limit, f"{path.name}: id 0x{m['id']:X} too large for its id type"
            assert 0 <= m["dlc"] <= 8, f"{path.name}: frame 0x{m['id']:X} dlc out of range"
            # A transmit template with a zero period would never be scheduled.
            if t.get("direction", "transmit") == "transmit":
                assert m["period_ms"] > 0, f"{path.name}: frame 0x{m['id']:X} has no period"
            for f in m["fields"]:
                # Templates are stored normalised — the spec's `bits` notation is resolved at install.
                assert "bits" not in f, f"{path.name}: '{f['sig']}' was not normalised"
                # A null channel is not a mistake: it is a field a SENSOR consumes, so the sensor
                # publishes it with its own calibration and diagnostics instead of the field writing
                # the channel underneath.
                if f.get("sig") is not None:
                    sig = "SIG_" + f["sig"].upper()
                    assert re.search(rf"\b{sig}\s*=", ids), f"{path.name}: {sig} is not a real signal"
                assert 1 <= f["width"] <= 32, f"{path.name}: '{f['sig']}' width out of range"
                # Big-endian fields run down from their start bit into the next byte, so the
                # extent is a walk, not an addition.
                off, w = f["bit_off"], f["width"]
                last = ((off + w - 1) // 8) if (f.get("flags", 0) & 2) \
                       else (off // 8 + ((7 - off % 8) + w - 1) // 8)
                assert last < m["dlc"], \
                    f"{path.name}: '{f['sig']}' runs past the end of frame 0x{m['id']:X}"
                seen += 1
    assert seen > 0, "templates published but not one field was checked"


def test_signal_ids_match_the_lock():
    """Every SignalId is the one definition/signal_ids.<board>.lock pins it to.

    A tune stores kind:"signal" selectors as this raw number, and layout_hash cannot notice them
    moving: it hashes the config BYTE layout, and renumbering the catalog moves no bytes. Adding
    `app_state` mid-schema once shifted 179 ids, which re-pointed a saved tune's half-bridge enable
    from etb_en_1 to etb_en_2 — a throttle that would not move, reading exactly like dead hardware.
    So the lock is the contract, and this asserts codegen honoured it.
    """
    import json
    # The lock is PER BOARD (definition/signal_ids.<board>.lock): the catalog is partly board-derived.
    d = load_dict()
    lock = json.loads((ROOT / "definition" / f"signal_ids.{d['meta']['board']}.lock").read_text())["ids"]
    live = d["signals"]

    # The lock is authoritative for every signal the meta publishes.
    for name, sid in live.items():
        assert name in lock, f"signal '{name}' has no locked id — codegen must append it to the lock"
        assert lock[name] == sid, \
            f"signal '{name}' is id {sid} but locked to {lock[name]} — ids must never move"

    # Contiguous 0..N-1: SignalId indexes SIGNAL_NAMES[]/SIGNAL_TYPES[] directly, so a hole would
    # desynchronise index and id everywhere downstream.
    ids = sorted(lock.values())
    assert ids == list(range(len(ids))), "locked signal ids must be contiguous from 0"

    # And the generated enum agrees with both.
    text = (GEN / "signal_ids.h").read_text()
    for name, sid in list(live.items())[:40]:
        assert re.search(rf"\bSIG_{name.upper()}\s*=\s*{sid}\b", text), \
            f"signal_ids.h disagrees: SIG_{name.upper()} should be {sid}"


def test_every_firmware_dtc_has_a_description():
    """Every P-code the firmware declares must have hover text in the studio's DTC dock.

    Sensor and module codes get theirs generated from the catalog, so they cannot drift. Codes raised
    by firmware LOGIC -- protection trips, config errors, throttle plate faults -- declare their
    constant at the raise site instead, and for a long time nothing tied the two together: 14 of the 17
    were unlabelled, including P0234 overboost and P0217 coolant over-temp, both level 3. The dock
    showed a bare number and the technician got to guess which of those was the one that just cut the
    engine. definition/ecu.schema.yaml `firmware_dtc` supplies the text; this asserts the pairing.
    """
    desc = load_dict()["dtc_descriptions"]

    # Constants named P_* / PCODE_* whose value is a literal P-code (the nibbles ARE the digits).
    pat = re.compile(r"constexpr\s+uint16_t\s+(P_\w+|PCODE_\w+)\s*=\s*0x([0-9A-Fa-f]{3,4})\s*;")
    missing = []
    for src in sorted((ROOT / "firmware").rglob("*")):
        if src.suffix not in (".cpp", ".h"):
            continue
        for n, line in enumerate(src.read_text(errors="replace").splitlines(), 1):
            m = pat.search(line)
            if not m:
                continue
            code = f"P{int(m.group(2), 16):04X}"
            if code not in desc:
                missing.append(f"{code} ({m.group(1)}) at {src.relative_to(ROOT)}:{n}")

    assert not missing, (
        "firmware-raised DTCs with no studio description — add them to `firmware_dtc` in "
        "definition/ecu.schema.yaml:\n  " + "\n  ".join(missing))


if __name__ == "__main__":
    tests = [run_codegen, test_telemetry_offsets_contiguous,
             test_sizes_consistent, test_streams_capture_index_is_raw_board_index,
             test_can_bit_addressing_matches_the_spec_notation,
             test_can_templates_are_published_and_sane,
             test_signal_ids_match_the_lock,
             test_every_firmware_dtc_has_a_description]
    passed = failed = 0
    for t in tests:
        try:
            t()
            print(f"  PASS  {t.__name__}")
            passed += 1
        except AssertionError as e:
            print(f"  FAIL  {t.__name__}: {e}")
            failed += 1
        except Exception as e:
            print(f"  ERROR {t.__name__}: {e}")
            failed += 1

    # check_static_asserts requires g++ — skip gracefully if not available
    try:
        check_static_asserts()
        print("  PASS  check_static_asserts")
        passed += 1
    except FileNotFoundError:
        print("  SKIP  check_static_asserts (g++ not found)")
    except AssertionError as e:
        print(f"  FAIL  check_static_asserts: {e}")
        failed += 1

    print(f"\n{passed} passed, {failed} failed")
    sys.exit(0 if failed == 0 else 1)
