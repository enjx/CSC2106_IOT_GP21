"""
benchmark.py
------------
Sends a list of images over UART to the ESP32 sender node sequentially
and logs performance metrics for each transfer.

Metrics captured per transfer:
  - Latency          : total time from first byte sent to IMG_ACK:OK/FAIL (seconds)
  - Throughput       : image_bytes / latency (bytes/s)
  - Packet loss      : chunks not delivered on initial send / total chunks (%)
                       parsed from: "Initial send: X/N chunks delivered"
  - Retry rounds     : number of RETRY responses received from root
                       parsed from: "Retransmitted N missing chunks"
  - Retry chunks     : total extra chunk transmissions across all retry rounds
  - ACK timeouts     : number of 30s ACK attempts that expired with no response
                       parsed from: "ACK timeout (attempt N)"
  - All recovered    : YES if transfer ended IMG_ACK:OK, NO otherwise

Results are written to benchmark_results.csv in the same directory.

The mesh layer is detected automatically from the firmware UART logs
("PARENT_CONNECTED — layer N"). Use --layer only to override if detection fails.

Usage:
    python benchmark.py <COM_PORT> <image1> [image2 ...] [--layer N]
    python benchmark.py COM4 img1.jpg img2.jpg img3.jpg img4.jpg img5.jpg
    python benchmark.py COM4 img1.jpg img2.jpg --layer 4  # manual override
"""

import sys
import os
import serial
import time
import csv
import re
from datetime import datetime

BAUD_RATE      = 115200
WAIT_SECS      = 10   # only applied before the very first transfer
INTER_IMAGE_S  = 15   # pause between images — enough for mesh to stabilise
                      # after any disconnect that occurred during the previous transfer
CHUNK_SIZE     = 800  # must match IMG_CHUNK_SIZE in firmware

def ack_timeout_for_layer(layer: int) -> int:
    layer_mult  = max(1, layer - 2)
    ack_timeout = 30 * layer_mult
    return 40 + 6 * ack_timeout + 10

def num_chunks(total_bytes: int) -> int:
    return (total_bytes + CHUNK_SIZE - 1) // CHUNK_SIZE

def detect_layer(ser, timeout_s: int = 15) -> int:
    """Read UART output and detect mesh layer from firmware logs.
    Parses: 'PARENT_CONNECTED — layer N'
    Returns detected layer or 3 as fallback."""
    print(f"Detecting mesh layer (waiting up to {timeout_s}s)...")
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if not line:
            continue
        m = re.search(r'PARENT_CONNECTED.*layer\s+(\d+)', line)
        if m:
            layer = int(m.group(1))
            print(f"  Detected mesh layer: {layer}")
            return layer
    print("  Layer detection timed out — defaulting to layer 3")
    return 3

