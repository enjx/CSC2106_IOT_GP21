# ESP-MESH Image Transfer

CSC2106 IoT Protocols and Networks — Team GP21

Infrastructure-less WiFi mesh image transfer using ESP-IDF ESP-MESH on M5StickC Plus.

---

## Hardware

- M5StickC Plus × 3–4 nodes
- USB-C cable per node
- 2 PCs (one for sender node, one for root node)

---

## Environment Setup

1. Install **ESP-IDF extension** in VSCode (by Espressif Systems)
2. Configure via `Ctrl+Shift+P` → `ESP-IDF: Configure ESP-IDF Extension` → Express → **v5.5.3**
3. Open project folder in VSCode
4. Set target: `Ctrl+Shift+P` → `ESP-IDF: Set Espressif Device Target` → **esp32**
5. Install Python dependency: `pip install pyserial`

---

## Configuration (menuconfig)

`Ctrl+Shift+P` → `ESP-IDF: SDK Configuration Editor` → **Mesh Configuration**

| Setting | Root node | Other nodes |
|---|---|---|
| `MESH_IS_ROOT` | `y` | `n` |
| `MESH_ID` | Same on all | Same on all |
| `MESH_CHANNEL` | Same on all | Same on all |

Two separate firmware binaries must be built — one with `MESH_IS_ROOT=y`, one with `MESH_IS_ROOT=n`.

---

## Flashing

1. Build: click **Build** (hammer) in status bar
2. Select COM port in status bar
3. Flash: click **Flash** (lightning bolt)
4. Monitor: click **Monitor** (plug icon)

Flash root node first, then all other nodes. Mesh forms automatically within ~15 seconds.

> Close monitor before flashing — they share the same COM port.

---

## Running a Transfer

**On receiver PC (root node):**
```bash
python receive_image.py COM5
```
Leave this running. It saves received images to the `output/` folder automatically.

**On sender PC (sender node):**
```bash
python send_image.py COM4 testImage.jpg
```
Waits 10s for ESP32 to stabilise, sends the image, prints live firmware logs, reports time taken.

---

## Benchmark Testing

Sends multiple images sequentially and logs metrics to a timestamped CSV.

```bash
python benchmark.py COM4 photo1.jpg photo2.jpg photo3.jpg photo4.jpg photo5.jpg
```

Metrics logged per transfer: latency (s), throughput (B/s), packet loss (%), retry rounds, extra chunks retransmitted, ACK timeouts, all recovered (YES/NO).

### Results (2026-03-31, Layer 3, no disconnections)

| File | Size | Time (s) | B/s | Loss% | Recovered |
|---|---|---|---|---|---|
| photo1.jpg | 17,404 | 53.88 | 323.0 | 0.0 | YES |
| photo2.jpg | 32,146 | 64.09 | 501.6 | 0.0 | YES |
| photo3.jpg | 23,625 | 56.03 | 421.7 | 0.0 | YES |
| photo4.jpg | 36,153 | 84.44 | 428.1 | 0.0 | YES |
| photo5.jpg | 44,919 | 83.82 | 535.9 | 0.0 | YES |

**Average:** 68.45s | 442.1 B/s | 0.0% loss | 5/5 fully recovered

---

## LCD Display

Each node shows: MAC, ROLE, LAYER, CHLD (children count), STATUS, RSSI (non-root only).

RSSI colour: Green (> −60 dBm) / Yellow (−60 to −75) / Red (< −75)

**Buttons:** A = send test message | B = force disconnect/rejoin

---

## Known Limitations

- Max image size: 50KB
- One transfer at a time (exclusive lock)
- No crash recovery — reboot loses transfer state
- No security — open mesh, no encryption
- Root is a single point of failure

---

## Troubleshooting

| Problem | Fix |
|---|---|
| Node stuck on SEARCHING | Check all nodes have matching MESH_ID and MESH_CHANNEL |
| UART read timeout | Ensure `idf.py monitor` is not running on the same port |
| Transfer FAIL | Move nodes closer; try a different WiFi channel |
| benchmark.py times out | Pass `--layer 4` if node is deeper than layer 3 |
| Wrong LCD colours | Flash latest firmware — old RGB() macro produced incorrect values |
