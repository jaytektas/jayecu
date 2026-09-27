#!/usr/bin/env python3
"""Reproduce + trace the 'b' (burn) command crash over the USB VCP.

The firmware's efi_log_usb sink emits text debug lines on the SAME VCP as the
binary protocol, so we just dump everything raw and let the interleaved
"[tick] burn: ..." lines show the last step reached before a crash.
"""
import binascii, struct, sys, time, glob
import serial

DEV = "/dev/ttyACM0"


def crc32(b): return binascii.crc32(b) & 0xFFFFFFFF


def envelope(payload: bytes) -> bytes:
    return struct.pack(">H", len(payload)) + payload + struct.pack(">I", crc32(payload))


def wait_port(timeout=10.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if glob.glob(DEV):
            return True
        time.sleep(0.2)
    return False


def dump(port, secs, label):
    """Read for `secs`, print raw repr + hex. Return total bytes."""
    print(f"--- {label}: reading for {secs}s ---")
    t0 = time.time()
    total = b""
    while time.time() - t0 < secs:
        try:
            chunk = port.read(256)
        except (serial.SerialException, OSError) as e:
            print(f"  !! serial error (device dropped?): {e}")
            return total, True
        if chunk:
            total += chunk
            # print decodable text inline; hex for the rest
            print(f"  RX[{len(chunk)}]: {chunk!r}")
        else:
            time.sleep(0.05)
    return total, False


def main():
    if not wait_port():
        print("Port never appeared"); sys.exit(1)
    time.sleep(0.5)
    port = serial.Serial(DEV, 115200, timeout=0.3)
    print(f"Opened {DEV}")

    # 1) Baseline: 'Q' handshake must work before we trust anything else.
    port.reset_input_buffer()
    port.write(envelope(b'Q'))
    base, dropped = dump(port, 1.5, "baseline 'Q'")
    if dropped:
        print("Device dropped on baseline 'Q' — link unstable before burn."); sys.exit(2)
    print(f"  baseline got {len(base)} bytes, contains jayecu={b'jayecu' in base}\n")

    # 2) The burn command.
    print(">>> sending 'b' (burn) envelope: b'b\\x00\\x01'")
    port.reset_input_buffer()
    port.write(envelope(b'b\x00\x01'))
    burn, dropped = dump(port, 5.0, "after 'b'")
    print(f"\n  after-burn got {len(burn)} bytes; device_dropped={dropped}")

    # 3) Liveness probe: if it crashed/reset, 'Q' will time out or the port drops.
    print("\n>>> liveness probe: 'Q' again")
    try:
        port.reset_input_buffer()
        port.write(envelope(b'Q'))
        alive, dropped2 = dump(port, 2.0, "post-burn 'Q'")
        if dropped2 or len(alive) == 0:
            print("  *** NO RESPONSE — MCU appears crashed/hung after burn ***")
        else:
            print(f"  alive, {len(alive)} bytes (jayecu={b'jayecu' in alive})")
    except (serial.SerialException, OSError) as e:
        print(f"  *** port unusable: {e} — MCU crashed ***")

    port.close()


if __name__ == "__main__":
    main()
