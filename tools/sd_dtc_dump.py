#!/usr/bin/env python3
"""Decode the ECU's SD persistence files once the card is USB-mounted (drop the
bench battery-sense rail below ~6V so the SdArbitrator hands the card to USB MSC).

  python3 tools/sd_dtc_dump.py /media/jay/<LABEL>     # or whatever the mount path is

dtc.bin       — the serialized DtcManager table (live "open cases", STORED history)
faultlog.bin  — append-only FaultLogEntry records with the freeze-frame at each fault
"""
import sys, struct, pathlib

def find_ci(root, name):
    """Case-insensitive file lookup (FatFS 8.3 stores uppercase: FAULTLOG.BIN)."""
    if not root.exists():
        return None
    for x in root.iterdir():
        if x.name.lower() == name.lower():
            return x
    return None

# DtcRecord (Dtc.h, #pragma pack(1), 36 bytes):
#   code u16, source u8, severity u8, status u8, heal_cycles u8, count u16,
#   first_boot u16, first_ms u32, last_boot u16, last_ms u32, ff[4] float
DTC_REC = "<HBBBBHHIHI4f"
DTC_MAGIC = 0x44544331  # "DTC1"
STATUS = {0x01: "ACTIVE", 0x02: "STORED", 0x04: "CONFIRMED"}

def flags(status):
    return "|".join(n for b, n in STATUS.items() if status & b) or "-"

def dump_dtc(path):
    b = path.read_bytes()
    if len(b) < 10:
        print(f"  {path.name}: too short ({len(b)} B)"); return
    magic, ver, boot, n = struct.unpack_from("<IHHH", b, 0)
    ok = "OK" if magic == DTC_MAGIC else f"BAD(0x{magic:08X})"
    print(f"  magic={ok} version={ver} boot_id={boot} records={n}")
    off = 10
    for i in range(n):
        (code, src, sev, status, heal, cnt, fb, fms, lb, lms,
         rpm, mapk, clt, batt) = struct.unpack_from(DTC_REC, b, off)
        off += struct.calcsize(DTC_REC)
        print(f"   P{code:04X}  sev={sev} {flags(status):16} count={cnt} src={src} "
              f"first=(boot {fb}, {fms}ms) last=(boot {lb}, {lms}ms)")
        print(f"        freeze: rpm={rpm:.0f} map={mapk:.1f}kPa clt={clt:.1f}C batt={batt:.2f}V")

if __name__ == "__main__":
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "/mnt/jaytek_sd")
    print(f"root: {root}\n--- dtc.bin (table + per-code freeze-frames) ---")
    p = find_ci(root, "dtc.bin")
    dump_dtc(p) if p else print("  (absent)")
    print("\nall files at root:", sorted(x.name for x in root.iterdir()) if root.exists() else "(mount path not present)")