def send_one(ser, filepath: str, mesh_layer: int, is_first: bool) -> dict:
    """Send a single image and return its metrics dict."""
    filename    = os.path.basename(filepath)
    data        = open(filepath, 'rb').read()
    total_bytes = len(data)
    expected_chunks = num_chunks(total_bytes)
    mesh_layer  = mesh_layer          # local mutable copy — may update on reconnect
    ack_timeout = ack_timeout_for_layer(mesh_layer)

    print(f"\n{'='*55}")
    print(f"  File     : {filename}  ({total_bytes} bytes, {expected_chunks} chunks)")
    print(f"  Timeout  : {ack_timeout}s")
    print(f"{'='*55}")

    if is_first:
        print(f"Waiting {WAIT_SECS}s for ESP32 to stabilise...")
        for i in range(WAIT_SECS, 0, -1):
            print(f"  {i}s...", end='\r')
            time.sleep(1)
        print("Ready.          ")
    else:
        print(f"Pausing {INTER_IMAGE_S}s before next image...")
        time.sleep(INTER_IMAGE_S)

    # Flush any stale bytes
    ser.reset_input_buffer()

    header = f"IMG_FILE:{filename}:{total_bytes}\n"
    t_start = time.time()
    ser.write(header.encode('utf-8'))
    ser.flush()
    time.sleep(0.1)
    ser.write(data)
    ser.flush()
    print(f"Sent {total_bytes} bytes. Waiting for ACK...\n")

    result = {
        'filename'        : filename,
        'size_bytes'      : total_bytes,
        'status'          : 'TIMEOUT',
        'latency_s'       : None,
        'throughput_bps'  : None,
        'initial_sent'    : None,
        'total_chunks'    : expected_chunks,
        'packet_loss_pct' : None,
        'retry_rounds'    : 0,
        'retry_chunks'    : 0,
        'ack_timeouts'    : 0,
        'all_recovered'   : 'NO',
    }

    deadline = time.time() + ack_timeout
    while time.time() < deadline:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if not line:
            continue
        print(f"  [NODE] {line}")

        # Layer change mid-transfer: "PARENT_CONNECTED — layer N"
        # Update ack_timeout if node reconnects at a different layer
        m = re.search(r'PARENT_CONNECTED.*layer\s+(\d+)', line)
        if m:
            new_layer = int(m.group(1))
            if new_layer != mesh_layer:
                mesh_layer = new_layer
                ack_timeout = ack_timeout_for_layer(mesh_layer)
                deadline    = time.time() + ack_timeout
                print(f"  [LAYER CHANGE] Now layer {mesh_layer} — ACK timeout updated to {ack_timeout}s")

        # Initial send delivery: "Initial send: 7/16 chunks delivered"
        m = re.search(r'Initial send:\s*(\d+)/(\d+)\s*chunks delivered', line)
        if m:
            result['initial_sent'] = int(m.group(1))
            result['total_chunks'] = int(m.group(2))

        # Retry round: "Retransmitted 9 missing chunks"
        m = re.search(r'Retransmitted\s+(\d+)\s+missing chunks', line)
        if m:
            result['retry_rounds']  += 1
            result['retry_chunks']  += int(m.group(1))

        # ACK timeout: "ACK timeout (attempt 3)"
        if re.search(r'ACK timeout \(attempt \d+\)', line):
            result['ack_timeouts'] += 1

        # All chunks confirmed: "IMG_COMPLETE -- root confirmed all chunks OK"
        if 'IMG_COMPLETE' in line:
            result['all_recovered'] = 'YES'

        if line == "IMG_ACK:OK":
            elapsed = time.time() - t_start
            result['status']         = 'OK'
            result['latency_s']      = round(elapsed, 2)
            result['throughput_bps'] = round(total_bytes / elapsed, 1)
            if result['initial_sent'] is not None:
                missed = result['total_chunks'] - result['initial_sent']
                result['packet_loss_pct'] = round(missed / result['total_chunks'] * 100, 1)
            else:
                result['packet_loss_pct'] = 0.0
            print(f"\n  ✓ OK  — {elapsed:.1f}s  |  "
                  f"{result['throughput_bps']} B/s  |  "
                  f"loss {result['packet_loss_pct']}%  |  "
                  f"{result['retry_rounds']} retry rounds  |  "
                  f"{result['retry_chunks']} extra chunks  |  "
                  f"{result['ack_timeouts']} ACK timeouts")
            break

        elif line == "IMG_ACK:FAIL":
            elapsed = time.time() - t_start
            result['status']    = 'FAIL'
            result['latency_s'] = round(elapsed, 2)
            if result['initial_sent'] is not None:
                missed = result['total_chunks'] - result['initial_sent']
                result['packet_loss_pct'] = round(missed / result['total_chunks'] * 100, 1)
            print(f"\n  ✗ FAIL — {elapsed:.1f}s")
            break
    else:
        elapsed = time.time() - t_start
        result['latency_s'] = round(elapsed, 2)
        print(f"\n  ✗ TIMEOUT after {elapsed:.1f}s")

    return result


