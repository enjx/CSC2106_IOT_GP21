# ESP-MESH Network Implementation

A mesh networking system using WiFi radios (ESP32) with the ESP-IDF ESP-MESH protocol for automatic routing and packet forwarding.

## Overview

This implementation demonstrates an ESP-MESH network capable of:

- Image transfer across multiple hops with automatic retries and CRC32 verification
- Real-time performance metrics including RSSI, latency, and throughput
- Benchmark testing across multiple images with CSV result logging
- LCD status display on each node showing mesh state and signal strength

## Architecture

**Mesh Protocol:** ESP-IDF ESP-MESH handles automatic routing and packet forwarding  
**Frequency:** 2.4 GHz (802.11 b/g/n)  
**Node Types:** Sender nodes and a fixed root (gateway) node  
**Chunk Size:** 800 bytes per mesh packet  
**Packet Structure:** Session ID, chunk index, total chunks, CRC32, and Base64-encoded payload

## Key Features

- **Automatic Routing:** ESP-MESH automatically routes packets to the root across multiple hops
- **Reliability:** Selective ARQ retry — root identifies missing chunks and node retransmits only those, up to 6 attempts
- **Data Integrity:** CRC32 per chunk + full file CRC32 verification
- **Exclusive Lock:** Only one node transfers at a time via distributed mutex (IMG_REQ/GRANT/BUSY)
- **Performance Monitoring:** Real-time RSSI, latency, throughput, and packet loss metrics
- **Session IDs:** Prevents stale chunks from a crashed transfer contaminating a new one

## Hardware Requirements

- **M5StickC Plus** × 3–4 (ESP32, ST7789 135×240 LCD, AXP192 PMIC, 2 buttons)
- **USB-C cable** × 1 per node
- **2 PCs** — one connected to sender node, one connected to root node

## Software Requirements

- VSCode with **ESP-IDF extension** (Espressif Systems) — v5.5.3
- Python 3.x with `pyserial` library: `pip install pyserial`

## Configuration

Open menuconfig: `Ctrl+Shift+P` → `ESP-IDF: SDK Configuration Editor` → **Mesh Configuration**

```
MESH_IS_ROOT      y (root only) / n (all other nodes)
MESH_ID           Same 6-character string on all nodes
MESH_CHANNEL      Same WiFi channel on all nodes (default: 1)
MESH_AP_MAX_CONN  Max children per node (default: 2)
LED_GPIO          GPIO10 on M5StickC Plus
```

**Important:**
- Two separate firmware binaries must be built and flashed
- Root firmware: `MESH_IS_ROOT = y`
- Node firmware: `MESH_IS_ROOT = n`
- All other settings must be identical across all nodes

## Usage & Test Cases

### Image Transfer

1. Configure `MESH_IS_ROOT` and flash root firmware to one M5StickC Plus
2. Flash node firmware to all remaining M5StickC Plus units
3. Power all devices — mesh forms automatically within ~15 seconds
4. **On receiver PC** (root node connected): run `receive_image.py`
5. **On sender PC** (sender node connected): run `send_image.py`

```bash
# Receiver PC — leave running throughout
python receive_image.py COM5

# Sender PC
python send_image.py COM4 testImage.jpg
```

**Outcome:** Received image appears in the `output/` folder. Sender PC prints live firmware logs and reports total time taken.

---

### Benchmark Testing

Sends multiple images sequentially and logs performance metrics to a timestamped CSV.

1. Ensure `receive_image.py` is running on the receiver PC
2. Run benchmark on the sender PC:

```bash
python benchmark.py COM4 photo1.jpg photo2.jpg photo3.jpg photo4.jpg photo5.jpg
```

**Outcome:** A `benchmark_YYYYMMDD_HHMMSS.csv` is saved with per-transfer metrics.

**Metrics displayed:**
- Latency (seconds)
- Throughput (bytes per second)
- Packet loss % (chunks missed on initial send)
- Retry rounds and extra chunks retransmitted
- ACK timeouts
- All recovered (YES/NO)

---

### RSSI & Signal Testing

Each non-root node displays live RSSI to its current parent on the LCD, updated every 2 seconds.

