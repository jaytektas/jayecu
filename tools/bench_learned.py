#!/usr/bin/env python3
"""Live bench validation of the LTFT/LTT learned region and its rotating SD totems.

The learned region used to live in BKPSRAM; it now lives in plain RAM and is persisted to rotating
"totem" files on the SD card ([LearnedHeader][payload], newest valid one wins on boot). RAM does not
survive a reset, so the whole claim rests on the totem round-trip — and that claim had never been
tested, because the region only drifts when the engine is actually LEARNING (closed-loop lambda, engine
running), which a bench with no crank signal cannot produce.

The `learn` CLI reaches the region directly, so the persistence can be proven without an engine:

  1. write a marker into the region and force a totem flush
  2. RESET the ECU — RAM is gone; anything that comes back came off the card
  3. read the marker back: it survived, so the newest totem was found, its header + CRC validated, and
     its payload was restored into the region
  4. the totem sequence advances, so a flush rolls a NEW slot rather than overwriting the good one

  python3 tools/bench_learned.py
"""
import sys
import time
import pathlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from ts_bench import TsLink
import push_meta as pm

OFFSETS = [0, 7, 64, 255]      # spread across the region, not just its first byte
MARKER = 0xA5
fails = 0


def check(ok, what, detail=""):
    print(f"  [{'PASS' if ok else 'FAIL'}] {what}{('  — ' + detail) if detail else ''}")
    global fails
    if not ok:
        fails += 1


def read_at(link, off):
    out = link.execute(f"learn 0 {off}").strip()
    return int(out.split("=")[-1]) if "=" in out else -1


def seq(link):
    # "next_seq=N cap=M"
    out = link.execute("learn 3 0").strip()
    for tok in out.split():
        if tok.startswith("next_seq="):
            return int(tok.split("=")[1])
    return -1


