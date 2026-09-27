#!/usr/bin/env python3
"""Live bench validation of GENERIC CAN — the tune's own frames, both directions, on the wire.

The frames are tune data, so this test writes them the way the studio would (config writes into the
gc_frame / gc_field pools) and then checks the WIRE, not an internal counter:

  transmit  the slcan adapter reads the frame back and the bytes are decoded against what the ECU's
            own telemetry says the signals are. That closes the loop through encode -> pack -> bxCAN
            -> transceiver -> adapter, rather than trusting the ECU's account of itself.
  receive   the adapter SENDS a frame and the decoded value appears on the ECU's signal bus, read
            back through telemetry. Then the sender goes quiet and the channel must EXPIRE.

  python3 tools/bench_generic_can.py
"""
import struct
import sys
import time
import pathlib

import serial

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from ts_bench import TsLink

ADAPTER = '/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0'
BUS = 0
TX_ID = 0x360
RX_ID = 0x200

FRAME_USED, FRAME_TX, FRAME_EXT = 1, 2, 4
fails = 0


def check(ok, what, detail=""):
    global fails
    print(f"  [{'PASS' if ok else 'FAIL'}] {what}{('  - ' + detail) if detail else ''}")
    if not ok:
        fails += 1


class Adapter:
    """slcan straight over the tty — the adapter is the peer node, no slcand and no root."""

    def __init__(self, path=ADAPTER, code=b'S6'):        # S6 = 500 kbit
        self.s = serial.Serial(path, 2000000, timeout=0.2)
        time.sleep(0.3)
        for c in (b'C\r', code + b'\r', b'O\r'):
            self.s.write(c)
            time.sleep(0.2)
            self.s.read(4096)

    def close(self):
        try:
            self.s.write(b'C\r')
            time.sleep(0.1)
            self.s.close()
        except Exception:
            pass

    def send(self, can_id, data):
        self.s.write(b't%03X%d%s\r' % (can_id, len(data), data.hex().upper().encode()))
        self.s.flush()

    def flush(self):
        """Throw away whatever arrived while the test was busy. Without this a collect() measures the
        backlog as well as the window: the first run counted 184 frames in 1.0 s for a 20 ms frame,
        which is the 2.2 s of sleeps before it, not the ECU."""
        t0 = time.time()
        while time.time() - t0 < 0.15:
            self.s.read(65536)
            time.sleep(0.01)

    def collect(self, seconds, want_id=None):
        """Every frame seen in `seconds`. Anchors on the frame-start char wherever it falls: the
        adapter emits bare CRs and BEL on error, un-terminated, so a frame arrives glued behind one
        and checking line[0] silently discards the lot."""
        self.flush()
        out, buf, t0 = [], b'', time.time()
        while time.time() - t0 < seconds:
            buf += self.s.read(4096)
            while True:
                i = buf.find(b't')
                if i < 0:
                    buf = buf[-32:]
                    break
                j = buf.find(b'\r', i)
                if j < 0:
                    buf = buf[i:]
                    break
                tok, buf = buf[i + 1:j], buf[j + 1:]
                try:
                    cid, n = int(tok[:3], 16), int(tok[3:4])
                    data = bytes.fromhex(tok[4:4 + 2 * n].decode())
                except Exception:
                    continue
                if want_id is None or cid == want_id:
                    out.append((cid, data))
            time.sleep(0.01)
        return out


def Meta_size(link, field):
    """Bytes one sensor-element field occupies, so a save/restore round-trips it exactly."""
    return link.meta.size(link.meta.array('sensors', 'sensor')['fields'][field]['datatype'])


def telem_eng(link, name):
    """A channel in ENGINEERING units. link.telem() hands back the raw counts and the descriptor's
    scale is what turns them into the number a person means."""
    raw = link.telem(name)
    return None if raw is None else raw * link.meta.t(name).get('scale', 1.0)


def frame_off(link, i, field):
    return link.meta.array_offset('can', 'gc_frame', i, field)


def field_off(link, i, field):
    return link.meta.array_offset('can', 'gc_field', i, field)


