#!/usr/bin/env python3
"""
capture_serial.py — Phase 14: reset the board and capture one boot to logs/<tag>.txt

Replaces idf.py monitor (which triggers a slow cmake reconfigure) with a direct
pyserial capture. The reset replicates esptool's USB-Serial-JTAG HardReset
sequence (RTS high -> 200 ms -> RTS low).

Usage:
    capture_serial.py <tag> [timeout_s] [port] [baud]
"""
import argparse
import os
import sys
import time

import serial

DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_BAUD = 921600


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tag", help="log tag, written to logs/<tag>.txt")
    ap.add_argument("--timeout", type=float, default=20.0, help="capture window (s)")
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    args = ap.parse_args()

    logs_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs")
    os.makedirs(logs_dir, exist_ok=True)
    out_path = os.path.join(logs_dir, args.tag + ".txt")

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.2
    ser.open()
    ser.setDTR(False)
    ser.setRTS(False)
    time.sleep(0.2)

    # esptool USB-Serial-JTAG HardReset: RTS assert -> release -> boot
    print(f"[INFO] resetting {args.port} @ {args.baud} baud ...")
    ser.setRTS(True)
    time.sleep(0.2)
    ser.setRTS(False)

    start = time.time()
    out = []
    while time.time() - start < args.timeout:
        try:
            data = ser.read(2048)
        except serial.SerialException as exc:
            out.append(f"\n[SERIAL ERROR] {exc}\n")
            break
        if data:
            out.append(data.decode("utf-8", errors="replace"))
        else:
            time.sleep(0.05)

    ser.close()
    text = "".join(out)
    with open(out_path, "w") as fh:
        fh.write(text)
    print(f"[PASS] log written: {out_path} ({len(text)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
