#!/usr/bin/env python3
"""Push the ECU's descriptor AND its studio layout to the SD card via OMNI_CMD_WRITE_FILE.
Called by `make flash` after the firmware is flashed and the ECU has rebooted.

  python3 tools/push_meta.py [--port /dev/ttyACMx]
                             [--meta shared/tuneit-meta.json] [--gui shared/dashboard.gui]

Two files, same idea: the board carries what a studio needs to talk to it, so any machine that
plugs in gets the right thing without hunting for a matching build.

  "<board> <layout_hash>.meta"  the descriptor, raw JSON + 4-byte CRC32 footer
  "<board> <layout_hash>.gui"   the studio layout, raw JSON + the same footer

Both plain, for the same reason: a card pulled and read by a person should hold what it says
it holds. MEASURED, not assumed — the link runs at ~139 KB/s (it is USB CDC, so the 115200 is
a fiction; the limit is the chunked request/response protocol and the card, not the wire), which
puts the 7.9 MB layout at about 56 s and the meta at about 13 s. Deflating the layout would cut
it to ~7 s and is worth revisiting if a layout ever has to go out over something slower — the
studio already vendors an inflater in stb_image.h — but 56 s does not buy a second format.

Both files are skipped when the card already holds a byte-identical copy, so a reflash that
does not move the layout_hash costs one size check and one read-back each.
"""
import argparse, json, pathlib, struct, sys, time, zlib
import serial
import serial.tools.list_ports

REPO = pathlib.Path(__file__).resolve().parent.parent

# Commands (host → ECU)
CMD_IDENTITY   = ord('Q')
CMD_FETCH_FILE = 0x24
CMD_WRITE_FILE = 0x25
CMD_SD_MCU     = 0x20
CMD_SD_MSC     = 0x21   # give the card to USB MSC (host reads it)
CMD_SD_RELEASE = 0x22

# Responses (ECU → host)
RSP_ACK        = 0x00
RSP_FILE_DATA  = 0x08
RSP_SD_STATUS  = 0x09
PACKET_IDENTITY = 0x07

# SD_STATE byte in RSP_SD_STATUS payload
SD_STATE_READY   = 0x00
SD_STATE_NO_CARD = 0x01
SD_STATE_BUSY    = 0x02

OMNI_FRAME_FIXED = 18     # header(16) + crc(2)
OMNI_CFG_SUBHDR  = 6      # config sub-header slack (matches firmware OMNI_MAX_PAYLOAD)
WRITE_OFFSET_LEN = 4      # offset field in write payload


def max_frame_for(block_size: int) -> int:
    """Largest omnidyno frame the firmware will assemble, derived from its block size.
    Mirrors OmniProtocol.h: OMNI_HEADER_SIZE(16) + (block_size + OMNI_CFG_SUBHDR) + OMNI_CRC_SIZE(2).
    Read block_size from the meta so this can never drift from the firmware's frame buffer."""
    return 16 + (block_size + OMNI_CFG_SUBHDR) + 2

ECU_VID = 0x0483
ECU_PID = 0x5740


# ---------------------------------------------------------------------------
# Wire framing
# ---------------------------------------------------------------------------

def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc


# Correlation id: each request stamps a fresh sequence into the frame; the firmware
# echoes it back in the reply, so transact() can match a response to its request and
# ignore anything stale or unsolicited. A logical request keeps its seq across retries.
_seq_counter = 0

def next_seq() -> int:
    global _seq_counter
    _seq_counter = (_seq_counter + 1) & 0xFFFF
    return _seq_counter


def build_frame(type_id: int, payload: bytes, seq: int = 0) -> bytes:
    total = 16 + len(payload) + 2
    header = (b'\xAA\x55' + bytes([type_id, 0])
              + struct.pack('<H', total)
              + struct.pack('<Q', 0)
              + struct.pack('<H', seq & 0xFFFF))
    raw = header + payload
    return raw + struct.pack('<H', crc16(raw))


def parse_frame(buf: bytes):
    """Find the first valid frame. Returns (type_id, seq, payload, end_offset) or None."""
    i = 0
    while i + 16 <= len(buf):
        if buf[i] != 0xAA or buf[i + 1] != 0x55:
            i += 1
            continue
        total = struct.unpack_from('<H', buf, i + 4)[0]
        if total < 18 or total > 8192:
            i += 1
            continue
        if i + total > len(buf):
            break
        frame = buf[i:i + total]
        if crc16(frame[:-2]) != struct.unpack_from('<H', frame, total - 2)[0]:
            i += 1
            continue
        seq = struct.unpack_from('<H', frame, 14)[0]
        return frame[2], seq, frame[16:total - 2], i + total
    return None