def write_frame(link, i, *, flags, bus, can_id, dlc, period_ms, first_field, field_count, name=""):
    link.write_raw(frame_off(link, i, 'flags'), struct.pack('<B', flags))
    link.write_raw(frame_off(link, i, 'bus'), struct.pack('<B', bus))
    link.write_raw(frame_off(link, i, 'id'), struct.pack('<I', can_id))
    link.write_raw(frame_off(link, i, 'dlc'), struct.pack('<B', dlc))
    link.write_raw(frame_off(link, i, 'period_ms'), struct.pack('<H', period_ms))
    link.write_raw(frame_off(link, i, 'first_field'), struct.pack('<H', first_field))
    link.write_raw(frame_off(link, i, 'field_count'), struct.pack('<B', field_count))
    nb = name.encode()[:19].ljust(20, b'\0')
    link.write_raw(frame_off(link, i, 'name'), nb)


SIG_NONE = 0xFFFF   # a field with NO channel — one a SENSOR reads. It cannot be 0: that is abs_mode.


def write_field(link, i, *, sig, bit_off, width, flags=0, scale=1.0, offset=0.0,
                ttl_ms=500, policy=0, priority=10):
    link.write_raw(field_off(link, i, 'sig'), struct.pack('<H', sig))
    link.write_raw(field_off(link, i, 'bit_off'), struct.pack('<H', bit_off))
    link.write_raw(field_off(link, i, 'width'), struct.pack('<B', width))
    link.write_raw(field_off(link, i, 'flags'), struct.pack('<B', flags))
    link.write_raw(field_off(link, i, 'scale'), struct.pack('<f', scale))
    link.write_raw(field_off(link, i, 'offset'), struct.pack('<f', offset))
    link.write_raw(field_off(link, i, 'ttl_ms'), struct.pack('<H', ttl_ms))
    link.write_raw(field_off(link, i, 'policy'), struct.pack('<B', policy))
    link.write_raw(field_off(link, i, 'priority'), struct.pack('<B', priority))


