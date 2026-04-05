"""
send_image.py
-------------
Streams a raw image file to the ESP32 node over UART.
All chunking, CRC, base64 encoding and ACK/retry is handled entirely
by the node firmware (mesh_main.c).

Protocol (PC -> Node):
  IMG_FILE:<filename>:<total_bytes>\n
  <raw binary bytes>

Protocol (Node -> PC):
  IMG_ACK:OK    -- root confirmed all chunks received
  IMG_ACK:FAIL  -- transfer failed after max retries

ACK timeout scales with the sender node's mesh layer, matching the
firmware's dynamic timeout formula:
  layer_mult      = max(1, layer - 2)
  ack_timeout_ms  = 30s × layer_mult
  total_budget    = 37s send + 6 × ack_timeout + 13s headroom

  Layer 2/3 → ACK_TIMEOUT = 230s   (40 + 180 + 10)
  Layer 4   → ACK_TIMEOUT = 410s   (40 + 360 + 10)
  Layer 5   → ACK_TIMEOUT = 590s   (40 + 540 + 10)

Usage:
    python send_image.py <COM_PORT> <image_file> [mesh_layer]
    python send_image.py COM4 testImage.jpg        # default layer 3
    python send_image.py COM4 testImage.jpg 4      # layer 4 node
"""

import sys
import os
import serial
import time

BAUD_RATE   = 115200
WAIT_SECS   = 10

# Mirrors the firmware formula exactly:
#   ack_timeout  = 30s × max(1, layer - 2)
#   total_budget = 40s send + 6 × ack_timeout + 10s headroom
#   send budget  = 32s chunks + 5s handshake + 2.5s rounding buffer = 40s (rounded up)
#   firmware worst case: 40 + 180 = 220s  |  PC timeout: 220 + 10 = 230s
def ack_timeout_for_layer(layer: int) -> int:
    layer_mult  = max(1, layer - 2)
    ack_timeout = 30 * layer_mult
    return 40 + 6 * ack_timeout + 10

def transfer_image(port, filepath, mesh_layer=3):
    if not os.path.exists(filepath):
        print(f"Error: file not found: {filepath}")
        sys.exit(1)

    ACK_TIMEOUT = ack_timeout_for_layer(mesh_layer)

    filename    = os.path.basename(filepath)
    data        = open(filepath, 'rb').read()
    total_bytes = len(data)

    print(f"\nFile        : {filename}")
    print(f"Size        : {total_bytes} bytes")
    print(f"Port        : {port}")
    print(f"Mesh layer  : {mesh_layer}")
    print(f"ACK timeout : {ACK_TIMEOUT}s\n")

    ser = serial.Serial()
    ser.port     = port
    ser.baudrate = BAUD_RATE
    ser.timeout  = 1
    ser.open()
    ser.dtr = False
    ser.rts = False

    print(f"Waiting {WAIT_SECS}s for ESP32 to stabilise...")
    for i in range(WAIT_SECS, 0, -1):
        print(f"  {i}s...", end='\r')
        time.sleep(1)
    print("Ready.          ")

    try:
        # Send header line
        header = f"IMG_FILE:{filename}:{total_bytes}\n"
        t_start = time.time()
        ser.write(header.encode('utf-8'))
        ser.flush()
        time.sleep(0.1)

        # Stream raw bytes
        print(f"Sending {total_bytes} bytes...")
        ser.write(data)
        ser.flush()
        print("Done. Waiting for ACK from node...\n")

        # Wait for ACK written back by node after mesh transfer completes
        deadline = time.time() + ACK_TIMEOUT
        while time.time() < deadline:
            line = ser.readline().decode('utf-8', errors='replace').strip()
            if not line:
                continue
            print(f"  [NODE] {line}")
            if line == "IMG_ACK:OK":
                elapsed = time.time() - t_start
                print(f"\nTransfer confirmed by root.")
                print(f"Time taken: {elapsed:.1f}s")
                break
            elif line == "IMG_ACK:FAIL":
                elapsed = time.time() - t_start
                print(f"\nTransfer failed — check root monitor for details.")
                print(f"Time taken: {elapsed:.1f}s")
                break
        else:
            elapsed = time.time() - t_start
            print(f"\nTimeout — no ACK received within {ACK_TIMEOUT}s.")
            print(f"Time taken: {elapsed:.1f}s")

    except serial.SerialException as e:
        print(f"Serial error: {e}")
        print("Ensure idf.py monitor is not running.")
    finally:
        ser.close()

if __name__ == '__main__':
    if len(sys.argv) not in (3, 4):
        print("Usage: python send_image.py <COM_PORT> <image_file> [mesh_layer]")
        print("Example: python send_image.py COM4 testImage.jpg")
        print("Example: python send_image.py COM4 testImage.jpg 4")
        sys.exit(1)
    layer = int(sys.argv[3]) if len(sys.argv) == 4 else 3
    transfer_image(sys.argv[1], sys.argv[2], layer)
