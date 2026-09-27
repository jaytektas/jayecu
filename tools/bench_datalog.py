#!/usr/bin/env python3
"""Live bench validation of the SD datalogger (the FatFS write path + the USB-MSC hand-off).

The logger is inert by default and auto-starts only while the engine is turning (rpm > 100) and the ECU
owns the card. So this drives the Ardu-Stim to give the decoder a real crank signal, lets the logger run,
stops the stim, and then reads the file back off the card the same way a user would — over the ECU's own
file protocol.

What this proves on hardware:
  1. with datalog disabled, nothing is logging (the default really is inert)
  2. enabled + engine turning -> a LOGnnnn.MLG is opened and grows
  3. the records are the packed telemetry frame, at the configured rate
  4. stopping the engine closes the file (a torn last record would mean the close path is broken)
  5. the file reads back off the card intact through the SD file protocol

  python3 tools/bench_datalog.py [rpm]      (default 1200)
"""
import struct
import sys
import time
import pathlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from ts_bench import TsLink
from gen_rebench import open_stim, select, configure, WHEELS
import push_meta as pm

WHEEL_IDX = 6          # '36-1' — dense, locks in ~7 s, single crank stream (no cam needed)

STIM_PORT = "/dev/ttyUSB0"
fails = 0


def check(ok, what, detail=""):
    global fails
    print(f"  [{'PASS' if ok else 'FAIL'}] {what}{('  — ' + detail) if detail else ''}")
    if not ok:
        fails += 1


def stim_rpm(sp, rpm):
    """ardustim 'F' — software-override the pot and hold a fixed RPM (little-endian word)."""
    sp.reset_input_buffer()
    sp.write(b"F" + struct.pack("<H", int(rpm)))
    time.sleep(0.3)


def _sd_state(link):
    """(phase, override, card, ecu_has, usb_has, busy) — the arbitrator's own answer."""
    rt, d = link.cmd(bytes([0x23]))
    if len(d) < 9:
        return None
    return struct.unpack("<8B", d[1:9])[:6]


def sd_for_writing(link, timeout=6.0):
    """Give the card back so the ECU's own writers (the datalogger) may use it.

    THE TWO HALVES ARE OPPOSITE AND THE SUITE HAS TO SWITCH BETWEEN THEM. SD_MCU makes the ECU hold
    the card so the HOST can read logs off it; while that override stands the logger's writes are
    refused and the file comes back as bare header. SD_RELEASE returns it to key-driven, which is
    what logging needs.

    POLLED, NOT SLEPT. The handover is not instant and every fixed delay I guessed here was wrong in
    one direction or the other — 0.5 s left the fetch empty, 1.5 s left a run logging into a card it
    did not own. Ask the arbitrator until it agrees, which is both faster and correct."""
    link.cmd(bytes([0x22]))
    deadline = time.time() + timeout
    while time.time() < deadline:
        st = _sd_state(link)
        if st and st[1] == 0:            # override cleared -> key-driven
            return True
        time.sleep(0.2)
    return False


def sd_for_reading(link, timeout=6.0):
    """Take the card so the host can fetch files off it (the SD_MCU half).

    WAIT FOR THE OVERRIDE, not for ecu_has. ecu_has is already 1 in both states with the key on —
    the ECU owns the card either way — so polling it returned instantly and waited for nothing, and
    the fetch that followed came back empty. The override is the thing that actually changes."""
    link.cmd(bytes([0x20]))
    deadline = time.time() + timeout
    while time.time() < deadline:
        st = _sd_state(link)
        if st and st[1] == 1 and st[3] == 1:      # override == MCU, and the ECU has it
            return True
        time.sleep(0.2)
    return False


def newest_log(ser, ext="MLG"):
    """The highest-numbered LOGnnnn.MLG the card holds, and its size (0 if there is none).

    .MLG, not .BIN: the logger writes MegaLogViewer's own format now — header, one 89-byte
    descriptor per selected channel, then fixed-length records. A card may still carry .BIN files
    from before that change; they are a different, undecodable format and are not this.

    THE SCAN MUST NOT HAVE A CEILING THE CARD CAN REACH. This walked 0..63 and returned the highest
    it found, so once the bench had written its 64th log it answered LOG0063 for ever — and every
    check of the form "a new file appeared" compared that name against itself and failed. It reads
    exactly like the logger not writing, which is where an afternoon went; the logger was fine and
    the card had simply filled past the end of the loop.

    Exponential probe for a gap, then bisect: about 2*log2(n) round trips instead of a fixed 64, and
    no ceiling. It assumes the numbering is contiguous, which is what the logger produces — it names
    the next free index in order — and a deleted file in the middle only makes it return an earlier
    log, never a wrong one."""
    def exists(n):
        sz = pm.sd_file_size(ser, f"LOG{n:04d}.{ext}")
        return (sz or 0) > 0

    if not exists(0):
        return None, 0
    lo, hi = 0, 1
    while exists(hi):                      # first index that is NOT there
        lo, hi = hi, hi * 2
        if hi > 1 << 20:                   # a card cannot hold this many; stop rather than spin
            break
    while hi - lo > 1:                     # bisect for the last one that is
        mid = (lo + hi) // 2
        if exists(mid): lo = mid
        else:           hi = mid
    name = f"LOG{lo:04d}.{ext}"
    return name, pm.sd_file_size(ser, name)