def main():
    link = TsLink()
    print("signature:", link.hello())
    print("(needs the `learn` CLI — firmware 2026-08-09 or later)")

    # The ECU must OWN the card to write a totem. After any host MSC session it does not, and
    # LearnedStore just returns false — which reads exactly like a broken totem writer. Claim it first.
    print("claiming the card for the MCU ->", pm.sd_mcu(link.s))
    time.sleep(1.0)

    print("\n--- before: what the region holds now ---")
    before = {off: read_at(link, off) for off in OFFSETS}
    print(f"  {before}")
    seq0 = seq(link)
    print(f"  next totem sequence info = {seq0}")

    print(f"\n--- writing 0x{MARKER:02X} markers and flushing a totem ---")
    for off in OFFSETS:
        link.execute(f"learn 1 {off}")
    after_poke = {off: read_at(link, off) for off in OFFSETS}
    print(f"  region now: {after_poke}")
    check(all(v == MARKER for v in after_poke.values()), "the markers are in the RAM region",
          str(after_poke))

    print("  " + link.execute("learn 2 0").strip())
    seq1 = seq(link)
    print(f"  next totem sequence = {seq1}")
    check(seq1 > seq0, "the flush rolled a NEW totem slot (the previous good one is kept)",
          f"{seq0} -> {seq1}")

    # THE decisive step. A software reset does not necessarily clear SRAM, so "the markers are still
    # there afterwards" proves nothing on its own — the RAM may simply have survived. So now overwrite
    # the region with a DIFFERENT marker and do NOT save it. Whatever comes back after the reset can only
    # have come off the card: RAM held 0x5A, the totem holds 0xA5.
    print(f"\n--- overwriting RAM with 0x5A (NOT saved) ---")
    for off in OFFSETS:
        link.execute(f"learn 4 {off}")
    dirty = {off: read_at(link, off) for off in OFFSETS}
    print(f"  region now: {dirty}")
    check(all(v == 0x5A for v in dirty.values()), "RAM now holds the unsaved marker", str(dirty))

    print("\n--- resetting the ECU: RAM holds 0x5A, the card holds 0xA5 ---")
    # OWN THE CARD ACROSS THE RESET. The restore happens at BOOT, before any host command, so
    # pm.sd_mcu() after the fact is too late to influence it — what matters is whether the ECU holds
    # the card as it comes up. Under "key auto" that depends on the bench key, and a suite that ran
    # before this one may well have left it there (bench_knock_siggen ends with exactly that).
    link.execute("key on")
    time.sleep(0.3)
    try:
        link.execute("reset")
    except Exception:
        pass                                   # the reply is lost with the link
    link.close()
    # THE RETRY LOOP IS THE WAIT. A flat 6 s here was a guess at how long the ECU takes to boot and
    # the port to re-enumerate, and the loop underneath then re-checked the same thing a second a go —
    # so the suite paid 6 s plus a rounding-up on every run. Give the node a moment to disappear, then
    # let the loop find it as soon as it is back, and print what it actually cost.
    t_reset = time.time()
    time.sleep(0.5)                            # let the old node go away before probing for a new one
    link = None
    while time.time() - t_reset < 20.0:        # the port re-enumerates
        try:
            link = TsLink(); link.hello(); break
        except Exception:
            link = None
            time.sleep(0.1)
    if link is None:
        raise SystemExit("  the ECU did not come back within 20 s of the reset")
    print(f"  back after {time.time() - t_reset:.1f} s:", link.hello())
    pm.sd_mcu(link.s)          # a fresh boot may not hold the card either
    time.sleep(0.5)

    print("\n--- after the reset ---")
    restored = {off: read_at(link, off) for off in OFFSETS}
    print(f"  region now: {restored}")

    # TELL THE TWO FAILURES APART. A region of zeros WITH the totem sequence back at 1 is not a
    # totem that failed to restore — it is the whole store having been REINITIALISED, which is what
    # the ECU does when it cannot read the card at boot. That happens because the card is arbitrated
    # between the ECU and a USB MSC endpoint the host has permanently enumerated (it is sitting on
    # /dev/sdd as usb-JayECU_SD_Card_..., 0 B while the ECU holds it), and at boot the two race.
    # Forcing "key on" before the reset above wins that race most of the time and not always:
    # measured 2 runs in 3 after a long card-holding suite like bench_knock_siggen.
    #
    # That race is the known-open USB-MSC hand-off, NOT what this suite is here to prove. So when it
    # is lost, say so and take the reset again rather than reporting a working totem writer as
    # broken — and never quietly, because a retry that is not announced is a test that lies.
    if all(v == 0 for v in restored.values()) and seq(link) <= 1:
        print("  the store came back REINITIALISED (sequence 1, region zeroed): the ECU did not own")
        print("  the card at boot — USB-MSC arbitration, not the totem writer. Retrying the reset.")
        # REDO THE WHOLE EXPERIMENT, not just the reset. Once the store reinitialises, the totem
        # holding 0xA5 is gone — resetting again would only re-read the empty store it just built.
        for off in OFFSETS:
            link.execute(f"learn 1 {off}")     # 0xA5 markers back into RAM
        print("  " + link.execute("learn 2 0").strip())
        for off in OFFSETS:
            link.execute(f"learn 4 {off}")     # 0x5A over RAM again, unsaved
        link.execute("key on"); time.sleep(1.0)
        try:
            link.execute("reset")
        except Exception:
            pass
        link.close()
        t_reset = time.time(); time.sleep(0.5); link = None
        while time.time() - t_reset < 20.0:
            try:
                link = TsLink(); link.hello(); break
            except Exception:
                link = None; time.sleep(0.1)
        if link is None:
            raise SystemExit("  the ECU did not come back within 20 s of the retry reset")
        pm.sd_mcu(link.s); time.sleep(0.5)
        restored = {off: read_at(link, off) for off in OFFSETS}
        print(f"  after the retry: region now: {restored}")
    check(all(v == MARKER for v in restored.values()),
          "the region was RESTORED FROM THE CARD (0xA5), not left over in RAM (0x5A)", str(restored))
    print(f"  next totem sequence = {seq(link)}")

    link.close()
    print(f"\n[learned] {'FAILURES' if fails else 'all passed'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