def run_benchmark(port: str, mesh_layer: int, image_paths: list):
    timestamp  = datetime.now().strftime('%Y%m%d_%H%M%S')
    csv_path   = os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        f"benchmark_{timestamp}.csv"
    )

    ser = serial.Serial()
    ser.port     = port
    ser.baudrate = BAUD_RATE
    ser.timeout  = 1
    ser.open()
    ser.dtr = False
    ser.rts = False

    # Auto-detect layer from firmware logs unless manually overridden
    if mesh_layer == 0:
        mesh_layer = detect_layer(ser)
    print(f"\nBenchmark — {len(image_paths)} images — Layer {mesh_layer} — Port {port}")
    print(f"Results will be saved to: {csv_path}\n")

    results = []
    for i, path in enumerate(image_paths):
        if not os.path.exists(path):
            print(f"[SKIP] File not found: {path}")
            continue
        r = send_one(ser, path, mesh_layer, is_first=(i == 0))
        r['run'] = i + 1
        results.append(r)

    ser.close()

    # Write CSV
    fields = ['run', 'filename', 'size_bytes', 'status',
              'latency_s', 'throughput_bps',
              'initial_sent', 'total_chunks', 'packet_loss_pct',
              'retry_rounds', 'retry_chunks', 'ack_timeouts', 'all_recovered']
    with open(csv_path, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(results)

    # Print summary table
    print(f"\n{'='*55}")
    print(f"  BENCHMARK SUMMARY")
    print(f"{'='*55}")
    print(f"  {'#':<4} {'File':<18} {'Status':<7} {'Time(s)':<9} {'B/s':<9} {'Loss%':<8} {'Retries':<9} {'ExtraChk':<10} {'AckTO':<7} {'Recovered'}")
    print(f"  {'-'*90}")
    for r in results:
        print(f"  {r['run']:<4} {r['filename'][:17]:<18} {r['status']:<7} "
              f"{str(r['latency_s']):<9} {str(r['throughput_bps']):<9} "
              f"{str(r['packet_loss_pct']):<8} {str(r['retry_rounds']):<9} "
              f"{str(r['retry_chunks']):<10} {str(r['ack_timeouts']):<7} "
              f"{r['all_recovered']}")
    print(f"{'='*55}")

    ok_results = [r for r in results if r['status'] == 'OK']
    if ok_results:
        avg_lat    = round(sum(r['latency_s']       for r in ok_results) / len(ok_results), 2)
        avg_thr    = round(sum(r['throughput_bps']  for r in ok_results) / len(ok_results), 1)
        avg_loss   = round(sum(r['packet_loss_pct'] for r in ok_results) / len(ok_results), 1)
        avg_retry  = round(sum(r['retry_rounds']    for r in ok_results) / len(ok_results), 1)
        avg_chunks = round(sum(r['retry_chunks']    for r in ok_results) / len(ok_results), 1)
        avg_ackto  = round(sum(r['ack_timeouts']    for r in ok_results) / len(ok_results), 1)
        recovered  = sum(1 for r in ok_results if r['all_recovered'] == 'YES')
        print(f"  Averages ({len(ok_results)}/{len(results)} OK): "
              f"{avg_lat}s  |  {avg_thr} B/s  |  {avg_loss}% loss  |  "
              f"{avg_retry} retry rounds  |  {avg_chunks} extra chunks  |  "
              f"{avg_ackto} ACK timeouts  |  {recovered}/{len(ok_results)} fully recovered")
    print(f"\n  Full results saved to: {csv_path}\n")


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("Usage: python benchmark.py <COM_PORT> <img1> [img2 ...] [--layer N]")
        print("Example: python benchmark.py COM4 img1.jpg img2.jpg img3.jpg")
        print("Example: python benchmark.py COM4 img1.jpg img2.jpg --layer 4  # override auto-detect")
        sys.exit(1)
    port   = sys.argv[1]
    # layer=0 means auto-detect from firmware logs
    # only set manually if --layer flag is provided
    layer  = 0
    images = []
    args   = sys.argv[2:]
    i = 0
    while i < len(args):
        if args[i] == '--layer' and i + 1 < len(args):
            layer = int(args[i + 1])
            i += 2
        else:
            images.append(args[i])
            i += 1
    if not images:
        print("Error: no image files specified.")
        sys.exit(1)
    run_benchmark(port, layer, images)
