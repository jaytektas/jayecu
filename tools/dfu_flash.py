#!/usr/bin/env python3
"""Reflash the ECU over USB DFU — the path for a board with no ST-LINK.

  python3 tools/dfu_flash.py [--board jaytek_v1] [--bin path/to/code.bin]

Sends the `dfu` CLI command over the ECU's serial port, which reboots it into the STM32
system bootloader, then writes the image with dfu-util and leaves.

THE PERMISSION CHECK COMES FIRST, AND IT IS THE POINT. Entering DFU is one command;
leaving it needs a successful write or a power cycle. dfu-util's packaged udev rule tags
the bootloader `uaccess`, which grants access only to the ACTIVE LOCAL SEAT SESSION — so
over SSH, from a script, or with the seat at a login greeter it fails with
LIBUSB_ERROR_ACCESS, and it fails AFTER the board is already in the bootloader and no
longer reachable over serial. This refuses to start in that case rather than stranding
the board, which is exactly what happened before the check existed.
"""
import argparse, glob, os, pathlib, re, shutil, subprocess, sys, time

REPO = pathlib.Path(__file__).resolve().parent.parent
DFU_VID_PID = "0483:df11"


def dfu_node():
    """(bus, dev, /dev/bus/usb/... path) of the bootloader, or None."""
    try:
        out = subprocess.run(["lsusb"], capture_output=True, text=True, timeout=10).stdout
    except Exception:
        return None
    m = re.search(r"Bus (\d+) Device (\d+): ID " + DFU_VID_PID, out)
    return (m.group(1), m.group(2), f"/dev/bus/usb/{m.group(1)}/{m.group(2)}") if m else None


def preflight():
    """Refuse to enter DFU unless we are likely to be able to write to it afterwards."""
    if not shutil.which("dfu-util"):
        sys.exit("dfu-util is not installed.  sudo apt install dfu-util")

    # Already in DFU from a previous attempt? Then we can test the real thing.
    node = dfu_node()
    if node and not os.access(node[2], os.W_OK):
        sys.exit(f"The board is already in DFU but {node[2]} is not writable by "
                 f"{os.environ.get('USER','this user')}.\n" + _perm_help())

    if node:
        return  # in DFU and writable — nothing to check

    # Not in DFU yet, so the node does not exist and we must predict. Either a rule grants
    # our group, or we own the active seat and `uaccess` will grant us an ACL.
    rule_ok = any("df11" in pathlib.Path(p).read_text(errors="ignore")
                  and ("GROUP" in pathlib.Path(p).read_text(errors="ignore"))
                  for p in glob.glob("/etc/udev/rules.d/*.rules") if os.path.isfile(p))
    seat_ok = False
    try:
        r = subprocess.run(["loginctl", "show-session", "self", "-p", "Remote", "-p", "Active"],
                           capture_output=True, text=True, timeout=5).stdout
        seat_ok = "Active=yes" in r and "Remote=no" in r
    except Exception:
        pass
    if not (rule_ok or seat_ok):
        sys.exit("Refusing to enter DFU: nothing would let this session write to the\n"
                 "bootloader device, and the board would be stuck there.\n" + _perm_help())


def _perm_help():
    return ("\nInstall the project's udev rule (once):\n"
            "  sudo cp tools/udev/70-jayecu.rules /etc/udev/rules.d/\n"
            "  sudo udevadm control --reload-rules && sudo udevadm trigger\n"
            "  # then unplug/replug the ECU\n"
            "Or run this from a terminal inside your desktop session, where dfu-util's\n"
            "own `uaccess` rule grants you an ACL.\n")


def enter_dfu():
    if dfu_node():
        print("already in DFU")
        return
    sys.path.insert(0, str(REPO / "tools"))
    from ts_bench import TsLink                                   # noqa: E402
    link = TsLink()
    print("ECU:", link.hello())
    print("sending `dfu` — the serial link will drop")
    try:
        link.execute("dfu")
    except Exception:
        pass                                                      # the reset pre-empts the reply
    for _ in range(50):
        time.sleep(0.2)
        if dfu_node():
            return
    sys.exit("The board never appeared as " + DFU_VID_PID + ".  Power-cycle it to return to the app.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", default="jaytek_v1")
    ap.add_argument("--bin", default=None, help="default: firmware/build/<board>/code.bin")
    a = ap.parse_args()
    img = pathlib.Path(a.bin) if a.bin else REPO / "firmware" / "build" / a.board / "code.bin"
    if not img.is_file():
        sys.exit(f"{img} not found — run:  make firmware BOARD={a.board}")

    preflight()
    enter_dfu()

    node = dfu_node()
    if node and not os.access(node[2], os.W_OK):
        # The prediction was wrong and the board IS in the bootloader. Say how to get out.
        sys.exit(f"{node[2]} is not writable, and the board is now in DFU.\n"
                 "POWER-CYCLE THE ECU to return it to the application — nothing was written.\n"
                 + _perm_help())

    print(f"writing {img} ({img.stat().st_size} bytes) to 0x08000000")
    r = subprocess.run(["dfu-util", "-a", "0", "-s", "0x08000000:leave", "-D", str(img)])
    if r.returncode != 0:
        sys.exit("\ndfu-util failed. The board may still be in DFU — power-cycle it to return\n"
                 "to the application. Nothing is written unless dfu-util reports success.\n")

    # Prove it took, rather than trusting the exit code.
    print("\nwaiting for the ECU to re-enumerate...")
    sys.path.insert(0, str(REPO / "tools"))
    for _ in range(40):
        time.sleep(0.5)
        try:
            from ts_bench import TsLink                           # noqa: E402
            print("ECU:", TsLink().hello())
            return
        except Exception:
            continue
    print("flashed, but the ECU did not answer in time — check it manually.")


if __name__ == "__main__":
    main()
