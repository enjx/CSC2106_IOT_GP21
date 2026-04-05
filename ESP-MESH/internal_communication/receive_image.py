"""
receive_image.py — listens on the ROOT node's serial port and saves
any image streamed by the firmware after a successful mesh transfer.

Usage:
    python receive_image.py [PORT] [--out DIR]

Examples:
    python receive_image.py COM5
    python receive_image.py COM5 --out C:/Users/Ethan/Pictures/mesh

The root firmware sends:
    IMG_SAVE:<filename>:<size>\n
    <raw bytes>

All other lines (ESP-IDF log output) are printed to the console
so you can keep monitoring root activity in the same window.
"""

import argparse
import os
import sys
import time
import serial

BAUD      = 115200
TIMEOUT_S = 5      # seconds to wait for the size bytes once header is seen


def main():
    parser = argparse.ArgumentParser(description="Receive images from root ESP32 over serial")
    parser.add_argument("port", help="Serial port (e.g. COM5 or /dev/ttyUSB0)")
    parser.add_argument("--out", default="output", help="Output directory (default: output/)")
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)

    print(f"[receive_image] Opening {args.port} @ {BAUD} baud")
    print(f"[receive_image] Saving images to: {os.path.abspath(args.out)}")
    print(f"[receive_image] Monitoring... (Ctrl-C to stop)\n")

    ser = serial.Serial()
    ser.port     = args.port
    ser.baudrate = BAUD
    ser.timeout  = 0.1
    ser.dtr      = False
    ser.rts      = False
    ser.open()
    ser.dtr = False
    ser.rts = False

    line_buf = bytearray()

    try:
        while True:
            byte = ser.read(1)
            if not byte:
                continue

            if byte == b'\n':
                line = line_buf.decode("utf-8", errors="replace").rstrip("\r")
                line_buf.clear()

                if line.startswith("IMG_SAVE:"):
                    # Parse header: IMG_SAVE:<filename>:<size>
                    parts = line.split(":", 2)  # ["IMG_SAVE", "<filename>", "<size>"]
                    if len(parts) != 3:
                        print(f"[receive_image] Malformed IMG_SAVE header: {line!r}")
                        continue

                    filename = parts[1]
                    try:
                        total = int(parts[2])
                    except ValueError:
                        print(f"[receive_image] Bad size in header: {line!r}")
                        continue

                    print(f"\n[receive_image] *** Receiving '{filename}' ({total} bytes) ***")

                    # Read exactly `total` raw bytes
                    img_data = bytearray()
                    deadline = time.time() + TIMEOUT_S + total / BAUD * 10
                    ser.timeout = 2.0  # longer timeout for bulk read
                    while len(img_data) < total:
                        remaining = total - len(img_data)
                        chunk = ser.read(min(remaining, 512))
                        if not chunk:
                            if time.time() > deadline:
                                print(f"[receive_image] TIMEOUT after {len(img_data)}/{total} bytes")
                                break
                        else:
                            img_data.extend(chunk)
                    ser.timeout = 0.1

                    if len(img_data) == total:
                        out_path = os.path.join(args.out, filename)
                        if os.path.exists(out_path):
                            name, ext = os.path.splitext(filename)
                            counter = 2
                            while os.path.exists(os.path.join(args.out, f"{name}({counter}){ext}")):
                                counter += 1
                            out_path = os.path.join(args.out, f"{name}({counter}){ext}")
                        with open(out_path, "wb") as f:
                            f.write(img_data)
                        print(f"[receive_image] Saved → {out_path}")
                    else:
                        print(f"[receive_image] INCOMPLETE — only got {len(img_data)}/{total} bytes, discarding")

                else:
                    # Regular ESP-IDF log line — just print it
                    print(line)

            else:
                line_buf.extend(byte)

    except KeyboardInterrupt:
        print("\n[receive_image] Stopped.")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