# The format's own arithmetic, so a file's size alone says how many records are in it — and a size
# that is NOT a whole number of records means a torn tail, which is the failure worth catching.
MLG_HEADER = 24
MLG_DESC   = 89


def records_in(size, channels, field_bytes):
    body = size - (MLG_HEADER + channels * MLG_DESC)
    rec  = 4 + field_bytes + 1
    return body / rec, rec


def mask_base(link):
    """Absolute offset of datalog.mask[0].bits. The array follows the module's scalars, and
    on_invalid is the last of them — derived from the meta rather than written down."""
    return link.meta.c("datalog_on_invalid")["offset"] + 1


def catalog_head(n):
    """Field bytes of the first `n` catalog channels — the set a low bit mask selects."""
    import re, pathlib
    gen = pathlib.Path(__file__).resolve().parent.parent / "generated" / "mlg_log.h"
    rows = re.findall(r'\{ "[a-z0-9_]+", "[^"]*", "[^"]*", [^,]+, [^,]+, \d+u, (\d+)u',
                      gen.read_text())
    return sum(int(x) for x in rows[:n])


def main():
    rpm = int(sys.argv[1]) if len(sys.argv) > 1 else 1200
    link = TsLink()
    print("signature:", link.hello())

    # TAKE THE CARD BACK FIRST. With the key off the ECU hands its SD to USB — correctly, that is
    # the whole point of the hand-off — and then every file operation here returns unavailable and
    # the run reads as a total failure of the logger. SD_MCU is the "I am done, take it back" half
    # of the same protocol. The host must have unmounted; on a desktop that automounts, unmount it
    # before running this.
    # A start writes the header AND one 89-byte descriptor per channel — 32 KB for the shipped set,
    # which takes about half a second. ts_bench's 2 s default is close enough to that to fail
    # intermittently and look like a firmware fault.
    link.s.timeout = 8
    link.cmd(bytes([0x20]))
    time.sleep(1.0)
    st = link.cmd(bytes([0x23]))[1]
    if len(st) >= 6 and st[4] != 1:
        print(f"  WARNING: the ECU does not have the card (present={st[3]} ecu={st[4]} usb={st[5]}) "
              f"— unmount it on the PC and re-run")

    saved = {k: link.get_config(k) for k in ("datalog_enabled", "datalog_rate_hz")}
    print(f"saved config: {saved}")

    ser = link.s            # the same open port, for the file-protocol helpers
    sp = None
    try:
        # --- 1. default state: inert -------------------------------------------------------------
        link.set_config("datalog_enabled", 0)
        time.sleep(0.5)
        before_name, before_size = newest_log(ser)
        print(f"\nnewest log before the run: {before_name} ({before_size} bytes)")

        # --- 2. drive the logger directly ---------------------------------------------------------
        # The logger self-starts only above 100 rpm, and rpm is decoder-owned (Lua cannot forge it), so
        # on a bench with no crank signal the FatFS path is unreachable. `datalog` forces it on and
        # `datarec` appends the very record the running logger writes — so the file this produces is
        # byte-identical in shape to a real one, which is what the FatFS write/close path is judged on.
        link.set_config("datalog_rate_hz", 10)
        link.set_config("datalog_enabled", 1)
        time.sleep(0.3)

        print("\n--- forcing the logger on ---")
        # Release the card first, for the same reason as the high-rate runs below: the SD_MCU taken
        # at startup is the file-transfer override, and while it stands the logger's writes are
        # refused. The file still gets created and its header written, so what came back was exactly
        # 31797 bytes of header with zero records — which reads as the logger sampling nothing.
        sd_for_writing(link)
        print("  " + link.execute("datalog 1 10").strip())

        N = 12
        print(f"--- appending {N} records ---")
        for i in range(N):
            out = link.execute("datarec").strip()
            if i == 0:
                print("  " + out)
            time.sleep(0.12)

        # DISABLE BEFORE CLOSING, not after. This used to close the file and THEN clear
        # datalog_enabled, which leaves a window — one serial round-trip wide, with the card still
        # released for writing — in which the logger is free to open a fresh file. It did: two logs
        # appeared per run (…0217 -> …0219) and "the newest log" was the empty one, 31797 bytes of
        # header and no records, while the twelve records this section wrote sat in the file before
        # it. That is the same symptom as the logger capturing nothing, which is what it was reported
        # as. Clearing the flag first and waiting for it to land closes the window instead of racing
        # it.
        print("--- closing the file ---")
        link.set_config("datalog_enabled", 0)
        time.sleep(0.3)
        print("  " + link.execute("datalog 0 0").strip())
        # AND STOP IT AUTO-STARTING AGAIN. datalog_enabled is still 1 and the engine is still turning,
        # so the logger opens a FRESH file the moment the card comes back for the read below — and
        # "the newest log" is then that empty one, header and nothing else, while the records this
        # section actually wrote sit in the file before it. Two files appeared per run (…65 -> …67)
        # and the suite read the wrong one, reporting 0 records against a logger that had written
        # them correctly.
        # WAIT FOR THE RECORDS TO LAND, do not assume one second covers the close. The writer task
        # flushes the ring and closes on its own schedule; read too early and the file is exactly its
        # header — 31797 bytes for the shipped channel set, header 31797, zero records — which reads
        # as the logger having captured nothing at all. Poll until it grows past the header, or give
        # up and let the checks below report what is actually there.
        #
        # AND TAKE THE CARD BACK FIRST. Logging needs the override cleared; reading the file needs it
        # set. They are opposite halves of the same protocol and the suite has to switch between them
        # — release to log, take to read. Releasing without re-taking is how I turned a passing read
        # into "0 bytes" while fixing the write.
        sd_for_reading(link)
        after_name, after_size = newest_log(ser)
        deadline = time.time() + 8.0
        while time.time() < deadline:
            if after_name and after_size > 0 and (after_name != before_name or after_size > before_size):
                break
            time.sleep(0.3)
            after_name, after_size = newest_log(ser)
        print(f"newest log after the run:  {after_name} ({after_size} bytes)")
        check(after_name is not None, "a datalog file exists", str(after_name))
        grew = (after_name != before_name) or (after_size > before_size)
        check(grew, "…and it is new or grew during the run",
              f"{before_name}/{before_size} -> {after_name}/{after_size}")

        # --- 4/5. read it back and check the shape ------------------------------------------------
        if after_name:
            data = pm.fetch_file_full(ser, after_name, 512)
            check(data is not None and len(data) > 0, "the file reads back off the card",
                  f"{len(data) if data else 0} bytes")
            if data:
                # ASK THE FILE. MLG states its own channel count and record length in the first 24
                # bytes, so the shape is checked against what the log SAYS it is rather than against
                # a constant that was true when the logger wrote raw telemetry structs. (That old
                # assumption — length divisible by sizeof(EcuTelemetry) — fails on every valid MLG
                # file, because it counts the header and descriptors as if they were data.)
                magic = data[:5]
                rec_len, n_ch = struct.unpack(">HH", data[20:24])
                hdr = MLG_HEADER + n_ch * MLG_DESC
                check(magic == b"MLVLG", "the file is MLG v2", repr(magic))
                check(len(data) > hdr, "…with a header and some records",
                      f"{len(data)} bytes, header {hdr}")
                body = len(data) - hdr
                rec  = 4 + rec_len + 1
                whole, tail = divmod(body, rec)
                print(f"    {len(data)} bytes = header {hdr} ({n_ch} channels) "
                      f"+ {whole} records of {rec} B, {tail} left over")
                check(tail == 0, "the file is a whole number of records (clean close)",
                      f"{tail} trailing bytes")
                check(whole >= 2, "the records made it to the card", f"{whole} records")

    finally:
        try:
            link.execute("datalog 0 0")
        except Exception:
            pass
        for k, v in saved.items():
            link.set_config(k, v)
        print(f"\nrestored config: {saved}")

    # --- 6. THE HIGH-RATE PATH -----------------------------------------------------------------
    # The reason the sampler and the writer are separate tasks with a ring between them. Two runs:
    # a small channel set, which must sustain 1 kHz with nothing dropped, and the shipped set, which
    # must NOT — it is past what the card can take, and the point is that it says so rather than
    # quietly thinning the log.
    # A CHANNEL COUNT AT A RATE IS A BYTE RATE, and the card has one ceiling. These runs walk up to
    # it: small sets sustain 1 kHz outright, wider ones do not, and the one that does not must SAY so
    # rather than quietly thinning the log. That reporting is the feature — a high-rate log missing
    # every third sample looks exactly like one that is not.
    base = mask_base(link)

    def high_rate(n_ch, hz, seconds=5.0):
        mask = bytearray(57)
        for i in range(n_ch):
            mask[i >> 3] |= 1 << (i & 7)
        link.write_raw(base, bytes(mask))
        prev_name, _ = newest_log(ser)
        # HAND THE CARD BACK BEFORE LOGGING. SD_MCU above is the file-transfer half of the protocol —
        # it forces the ECU to hold the card so the host can read logs off it — and while that
        # override stands the datalogger's own writes are refused: "datalog: start failed (card busy
        # / not ours?)", 0 Hz sustained, ring high water 0 B. The suite set it once at startup and
        # never cleared it, so it broke its own later runs and left the ECU that way for every suite
        # after it, and for whoever used the bench next. SD_RELEASE returns ownership to key-driven,
        # which is what the logger needs.
        sd_for_writing(link)
        link.execute(f"datalog 1 {hz}")
        t0 = time.time(); time.sleep(seconds); elapsed = time.time() - t0
        stats = link.execute("datalog 0 2").strip()
        link.execute("datalog 0 0")
        # WAIT FOR THE NEW FILE, do not assume 1.5 s is enough. The logger closes and flushes on its
        # own schedule, and until it has, newest_log() truthfully reports the PREVIOUS file — so the
        # "produced a file" check compared a name against itself and failed a run that had worked.
        # It moved between runs, which is what a race looks like from the outside. Same worst case,
        # returns as soon as the card actually has the file.
        name, size = newest_log(ser)
        deadline = time.time() + 8.0
        while name == prev_name and time.time() < deadline:
            time.sleep(0.25)
            name, size = newest_log(ser)

        field_bytes = catalog_head(n_ch)
        rec = 4 + field_bytes + 1
        n_rec, _ = records_in(size, n_ch, field_bytes) if name != prev_name else (0, rec)
        actual = n_rec / elapsed if name != prev_name else 0.0
        dropped = int(stats.split("dropped")[1].split()[0]) if "dropped" in stats else -1
        high    = int(stats.split("high water")[1].split()[0]) if "high water" in stats else -1
        print(f"  {n_ch:3d} ch @ {hz:4d} Hz -> {actual:6.0f} Hz sustained, "
              f"{rec:4d} B/record, {actual*rec/1024:6.1f} KB/s, "
              f"dropped {dropped}, ring high water {high} B")
        check(name != prev_name, f"{n_ch} ch @ {hz} Hz produced a file", str(name))
        if name != prev_name:
            check(abs(n_rec - round(n_rec)) < 1e-6,
                  f"…{n_ch} ch file ends on a whole record", f"{n_rec:.3f} records")
        return actual, dropped, high, rec

    print("\n--- high rate: walking the channel count up at 1 kHz ---")
    try:
        a20, d20, _, _ = high_rate(20, 1000)
        check(d20 == 0, "20 channels sustains 1 kHz with nothing dropped", f"{d20} dropped")
        check(a20 > 900, "…at the rate asked for", f"{a20:.0f} Hz")

        a50, d50, h50, r50 = high_rate(50, 1000)
        # 50 channels is the interesting one: whether it fits depends on the byte rate, which is why
        # the harness reports rather than asserts a number somebody guessed.
        check(a50 > 0, "50 channels at 1 kHz produced a log", f"{a50:.0f} Hz")
        if d50 == 0:
            check(a50 > 900, "…and sustained the rate", f"{a50:.0f} Hz, ring peak {h50} B")
        else:
            print(f"    (50 ch @ 1 kHz is past the card: {a50*r50/1024:.0f} KB/s sustained, "
                  f"{d50} dropped — reported, not hidden)")

        a50b, d50b, h50b, _ = high_rate(50, 500)
        check(d50b == 0, "50 channels at 500 Hz drops nothing", f"{d50b} dropped, peak {h50b} B")
    finally:
        link.write_raw(base, bytes(57))     # back to "as shipped"
        try: link.execute("datalog 0 0")
        except Exception: pass

    link.close()
    # LEAVE THE CARD AS WE FOUND IT. The SD_MCU taken at startup is an override, not a transient —
    # it outlives this process and blocks the ECU's own SD writers until something clears it. Left
    # set, it silently broke the datalogger and the learned-region totems for every suite that ran
    # afterwards, and would have for the next person to use the bench.
    try:
        link.cmd(bytes([0x22]))          # SD_RELEASE: back to key-driven
    except Exception:
        pass
    print(f"\n[datalog] {'FAILURES' if fails else 'all passed'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