def main():
    link = TsLink()
    print("signature:", link.hello())

    # The channel ids the fields bind to, taken from the meta rather than assumed. A field stores a
    # raw SignalId, so getting this wrong binds to a different channel and the test still "passes"
    # against whatever that one happens to read.
    sigs = link.meta.signals
    print(f"  signal map: {len(sigs)} entries")
    try:
        sid_rpm, sid_clt, sid_vss = sigs['rpm'], sigs['clt'], sigs['vehicle_spd']
    except KeyError as e:
        print(f"  !! {e} is not in the meta's signal map")
        return 1
    sid_oilt = sigs['oil_temp']
    print(f"  rpm={sid_rpm} clt={sid_clt} vehicle_spd={sid_vss} oil_temp={sid_oilt}")

    saved_script = link.get_script()
    ad = None
    try:
        # --- the bus, and a peer that ACKs ------------------------------------------------------
        for f, v in (('enabled', 1), ('listen_only', 0), ('bitrate', 2)):
            link.write_raw(link.meta.array_offset('can', 'bus', BUS, f), struct.pack('<B', v))
        time.sleep(0.5)
        ad = Adapter()
        link.execute(f"canloop {BUS} 0")          # on the wire, not loopback

        # --- TRANSMIT ---------------------------------------------------------------------------
        # rpm at bytes 0-1 (raw = value), clt at bytes 2-3 as Kelvin x10 (raw = C*10 + 2731.5).
        print("\n--- transmit: one frame, two fields, 50 Hz ---")
        write_field(link, 0, sig=sid_rpm, bit_off=7,  width=16, scale=1.0,  offset=0.0)
        write_field(link, 1, sig=sid_clt, bit_off=23, width=16, scale=10.0, offset=2731.5)
        write_frame(link, 0, flags=FRAME_USED | FRAME_TX, bus=BUS, can_id=TX_ID, dlc=8,
                    period_ms=20, first_field=0, field_count=2, name="bench tx")
        time.sleep(1.0)

        # Inject the two channels so there is something definite to read back. signalWrite lands at
        # Lua priority, which outranks the CAN receive priority and every sensor — exactly what an
        # injection should do.
        link.set_script("""
function onTick()
  signalWrite("rpm", 3000)
  signalWrite("clt", 85)
end
setTickRate(50)
""")
        time.sleep(1.2)

        got = ad.collect(1.0, want_id=TX_ID)
        print(f"  frames seen: {len(got)} in 1.0 s")
        check(45 <= len(got) <= 55, "the frame is on the wire at its configured 20 ms period",
              f"{len(got)} in 1.0 s, want ~50")
        if got:
            d = got[-1][1]
            rpm_raw = (d[0] << 8) | d[1]
            clt_raw = (d[2] << 8) | d[3]
            print(f"  payload {d.hex(' ')}  -> rpm={rpm_raw}  clt_raw={clt_raw} "
                  f"({(clt_raw - 2731.5) / 10:.1f} C)")
            check(abs(rpm_raw - 3000) <= 2, "rpm encodes as the bus value", f"{rpm_raw}")
            check(abs(clt_raw - 3582) <= 3, "clt encodes as Kelvin x10", f"{clt_raw} vs 3582")

        # --- absent policy ----------------------------------------------------------------------
        # A channel NOTHING publishes — no sensor fitted, no module claiming it — which is the case
        # the policy exists for. Injecting one and waiting for it to lapse does not work: signalWrite
        # carries no TTL, so the override never expires and the field is never absent.
        print("\n--- transmit: a channel with nothing to say follows its policy ---")
        write_field(link, 1, sig=sid_oilt, bit_off=23, width=16, scale=10.0, offset=2731.5,
                    policy=0)                                   # 0 = send raw zero
        time.sleep(0.8)
        got = ad.collect(0.8, want_id=TX_ID)
        if got:
            d = got[-1][1]
            print(f"  payload {d.hex(' ')}")
            check((d[0] << 8 | d[1]) == 3000, "the live field in the same frame still goes out",
                  f"rpm={(d[0] << 8 | d[1])}")
            check((d[2] << 8 | d[3]) == 0,
                  "an absent channel sends RAW zero, which reads as -273 C and not as a plausible 0",
                  f"raw={(d[2] << 8 | d[3])}")
        else:
            check(False, "frames still transmit while one field is absent")

        print("\n--- transmit: HOLD keeps the previous bits ---")
        link.write_raw(field_off(link, 1, 'policy'), struct.pack('<B', 1))   # 1 = hold last
        time.sleep(0.8)
        got = ad.collect(0.6, want_id=TX_ID)
        check(bool(got) and (got[-1][1][2] << 8 | got[-1][1][3]) == 0,
              "HOLD over bits never written is still zero, not noise",
              f"raw={(got[-1][1][2] << 8 | got[-1][1][3]) if got else '-'}")

        print("\n--- transmit: SKIP suppresses the WHOLE frame ---")
        link.write_raw(field_off(link, 1, 'policy'), struct.pack('<B', 2))   # 2 = skip frame
        time.sleep(0.8)
        got = ad.collect(0.8, want_id=TX_ID)
        print(f"  frames seen: {len(got)}")
        check(len(got) == 0,
              "one absent field with SKIP stops the frame — it does not go out full of zeros",
              f"{len(got)} frames")
        link.write_raw(field_off(link, 1, 'policy'), struct.pack('<B', 0))
        link.write_raw(frame_off(link, 0, 'field_count'), struct.pack('<B', 1))
        time.sleep(0.6)

        # --- RECEIVE ----------------------------------------------------------------------------
        print("\n--- receive: the adapter sends, the signal bus reads ---")
        write_field(link, 2, sig=sid_vss, bit_off=7, width=16, scale=10.0, offset=0.0,
                    ttl_ms=400, priority=10)
        write_frame(link, 1, flags=FRAME_USED, bus=BUS, can_id=RX_ID, dlc=8,
                    period_ms=0, first_field=2, field_count=1, name="bench rx")
        time.sleep(1.0)

        payload = struct.pack('>H', 1000) + b'\0' * 6      # 1000 / 10 = 100.0
        for _ in range(12):
            ad.send(RX_ID, payload)
            time.sleep(0.05)
        time.sleep(0.2)
        v = telem_eng(link, 'vehicle_spd')
        print(f"  vehicle_spd = {v} km/h  (raw {link.telem('vehicle_spd')})")
        check(v is not None and abs(v - 100.0) < 0.6, "the decoded value reached the signal bus",
              f"{v}")

        # --- TTL --------------------------------------------------------------------------------
        print("\n--- receive: the sender goes quiet and the channel EXPIRES ---")
        time.sleep(1.5)                            # ttl is 400 ms; give it plenty
        v2 = telem_eng(link, 'vehicle_spd')
        print(f"  vehicle_spd after silence = {v2} km/h")
        check(v2 is not None and abs(v2 - 100.0) > 0.6,
              "a dead sender leaves an absent channel, not a number frozen at its last value",
              f"{v2}")

        # --- A CAN SENSOR ------------------------------------------------------------------------
        # The whole point of the unification: one decode, and a sensor takes the reading through its
        # OWN pipeline — calibration, operating window, per-sensor P-codes, enable — rather than the
        # field writing the channel directly. The adapter plays the wideband.
        print("\n--- a CAN sensor: the field feeds the sensor, the sensor publishes ---")
        # The catalogue index of `lambda` — taken from the meta's element ids, not assumed.
        sensors = link.meta.array('sensors', 'sensor')
        eids = sensors.get('element_ids') or []
        lam = eids.index('lambda_1') if 'lambda_1' in eids else None
        if lam is None:
            check(False, "could not find the lambda_1 sensor in the meta", "element_ids missing")
        else:
            print(f"  lambda is sensor[{lam}]")
            saved_sensor = {f: link.read_config_raw(
                                link.meta.array_offset('sensors', 'sensor', lam, f),
                                Meta_size(link, f))
                             for f in ('enabled', 'interface', 'can_frame', 'can_bit')}

            # A receive field with NO channel: the sensor publishes lambda_1, not the field.
            write_field(link, 3, sig=SIG_NONE, bit_off=7, width=16, scale=1000.0, offset=0.0, ttl_ms=400)
            write_frame(link, 2, flags=FRAME_USED, bus=BUS, can_id=0x2B1, dlc=8,
                        period_ms=0, first_field=3, field_count=1, name="wideband")
            so = lambda f: link.meta.array_offset('sensors', 'sensor', lam, f)
            link.write_raw(so('interface'), struct.pack('<B', 5))     # 5 = CAN
            # THE SENSOR NAMES ITS FIELD BY FRAME AND START BIT, not by a pool index: the frame's
            # bus/ext/id as one key, and the start bit of the field within it. A pool index does not
            # survive the studio repacking the pool, which it does on any structural edit.
            link.write_raw(so('can_frame'), struct.pack('<I', (BUS << 30) | 0x2B1))
            link.write_raw(so('can_bit'), struct.pack('<h', 7))
            link.write_raw(so('enabled'), struct.pack('<B', 1))
            time.sleep(1.2)

            for _ in range(12):
                ad.send(0x2B1, struct.pack('>H', 850) + b'\0' * 6)   # 850/1000 = 0.850 lambda
                time.sleep(0.05)
            time.sleep(0.3)
            lv = telem_eng(link, 'lambda_1')
            print(f"  lambda_1 = {lv}  (raw {link.telem('lambda_1')})")
            check(lv is not None and abs(lv - 0.850) < 0.02,
                  "the reading arrived through the SENSOR, not as a direct bus write", f"{lv}")

            print("\n--- the pool is REPACKED: the sensor follows its frame, not a pool slot ---")
            # Exactly what the studio does when a frame is added above this one: every later field
            # moves. The frame keeps its id and its field keeps its start bit, so the sensor's
            # reference is untouched — and slot 3, where the field used to be, is left holding a
            # field with a scale of 1.0, so a stale pool index reads 850 lambda and rails the
            # channel instead of reading 0.850. The two answers are far enough apart to tell apart.
            write_field(link, 9, sig=SIG_NONE, bit_off=7, width=16, scale=1000.0, offset=0.0, ttl_ms=400)
            write_field(link, 3, sig=SIG_NONE, bit_off=7, width=16, scale=1.0, offset=0.0, ttl_ms=400)
            write_frame(link, 2, flags=FRAME_USED, bus=BUS, can_id=0x2B1, dlc=8,
                        period_ms=0, first_field=9, field_count=1, name="wideband")
            time.sleep(1.2)
            for _ in range(12):
                ad.send(0x2B1, struct.pack('>H', 850) + b'\0' * 6)
                time.sleep(0.05)
            time.sleep(0.3)
            lvr = telem_eng(link, 'lambda_1')
            print(f"  lambda_1 after the repack = {lvr}")
            check(lvr is not None and abs(lvr - 0.850) < 0.02,
                  "the field moved in the pool and the sensor still reads it", f"{lvr}")

            # …AND A REFERENCE THAT BREAKS IS NOT SILENT. Move the field to other bits, which the
            # sensor is not naming, and the sensor stops publishing and raises its config code
            # rather than going quietly on reading whatever now sits where it used to look.
            before = int(link.telem_all().get('dtc_active', 0) or 0)
            link.write_raw(field_off(link, 9, 'bit_off'), struct.pack('<H', 31))
            time.sleep(1.2)
            for _ in range(12):
                ad.send(0x2B1, struct.pack('>H', 850) + b'\0' * 6)
                time.sleep(0.05)
            time.sleep(0.3)
            lvb = telem_eng(link, 'lambda_1')
            after = int(link.telem_all().get('dtc_active', 0) or 0)
            print(f"  lambda_1 with a broken reference = {lvb}   active dtcs {before} -> {after}")
            check(lvb is None or abs(lvb - 0.850) > 0.02,
                  "a start bit the frame no longer has stops the sensor publishing", f"{lvb}")
            # …AND IT SAYS SO. Counted as a DELTA against the codes this bench was already carrying
            # (a default tune has no firing order, so P1651 is always up): asking whether ANY code is
            # active would be an assertion that could never go red.
            check(after == before + 1,
                  "a CAN sensor whose reference no longer resolves raises its config code",
                  f"{before} -> {after}")
            link.write_raw(field_off(link, 9, 'bit_off'), struct.pack('<H', 7))
            write_frame(link, 2, flags=FRAME_USED, bus=BUS, can_id=0x2B1, dlc=8,
                        period_ms=0, first_field=3, field_count=1, name="wideband")
            write_field(link, 3, sig=SIG_NONE, bit_off=7, width=16, scale=1000.0, offset=0.0, ttl_ms=400)
            link.write_raw(field_off(link, 9, 'width'), struct.pack('<B', 0))    # slot 9 back to nothing
            time.sleep(1.2)

            print("\n--- the sender says NOTHING TO REPORT: the sentinel is not a reading ---")
            # A wideband in free air sends 0x7FFF. PROVE THE CONTROL FIRST: with the sentinel off,
            # that code decodes to a mixture and gets published, so the test can actually go red.
            # (32.767 lambda through a U08 @0.01 channel reads as its 2.55 rail, which is why
            # comparing against 32.767 would have been an assertion that could never fail.)
            def feed(raw, n=8):
                for _ in range(n):
                    ad.send(0x2B1, struct.pack('>H', raw) + b'\0' * 6)
                    time.sleep(0.04)
                time.sleep(0.2)
                return telem_eng(link, 'lambda_1')

            link.write_raw(field_off(link, 3, 'flags'), struct.pack('<B', 0))     # sentinel OFF
            time.sleep(0.8)
            good = feed(900)
            bogus = feed(0x7FFF)
            print(f"  sentinel OFF: real={good}  free-air={bogus}")
            check(good is not None and abs(good - 0.900) < 0.02,
                  "the control: a real reading publishes", f"{good}")
            check(bogus is not None and bogus > 2.0,
                  "the control: free air publishes a bogus mixture when nothing filters it",
                  f"{bogus}")

            link.write_raw(field_off(link, 3, 'flags'), struct.pack('<B', 4))     # FIELD_SENTINEL
            link.write_raw(field_off(link, 3, 'sentinel'), struct.pack('<I', 0x7FFF))
            time.sleep(0.8)
            good2 = feed(900)
            held  = feed(0x7FFF)
            print(f"  sentinel ON:  real={good2}  free-air={held}")
            check(good2 is not None and abs(good2 - 0.900) < 0.02,
                  "a real reading still gets through with the sentinel armed", f"{good2}")
            check(held is not None and held < 2.0,
                  "free air is NOT decoded as a mixture — the field is skipped, so it expires instead",
                  f"{held}")
            link.write_raw(field_off(link, 3, 'flags'), struct.pack('<B', 0))

            print("\n--- the sender goes quiet: the sensor stops publishing ---")
            time.sleep(1.5)
            lv2 = telem_eng(link, 'lambda_1')
            print(f"  lambda_1 after silence = {lv2}")
            check(lv2 is None or abs(lv2 - 0.850) > 0.02,
                  "a stale field aborts the sensor's acquire, so nothing is published",
                  f"{lv2}")

            for f, v in saved_sensor.items():
                link.write_raw(so(f), v)
            link.write_raw(frame_off(link, 2, 'flags'), struct.pack('<B', 0))
            time.sleep(0.5)

    finally:
        print("\n--- restoring ---")
        try:
            link.restore_script()
        except Exception:
            pass
        for i in (0, 1, 2):
            try:
                link.write_raw(frame_off(link, i, 'flags'), struct.pack('<B', 0))
                link.write_raw(frame_off(link, i, 'field_count'), struct.pack('<B', 0))
            except Exception:
                pass
        if ad:
            ad.close()
        print(f"\n{'ALL PASS' if not fails else str(fails) + ' FAILURE(S)'}")
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