# ---------------------------------------------------------------------------
# Core transport: send a command, wait for the expected response type.
# Retries on timeout (lost packet or lost response — all commands are idempotent).
# Skips unsolicited PACKET_IDENTITY broadcasts unless we're explicitly expecting one.
# Returns the response payload bytes, or None if comms failed after all retries.
# ---------------------------------------------------------------------------

def transact(ser, cmd: int, payload: bytes = b'', expect: int = RSP_ACK,
             timeout: float = 3.0, retries: int = 3) -> bytes | None:
    seq = next_seq()                       # same correlation id for every retry of this request
    frame = build_frame(cmd, payload, seq)
    for attempt in range(retries):
        buf = bytearray()
        ser.write(frame)
        ser.flush()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            result = parse_frame(bytes(buf))
            if result:
                type_id, rsp_seq, rsp_payload, end = result
                del buf[:end]
                if rsp_seq != seq:
                    continue   # not our correlation id — stale/unsolicited, drop and keep scanning
                if type_id == PACKET_IDENTITY and expect != PACKET_IDENTITY:
                    continue   # unsolicited idle broadcast — ignore
                if type_id == expect:
                    return rsp_payload
                return None    # wrong response type — hard protocol error, don't retry
            # Grab the reply the instant it lands. read(1) blocks only until the first
            # byte (the real device latency, ~2 ms), then in_waiting drains the rest of
            # the frame without waiting. Reading a fixed 256 here would instead stall for
            # the whole port timeout on every small ACK — the ~200 ms/chunk that made a
            # full meta push take minutes. (pyserial read(N) waits for N bytes or timeout.)
            chunk = ser.read(1)
            if chunk:
                n = ser.in_waiting
                if n:
                    chunk += ser.read(n)
                buf.extend(chunk)
        # timeout — retry (resend)
    return None   # comms failed


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def get_identity(ser) -> dict | None:
    payload = transact(ser, CMD_IDENTITY, expect=PACKET_IDENTITY, timeout=3.0)
    if not payload:
        return None
    parts = payload.decode('latin1').strip().split()
    if len(parts) >= 6 and parts[0] == 'jayecu':
        return {'board': parts[1], 'fw_hash': parts[3], 'layout_hash': parts[4]}
    return None


def sd_mcu(ser) -> int:
    """Take SD card for ECU; returns SD_STATE_* from the ECU's synchronous card probe."""
    rsp = transact(ser, CMD_SD_MCU, expect=RSP_SD_STATUS, timeout=5.0)
    return rsp[0] if rsp and len(rsp) >= 1 else SD_STATE_BUSY


def release_sd(ser) -> None:
    transact(ser, CMD_SD_RELEASE, expect=RSP_ACK, timeout=1.0, retries=2)



def sd_msc(ser) -> None:
    """Hand the card to USB MSC so the HOST can read it — OMNI_CMD_SD_MSC (0x21).

    NOT the same as release_sd(): SD_RELEASE only clears the comms override and returns ownership to the
    key state, which on a powered bench leaves the ECU still holding the card. Reaching for release when
    you meant this makes it look like the MSC hand-off is broken — the host keeps seeing a 0-byte device
    while the ECU goes on serving files quite happily.
    """
    transact(ser, CMD_SD_MSC, b'', expect=RSP_ACK)

def sd_file_size(ser, name: str) -> int:
    """Return the file size on SD, or -1 if not present / card missing."""
    rsp = transact(ser, CMD_FETCH_FILE,
                   struct.pack('<IH', 0, 1) + name.encode(),
                   expect=RSP_FILE_DATA)
    if not rsp or len(rsp) < 4:
        return -1
    sz = struct.unpack_from('<I', rsp)[0]
    return -1 if sz == 0xFFFFFFFF else sz


# RSP_FILE_DATA payload: [file_size:u32][offset:u32][actual_len:u16][data...]
_FILE_HDR = 10