- **Green:** > −60 dBm (strong)
- **Yellow:** −60 to −75 dBm (moderate)
- **Red:** < −75 dBm (weak)

Move nodes to different positions and observe RSSI changes. Note that placing all nodes in close proximity causes co-channel interference on the shared 2.4 GHz channel, degrading RSSI even at short range.

## Benchmark Results

Results from 2026-03-31 — Layer 3 node, stable mesh, no disconnections.

| Run | File | Size (B) | Status | Time (s) | B/s | Loss% | Recovered |
|---|---|---|---|---|---|---|---|
| 1 | photo1.jpg | 17,404 | OK | 53.88 | 323.0 | 0.0 | YES |
| 2 | photo2.jpg | 32,146 | OK | 64.09 | 501.6 | 0.0 | YES |
| 3 | photo3.jpg | 23,625 | OK | 56.03 | 421.7 | 0.0 | YES |
| 4 | photo4.jpg | 36,153 | OK | 84.44 | 428.1 | 0.0 | YES |
| 5 | photo5.jpg | 44,919 | OK | 83.82 | 535.9 | 0.0 | YES |

**Averages:** 68.45s | 442.1 B/s | 0.0% loss | 5/5 fully recovered

## Implementation Details

### Packet Structure

Each mesh packet contains:

| Field | Size | Description |
|---|---|---|
| Session ID | 4 bytes | Monotonic counter — discards stale chunks from previous transfers |
| Chunk index | variable | 0-based internally, 1-based in RETRY list |
| Total chunks | variable | Total number of chunks for this image |
| CRC32 | 4 bytes | Per-chunk integrity check |
| Base64 payload | ≤ 1068 bytes | Up to 800 bytes raw data encoded as Base64 |

### Sender Node Behaviour

- Receives image binary over UART from `send_image.py`
- Acquires exclusive mesh lock from root via IMG_REQ/GRANT before sending
- Fragments image into 800-byte chunks, CRC32-verified and Base64-encoded
- Sends all chunks then waits for ACK from root
- On IMG_ACK:RETRY — retransmits only the listed missing chunks
- On IMG_ACK:OK — releases lock and reports success to PC via UART

### Root Node Behaviour

- Continuously listens for incoming mesh packets
- Validates CRC32 per chunk and discards corrupted data
- Tracks which chunks have arrived via `received[64]` boolean array
- Replies IMG_ACK:RETRY with missing 1-based indices, or IMG_ACK:OK on completion
- Streams reassembled image to PC over UART as `IMG_SAVE:<filename>:<size>`
- Displays RSSI, latency, and throughput in firmware logs

### Data Flow

```
Image File → send_image.py → UART → Sender Node → ESP-MESH → Root Node → UART → receive_image.py → Saved Image
```

## Performance Metrics

The system provides real-time monitoring of:

- **RSSI:** Signal strength to current parent (dBm) — displayed on node LCD
- **Latency:** Total transfer time from first byte sent to IMG_ACK:OK received
- **Throughput:** Image bytes / latency (bytes per second)
- **Packet loss:** Chunks not delivered on initial send pass (%)
- **Retry rounds:** Number of selective retransmission rounds required

## Troubleshooting

| Problem | Fix |
|---|---|
| Node stuck on SEARCHING | Check all nodes have matching MESH_ID and MESH_CHANNEL |
| UART read timeout | Ensure `idf.py monitor` is not running on the same port |
| Transfer FAIL | Move nodes closer; try a different WiFi channel |
| benchmark.py times out | Pass `--layer 4` if node is deeper than layer 3 |
| Wrong LCD colours | Flash latest firmware — old RGB() macro produced incorrect values |
| Duplicate saved images | Flash latest firmware — img_saved guard fixes Layer 3 ACK latency issue |

## Files

| File | Description |
|---|---|
| `main/mesh_main.c` | Main ESP32 firmware for both sender and root nodes |
| `send_image.py` | Python script to send image files to sender node via UART |
| `receive_image.py` | Python script to receive and save images from root node via UART |
| `benchmark.py` | Python script to benchmark multiple images and log metrics to CSV |
| `testImage.jpg` | Sample image for testing (11.78 KB, 16 chunks) |
| `photo1–5.jpg` | Benchmark test images (17–44 KB) |
