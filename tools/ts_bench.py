#!/usr/bin/env python3
"""Bench client for jayecu — driven entirely by shared/tuneit-meta.json.

Speaks the CURRENT Omnidyno wire framing the firmware (OmniProtocol.h / CommsManager) implements —
mirrors the studio's EcuLink.cpp:

    frame: [0xAA 0x55][typeId:1][reserved:1][length:u16 LE][timestamp:u64 LE][sequence:u16 LE][payload…][crc16:2 LE]
    length = TOTAL frame bytes (16-byte header + payload + 2-byte CRC).
    CRC16-CCITT (poly 0x1021, init 0xFFFF) over every byte from the first sync up to (not incl.) the CRC.
    typeId carries the command (request) or response code; sequence is echoed back by the reply.

Config access is a FLAT 32-bit g_config offset space (no pages / no 64 KB limit). Config command payload = [offset:u32 LE][size:u16 LE]([data] for write). Field byte offsets and
telemetry layout resolve from the Data Dictionary (tuneit-meta.json). Read-only by default; writes/burns
are explicit methods.
"""
import json, struct, zlib, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TUNEIT_META = ROOT / "shared" / "tuneit-meta.json"

# tuneit-meta datatype tags -> (struct fmt, byte size). ASCII strings carry their own size.
_FMT = {"U08": "<B", "S08": "<b", "U16": "<H", "S16": "<h",
        "U32": "<I", "S32": "<i", "F32": "<f"}
_SZ  = {"U08": 1, "S08": 1, "U16": 2, "S16": 2, "U32": 4, "S32": 4, "F32": 4}


def crc32(b: bytes) -> int:
    return zlib.crc32(b) & 0xFFFFFFFF


def crc16ccitt(b: bytes) -> int:
    """CRC16-CCITT (poly 0x1021, init 0xFFFF) — matches OmniProtocol.h omni_crc16."""
    crc = 0xFFFF
    for byte in b:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


OMNI_SYNC = b"\xAA\x55"
OMNI_HDR  = 16          # [AA 55][type][rsv][len:2][ts:8][seq:2]


class Meta:
    """The resolved Data Dictionary (shared/tuneit-meta.json) — the single source of truth for
    config offsets, telemetry layout, and the wire pages. Bench tools look offsets up here by name
    instead of re-deriving them from schema structure (which rots silently when the schema is
    reshaped — exactly how gen_rebench once broke). Change the schema, regen, and consumers follow."""
    def __init__(self, path=TUNEIT_META):
        # The shared meta carries a 4-byte CRC trailer after the JSON, so json.loads cannot be given
        # the whole buffer. This used to cut at the LAST '}' — which is a coin flip, not a rule: the
        # trailer is four arbitrary bytes and one of them is 0x7d about six percent of the time.
        # When that happens the slice keeps a stray CRC byte and the parse dies with "invalid start
        # byte" a few bytes from the end, pointing at the meta and blaming nothing that is wrong.
        #
        # Parse from the FRONT instead: raw_decode reads exactly one JSON value and reports where it
        # ended, so anything after it — this trailer, a longer one, none at all — is simply not read.
        # surrogateescape because the trailer is not text and must survive the decode to be ignored.
        raw = Path(path).read_bytes()
        d, _end = json.JSONDecoder().raw_decode(raw.decode("utf-8", "surrogateescape"))
        self.meta        = d["meta"]
        self.config      = d["config"]                 # {module: {field: {...}}}
        self.telem       = d["telemetry"]              # {name: {offset, datatype, size, ...}}
        self.signals     = d.get("signals", {})        # {channel name: SignalId} — the selector a
                                                       # precondition program bakes (stored = id + 1)
        self.config_size = self.meta["config_size"]
        self.telem_size  = self.meta["telemetry_size"]
        self.layout_hash = self.meta["layout_hash"]
        # Flat field-name index across all modules. The bench addresses config by the codegen
        # const_name = "<module>_<field>" (e.g. "engine_cylinder_count", "engine_protection_batt_min_mv"),
        # globally unique by construction — the same names the firmware's .ini constants used.
        self._flat = {}
        for mod, fields in self.config.items():
            for fname, entry in fields.items():
                if entry.get("type") == "struct_array":
                    continue                            # arrays go through array_offset(), not flat
                key = f"{mod}_{fname}"
                if key in self._flat:
                    raise KeyError(f"duplicate config constant {key!r}")
                self._flat[key] = (mod, entry)

    # ---- config lookups ----------------------------------------------------
    def c(self, name: str) -> dict:
        """A scalar/table config field entry by bare name: {offset, datatype, size, ...}."""
        return self._flat[name][1]

    def field(self, module: str, name: str) -> dict:
        return self.config[module][name]

    def array(self, module: str, name: str) -> dict:
        """A struct-array entry: {base_offset, count, stride, fields:{<f>:{rel_offset,datatype,...}}}."""
        return self.config[module][name]

    def array_offset(self, module: str, name: str, index: int, field: str) -> int:
        """Absolute byte offset of array[index].field — base + index*stride + the field's rel_offset."""
        a = self.config[module][name]
        return a["base_offset"] + index * a["stride"] + a["fields"][field]["rel_offset"]

    # ---- telemetry ---------------------------------------------------------
    def t(self, name: str) -> dict:
        return self.telem[name]

    # ---- wire helpers ------------------------------------------------------
    @staticmethod
    def fmt(datatype: str) -> str:
        return _FMT[datatype]

    @staticmethod
    def size(datatype: str) -> int:
        return _SZ[datatype]