def fetch_file_full(ser, name: str, fetch_chunk: int) -> bytes | None:
    """Read an entire file from SD. Returns the raw bytes or None on error.
    fetch_chunk is the per-request read length — the firmware clamps it to its block size,
    so pass the meta's block_size and let it size the transfer to the frame buffer."""
    name_b = name.encode() + b'\x00'
    data = bytearray()
    file_size = None
    offset = 0
    while True:
        rsp = transact(ser, CMD_FETCH_FILE,
                       struct.pack('<IH', offset, fetch_chunk) + name_b,
                       expect=RSP_FILE_DATA, timeout=10.0)
        if not rsp or len(rsp) < _FILE_HDR:
            return None
        fsz    = struct.unpack_from('<I', rsp, 0)[0]
        actual = struct.unpack_from('<H', rsp, 8)[0]
        if fsz == 0xFFFFFFFF:
            return None   # not found / card busy
        if file_size is None:
            file_size = fsz
        data.extend(rsp[_FILE_HDR:_FILE_HDR + actual])
        offset += actual
        if actual == 0 or offset >= file_size:
            break
    return bytes(data)


def verify_meta_crc(data: bytes) -> bool:
    """Check the 4-byte LE CRC32 footer appended by codegen."""
    if len(data) < 4:
        return False
    body    = data[:-4]
    stored  = struct.unpack_from('<I', data, len(data) - 4)[0]
    return (zlib.crc32(body) & 0xFFFFFFFF) == stored


def write_file_chunked(ser, name: str, data: bytes, max_frame: int) -> bool:
    total = len(data)
    offset = 0
    name_b = name.encode() + b'\x00'
    chunk_size = max_frame - OMNI_FRAME_FIXED - WRITE_OFFSET_LEN - len(name_b)
    while offset < total:
        chunk = data[offset:offset + chunk_size]
        rsp = transact(ser, CMD_WRITE_FILE,
                       struct.pack('<I', offset) + name_b + chunk,
                       expect=RSP_ACK, timeout=15.0)
        if rsp is None:
            print(f"\n  comms failed at offset {offset}", file=sys.stderr)
            return False
        if len(rsp) < 6:
            print(f"\n  short ACK at offset {offset}", file=sys.stderr)
            return False
        written = struct.unpack_from('<H', rsp, 4)[0]
        if written == 0:
            print(f"\n  ECU wrote 0 bytes at offset {offset} — SD error", file=sys.stderr)
            return False
        offset += written
        print(f"  {name}: {offset * 100 // total:3d}%  ({offset:,}/{total:,})   ",
              end='\r', flush=True)
    print()
    return True


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def push_one(ser, name: str, data: bytes, block_size: int, max_frame: int,
             force: bool, label: str) -> bool:
    """Write one file to the card and prove it landed. Shared by the meta and the layout so the
    two cannot drift apart — the verify path is the whole point of this tool and having it once
    is the only way it stays the same for both."""
    existing = sd_file_size(ser, name)
    if existing == len(data) and not force:
        # Size matches — verify CRC of what's on card before declaring done.
        readback = fetch_file_full(ser, name, block_size)
        if readback and verify_meta_crc(readback) and readback == data:
            print(f"  SD card already has {name}")
            return True
        print(f"  {name} on SD failed CRC verify — re-pushing")
    elif existing >= 0:
        print(f"  {name} on SD is {existing}B, expected {len(data)}B — re-pushing")

    print(f"  writing {name} ({len(data):,} bytes)...")
    if not write_file_chunked(ser, name, data, max_frame):
        return False

    print(f"  verifying...", end='\r', flush=True)
    readback = fetch_file_full(ser, name, block_size)
    if readback is None:
        print(f"  verify failed: could not read back {name}", file=sys.stderr)
        return False
    if not verify_meta_crc(readback):
        print(f"  verify failed: CRC mismatch on {name} (SD write corruption)", file=sys.stderr)
        return False
    if readback != data:
        print(f"  verify failed: content mismatch on {name}", file=sys.stderr)
        return False
    print(f"  {label} pushed + verified ({len(data):,} bytes)")
    return True


def build_gui_payload(gui_path: pathlib.Path, meta_path) -> bytes | None:
    """Give the layout the same CRC envelope the meta uses. Nothing here judges whether the layout
    matches the schema, DELIBERATELY: widgets bind by path, so a layout drawn against an older
    schema is usually still correct, and where it is not the studio says so on load — it runs the
    real check (MetaModel::locate) and names every binding that no longer resolves.

    Refusing to push a layout whose builtFor has moved would be worse than shipping it. The file is
    named "<board> <layout_hash>.gui", so a moved hash is a NEW name: not pushing leaves the card
    with no layout under it at all, the studio falls back to a tree seeded from the meta, and the
    user gets a bare screen with nothing explaining why. A mostly-correct layout that announces its
    own staleness beats nothing, silently."""
    try:
        # Stamped with the meta it ships beside (tools/stamp_dashboard.py): builtFor names this layout
        # only if every binding resolves in it, so a stale one still announces itself on connect.
        import stamp_dashboard
        raw, _bad = stamp_dashboard.stamp(gui_path.read_bytes(), stamp_dashboard.load_meta(meta_path))
    except Exception as e:
        print(f"  {gui_path} is not valid JSON ({e}) — skipping layout push", file=sys.stderr)
        return None
    return raw + struct.pack('<I', zlib.crc32(raw) & 0xFFFFFFFF)