# Back-compat alias: the meta-backed layout used to be a thinner `ConfigDict`. Meta is a superset
# (config + telemetry + pages + flat index), so existing importers (gen_rebench) keep working.
ConfigDict = Meta


def ecu_port() -> str:
    """The ECU found by its USB serial number, not by ttyACM number: a reset or re-plug while anything
    still has the old node open brings it back as ttyACM1, and every script aimed at ttyACM0 then fails."""
    import glob
    found = sorted(glob.glob("/dev/serial/by-id/usb-JayECU_*-if00"))
    return found[0] if found else "/dev/ttyACM0"


class TsLink:
    def __init__(self, port=None, baud=115200, timeout=1.0, verbose=False):
        import serial
        self.s = serial.Serial(port or ecu_port(), baud, timeout=timeout)
        self.meta = Meta()
        self.verbose = verbose
        time.sleep(0.2)
        self.s.reset_input_buffer()

    def close(self):
        self.s.close()

    # ---- framing (Omnidyno: AA 55 + 16-byte header + payload + CRC16) -------
    def _send(self, type_id: int, body: bytes):
        self._seq = (getattr(self, "_seq", 0) + 1) & 0xFFFF
        total = OMNI_HDR + len(body) + 2
        frame = bytearray(OMNI_SYNC)
        frame += bytes([type_id, 0])                 # typeId, reserved
        frame += struct.pack("<H", total)            # length = total bytes
        frame += struct.pack("<Q", 0)                # timestamp (informational)
        frame += struct.pack("<H", self._seq)        # sequence (echoed back)
        frame += body
        frame += struct.pack("<H", crc16ccitt(bytes(frame)))
        if self.verbose:
            print(f"  TX type={type_id!r} seq={self._seq} +{len(body)}B")
        self.s.write(frame)
        self.s.flush()
        return self._seq

    def _recv(self, want_seq=None):
        """Read the next well-formed frame (optionally matching want_seq). Returns (typeId, payload)."""
        deadline = time.time() + 2.0
        while time.time() < deadline:
            # hunt for sync
            b0 = self.s.read(1)
            if not b0: continue
            if b0[0] != 0xAA: continue
            b1 = self.s.read(1)
            if not b1 or b1[0] != 0x55: continue
            hdr = self._read_exact(OMNI_HDR - 2)     # rest of the 16-byte header
            if len(hdr) < OMNI_HDR - 2: raise TimeoutError("short header")
            type_id = hdr[0]                             # hdr = [typeId][rsv][len:2][ts:8][seq:2] (14 bytes after AA 55)
            total   = struct.unpack("<H", hdr[2:4])[0]
            seq     = struct.unpack("<H", hdr[12:14])[0]
            rest    = self._read_exact(total - OMNI_HDR)   # payload + crc16
            if len(rest) < total - OMNI_HDR: raise TimeoutError("short frame")
            frame = OMNI_SYNC + hdr + rest
            want = struct.unpack("<H", frame[total-2:total])[0]
            if crc16ccitt(frame[:total-2]) != want:
                raise ValueError("response CRC16 mismatch")
            payload = frame[OMNI_HDR:total-2]
            if want_seq is not None and seq != want_seq:
                continue                             # a stray/late frame — keep hunting for our reply
            return type_id, bytes(payload)
        raise TimeoutError("no matching response")

    def _read_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.s.read(n - len(buf))
            if not chunk:
                break
            buf += chunk
        return buf

    def cmd(self, cmd: bytes, body: bytes = b""):
        seq = self._send(cmd[0], body)
        return self._recv(want_seq=seq)              # (respTypeId, payload)

    # ---- high-level ops ----------------------------------------------------
    def hello(self) -> str:
        flag, data = self.cmd(b"Q")
        return data.decode("ascii", "replace")

    def execute(self, line: str) -> str:
        """Run a CLI text command via the 'E' console command; returns its text reply."""
        flag, data = self.cmd(b"E", line.encode())
        return data.decode("ascii", "replace")

    def get_debug(self) -> str:
        """Drain the ECU text console (Lua errors, ecu_print, …) via the 'D' command. Returns whatever
        text accumulated since the last poll (a double-buffer swap on the firmware side); '' if idle."""
        _flag, data = self.cmd(b"D")
        return data.decode("ascii", "replace")

    def read_telemetry(self) -> bytes:
        flag, data = self.cmd(b"A")
        assert len(data) >= self.meta.telem_size, (len(data), self.meta.telem_size)
        return data

    def telem(self, name: str):
        ch = self.meta.t(name)
        data = self.read_telemetry()
        return struct.unpack_from(self.meta.fmt(ch["datatype"]), data, ch["offset"])[0]

    def telem_all(self):
        data = self.read_telemetry()
        out = {}
        for name, ch in self.meta.telem.items():
            out[name] = struct.unpack_from(self.meta.fmt(ch["datatype"]), data, ch["offset"])[0]
        return out

    # ---- waiting ------------------------------------------------------------
    def wait_until(self, pred, timeout=5.0, poll=0.02):
        """Poll `pred(telemetry_dict)` until it is true. Returns (ok, elapsed, last_frame).

        WAIT FOR THE THING, DO NOT SLEEP A GUESS. Fixed settle times are the single biggest cost in
        this bench set — 273 s of literal time.sleep() across the suites before any loop multiplies
        it, and bench_narrowband spent 100 % of its runtime asleep. They are also the biggest source
        of flakiness, because one constant cannot be both long enough to be reliable and short enough
        to be quick: the same guesses that made the suites slow ALSO failed intermittently on the SD
        handover, the datalog file read and the ETB recovery.

        Polling fixes both at once. The worst case is unchanged — the timeout is the old sleep — but
        the common case returns as soon as the ECU has actually done the thing, which is typically
        milliseconds.

        What it CANNOT shorten is a quantity that genuinely accumulates over wall-clock: an
        integrator ramp, a learn window, a decay measured per engine cycle. Those sleeps are the
        measurement, not a delay, and converting them would only make the test weaker. Leave them,
        and say so where they are.
        """
        t0 = time.time()
        last = None
        while True:
            last = self.telem_all()
            if pred(last):
                return True, time.time() - t0, last
            if time.time() - t0 >= timeout:
                return False, time.time() - t0, last
            time.sleep(poll)

    def set_script_wait(self, text, expect=None, tol=0.05, timeout=5.0):
        """Push a Lua script and wait until its injected values are actually on the bus.

        MEASURED, NOT GUESSED: a signalWrite becomes visible in 6-8 ms and a config write changes a
        module's behaviour in 35 ms. The suites were sleeping 1200-3000 ms after each — between 150
        and 400 times longer than the thing takes. That is where the bench set's runtime went.

        `expect` is {channel: value} in ENGINEERING units; omit it and this falls back to a short
        fixed settle, which is still an order of magnitude under the old constant.
        """
        self.set_script(text)
        if not expect:
            time.sleep(0.1)
            return True, 0.1
        sc = {k: self.meta.t(k)["scale"] for k in expect}
        ok, dt, _ = self.wait_until(
            lambda f: all(abs(f[k] * sc[k] - v) <= tol for k, v in expect.items()), timeout)
        return ok, dt

    def wait_channel(self, name, want, tol=1e-6, timeout=5.0, scaled=True):
        """Wait until channel `name` reads `want` (within tol). Scale-aware by default."""
        sc = self.meta.t(name)["scale"] if scaled else 1.0
        ok, dt, last = self.wait_until(lambda f: abs(f[name] * sc - want) <= tol, timeout)
        return ok, dt, (last[name] * sc if last else None)

    def read_config_raw(self, offset: int, size: int) -> bytes:
        # Config read 'r': payload = [offset:u32 LE][size:u16 LE]. Flat 32-bit g_config space, no pages.
        body = struct.pack("<IH", offset, size)
        rtype, data = self.cmd(b"r", body)           # rtype == OMNI_RSP_CONFIG (0x02), or 0x84 on range error
        if rtype == 0x84:
            raise ValueError(f"config read out of range @ {offset}+{size}")
        return data

    def read_config_chunked(self, offset: int, size: int, chunk: int = 200) -> bytes:
        """Read a RANGE larger than one frame — tables run to kilobytes and a single 'r' for the whole
        thing comes back as a short frame. Same 200-byte slicing the write path already uses."""
        out = b""
        while len(out) < size:
            out += self.read_config_raw(offset + len(out), min(chunk, size - len(out)))
        return out

    def get_config(self, name: str):
        c = self.meta.c(name)
        raw = self.read_config_raw(c["offset"], c["size"])
        return struct.unpack_from(self.meta.fmt(c["datatype"]), raw, 0)[0]

    def set_config(self, name: str, value):
        c = self.meta.c(name)
        payload = struct.pack(self.meta.fmt(c["datatype"]), value)
        return self.write_raw(c["offset"], payload)

    def write_raw(self, offset: int, payload: bytes):
        """Write raw bytes at an absolute config offset (for array cells / table planes the named-
        scalar path can't address). Pair with Meta to resolve the offset by name, never a magic
        number. The config is one flat 32-bit space — no pages."""
        # Config write 'w': payload = [offset:u32 LE][size:u16 LE][data].
        body = struct.pack("<IH", offset, len(payload)) + payload
        rtype, data = self.cmd(b"w", body)           # OMNI_RSP_ACK (0x00) ok, OMNI_RSP_ERROR (0x84) range/short
        if rtype == 0x84:
            raise ValueError(f"config write rejected @ {offset}+{len(payload)}")
        return rtype

    def set_script(self, text: str):
        """Push a complete Lua script into the lua_source config string (chunked page writes).
        The firmware's ScriptEngine watches g_config_generation and re-loads the script live on the
        write — no burn/reset needed. Burn afterwards to persist it into the tune.

        THE FIRST PUSH SNAPSHOTS WHAT WAS THERE. A bench borrows the script slot to inject signals,
        and 15 of them ended by writing an empty `onTick` stub rather than putting back what they
        found — so running the suite quietly destroyed whatever script the ECU was carrying. The slot
        belongs to whoever loaded it; a bench is only borrowing it. Snapshotting here rather than in
        each bench means it cannot be forgotten in the next one. restore_script() puts it back."""
        if not hasattr(self, "_script0"):
            # CHUNKED, and NOT swallowed into None. A single 4096-byte config read times out (the
            # frame is capped around 200), and catching that quietly left the snapshot empty — so
            # restore_script() became a silent no-op and the script was destroyed anyway, which is
            # exactly the failure this is here to prevent. Read it the way every other long field
            # is read, and let a genuine failure be loud.
            c0 = self.meta.c("lua_source")
            raw = self.read_config_chunked(c0["offset"], c0["size"], chunk=200)
            self._script0 = raw.split(b"\x00", 1)[0].decode("ascii", "ignore")
        c = self.meta.c("lua_source")
        body = text.encode("ascii", "ignore")
        # ZERO THE WHOLE FIELD, not just the text plus its terminator. Writing only len(text)+1 bytes
        # leaves whatever was there before beyond the null — harmless while the null is intact, and a
        # spliced script the moment it is not. A write interrupted before its terminator lands leaves
        # the new text running straight into the old tail with nothing between them, and that is a
        # script that will not parse: seen on this rig as
        #     signalWrite("dwell", 100 -- affect anything by writing its signal
        #     function onTick()
        # — a user script cut mid-call with the default script's tail welded on, reported by the ECU
        # as a Lua load error and carried into the studio's current.tune as if it were the real thing.
        # The field is 4 KB; clearing it costs about twenty extra chunks and removes the whole class.
        data = (body + b"\x00").ljust(c["size"], b"\x00")
        CH = 200
        for i in range(0, len(data), CH):
            self.write_raw(c["offset"] + i, data[i:i + CH])

    def restore_script(self):
        """Put back the script that was on the ECU before this link first wrote one. A no-op if this
        link never touched it. Benches should call it in their cleanup instead of writing a stub."""
        s0 = getattr(self, "_script0", None)
        if s0 is None:
            return False
        del self._script0                 # so restoring does not re-snapshot the stub we just wrote
        self.set_script(s0)
        self._script0 = s0                # a second restore is still the original
        return True

    def get_script(self, maxlen: int = 512) -> str:
        c = self.meta.c("lua_source")
        raw = self.read_config_raw(c["offset"], maxlen)
        return raw.split(b"\x00", 1)[0].decode("ascii", "ignore")

    def burn(self):
        # 'b' — flag the tune for persist; ECU replies OMNI_RSP_BURN_ACK (0x04). No body needed.
        rtype, data = self.cmd(b"b")
        return rtype

    # ---- engine-cycle capture (0x26) ---------------------------------------
    # One cycle of DELIVERED coil/injector/trigger edges, in engine decidegrees. The capture is
    # one-shot and boundary-aligned: arm, poll until the header says Complete, then page it out.
    # Both actions answer with the same header, so "not armed" / "still running" / "no recorder"
    # are ordinary replies rather than errors.
    CYCLE_HDR = "<HBBHHHHIBBH"          # magic, ver, state, cycle_angle, total, first, count,
                                        # rpm_x10, sync_level, dropped, reserved  (20 bytes)
    CYCLE_STATES = {0: "Idle", 1: "Armed", 2: "Recording", 3: "Complete", 4: "Stale"}
    # 4 = the PLL's own virtual grid, distinct from the real teeth on purpose: the difference
    # between the two IS the PLL error.
    CYCLE_SIGNALS = {0: "Coil", 1: "Injector", 2: "Crank", 3: "Cam", 4: "Virtual",
                     5: "Stall"}   # a recorded discontinuity, never shipped inside a frame

    def _cycle_page(self, action: int, first: int = 0, back: int = 0):
        # [action:u8][back:u8][first:u16] — back counts cycles backwards from the newest complete
        # one, so a contiguous run can be pulled out of the ECU's ring in one pass.
        _rtype, data = self.cmd(b"\x26", struct.pack("<BBH", action, back, first))
        if len(data) < 20:
            raise RuntimeError(f"short cycle reply: {len(data)} bytes")
        (magic, ver, state, cycle_angle, total, pfirst, count,
         rpm_x10, sync, dropped, cycle_seq) = struct.unpack_from(self.CYCLE_HDR, data, 0)
        if magic != 0x5943:
            raise RuntimeError(f"bad cycle magic 0x{magic:04X}")
        edges = []
        for i in range(count):
            angle, chan, flags = struct.unpack_from("<HBB", data, 20 + i * 4)
            # bits 1-3 are the signal — MASK them. Bit 4 is the cycle-start marker, and reading the
            # signal as a bare flags>>1 folded it in, so the one marked tooth per lane decoded as a
            # phantom signal "?10" and the lane looked one edge short.
            edges.append({"angle": angle / 10.0, "channel": chan,
                          "signal": self.CYCLE_SIGNALS.get((flags >> 1) & 0x07,
                                                           f"?{(flags >> 1) & 0x07}"),
                          "active": bool(flags & 1),
                          "cycle_start": bool(flags & 0x10),
                          # bits 5-7: the low 3 bits of the cycle this edge belongs to, stamped by
                          # the writing ISR. Every edge in one frame carries the same value.
                          "seq3": (flags >> 5) & 0x07})
        return {"state": self.CYCLE_STATES.get(state, str(state)), "state_id": state,
                "cycle_angle": cycle_angle / 10.0, "total": total, "first": pfirst,
                "count": count, "rpm": rpm_x10 / 10.0, "sync_level": sync,
                # The number of the cycle actually being shipped (v2+). It used to be a reserved
                # word here, so anything reading it got None and quietly compared nothing.
                "dropped": dropped, "cycle_seq": cycle_seq, "edges": edges}

    def cycle_arm(self):
        """Request a capture of the next whole engine cycle. Returns the reply header."""
        return self._cycle_page(0x00)

    def cycle_read(self, back: int = 0):
        """Page a whole capture out. back: 0 = newest complete cycle, 1 = the one before it."""
        page = self._cycle_page(0x01, 0, back)
        edges = list(page["edges"])
        while len(edges) < page["total"]:
            nxt = self._cycle_page(0x01, len(edges), back)
            if nxt["count"] == 0:
                break
            edges.extend(nxt["edges"])
        page["edges"] = edges
        page["count"] = len(edges)
        return page

    def cycle_capture(self, timeout: float = 3.0):
        """Arm, wait for the capture to complete, read it out. Raises if the engine never
        reaches a cycle boundary within `timeout` (i.e. it is not turning)."""
        self.cycle_arm()
        deadline = time.time() + timeout
        while time.time() < deadline:
            page = self._cycle_page(0x01, 0)
            if page["state"] in ("Complete", "Stale"):
                return self.cycle_read()
            time.sleep(0.02)
        raise TimeoutError(f"capture never completed (state={page['state']}, "
                           f"total={page['total']}) — is the engine turning?")


if __name__ == "__main__":
    import sys
    link = TsLink(verbose="-v" in sys.argv)
    print("signature:", link.hello())
    t = link.telem_all()
    # Names come from the generated telemetry struct, so they track the schema: `battery` (0.01 V/count,
    # NOT millivolts) and the dtc_* set. The old battery_mv/active_faults were a schema ago and made this
    # smoke test die on a KeyError before it printed anything.
    print(f"rpm={t['rpm']}  sync_level={t['sync_level']}  crank_angle={t['crank_angle']}  "
          f"batt={t['battery'] * 0.01:.2f}V  dtc_active={t['dtc_active']}  "
          f"worst=P{t['dtc_worst_code']:04X}/sev{t['dtc_worst_sev']}")
    for f in ("trigger_strategy", "trigger_physical_teeth", "trigger_missing_teeth",
              "trigger_prescaler", "engine_cylinder_count"):
        try:
            print(f"  {f} = {link.get_config(f)}")
        except KeyError:
            print(f"  {f} = (not in schema)")
    link.close()