def find_ports() -> list[str]:
    return [p.device for p in serial.tools.list_ports.comports()
            if p.vid == ECU_VID and p.pid == ECU_PID]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port')
    ap.add_argument('--meta', default='shared/tuneit-meta.json')
    ap.add_argument('--gui',  default=None,
                    help="studio layout to ship (default: definition/boards/<board>.dashboard.gui, "
                         "'' to skip)")
    ap.add_argument('--force', action='store_true', help='re-push even if SD already has matching file')
    args = ap.parse_args()

    meta_path = pathlib.Path(args.meta)
    if not meta_path.exists():
        print(f"  meta not found: {meta_path}", file=sys.stderr)
        sys.exit(1)

    meta_data = meta_path.read_bytes()
    if not verify_meta_crc(meta_data):
        print(f"  meta file CRC invalid — run make codegen", file=sys.stderr)
        sys.exit(1)
    meta_json   = json.loads(meta_data[:-4])   # strip 4-byte footer before JSON parse
    local_board = meta_json['meta']['board']
    local_hash  = meta_json['meta']['layout_hash']

    # Block size is the single source of truth for chunking — read it from the meta so this
    # tool tracks the firmware automatically (it flows codegen BLOCK_SIZE -> meta -> firmware).
    block_size = int(meta_json['protocol']['framing']['block_size'])
    max_frame  = max_frame_for(block_size)

    port = args.port
    if not port:
        print("  waiting for ECU port...", end='\r', flush=True)
        for _ in range(20):
            ports = find_ports()
            if ports:
                port = ports[0]
                break
            time.sleep(1)
    if not port:
        print("  no ECU port found — skipping meta push", file=sys.stderr)
        sys.exit(0)

    print(f"  connecting {port}...", end='\r', flush=True)
    with serial.Serial(port, 115200, timeout=0.2) as ser:
        ident = get_identity(ser)
        if not ident:
            print("  no identity from ECU — skipping meta push", file=sys.stderr)
            sys.exit(0)

        if ident['board'] != local_board or ident['layout_hash'] != local_hash:
            print(f"  ECU {ident['board']} {ident['layout_hash']} != local {local_board} {local_hash} — skip")
            sys.exit(0)

        state = sd_mcu(ser)
        if state == SD_STATE_NO_CARD:
            print("  no SD card — skipping push", file=sys.stderr)
            sys.exit(0)
        if state != SD_STATE_READY:
            print(f"  SD unavailable (state=0x{state:02x}) — skipping push", file=sys.stderr)
            sys.exit(0)

        try:
            meta_name = f"{ident['board']} {ident['layout_hash']}.meta"
            if not push_one(ser, meta_name, meta_data, block_size, max_frame, args.force, 'meta'):
                sys.exit(1)

            # The layout ships with the firmware. A missing one is not an error — a tree seeded
            # from the meta is a working studio, just not a designed one.
            if args.gui == '':
                return
            # Per BOARD, not one shared file: the dashboard is authored per board (different pins,
            # different features, different bindings), and shared/ holds whichever board codegen
            # ran for last. Defaulting off the ECU's OWN board name means the layout that ships is
            # the one drawn for the board that is actually plugged in.
            gui_path = pathlib.Path(args.gui) if args.gui else (
                REPO / 'definition' / 'boards' / f"{ident['board']}.dashboard.gui")
            if not gui_path.exists():
                print(f"  no layout at {gui_path} — skipping layout push")
                return
            gui_data = build_gui_payload(gui_path, meta_path)
            if gui_data is None:
                return
            gui_name = f"{ident['board']} {ident['layout_hash']}.gui"
            if not push_one(ser, gui_name, gui_data, block_size, max_frame, args.force, 'layout'):
                sys.exit(1)
        finally:
            release_sd(ser)


if __name__ == '__main__':
    main()
