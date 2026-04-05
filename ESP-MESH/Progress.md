# ESP-MESH Project Progress Report
### CSC2106 IoT Protocols and Networks — Team GP21

---

## Milestone 1 — First Successful Image Transfer

**What happened:**
Completed the first end-to-end image transfer using `testImage.jpg` (12,059 bytes) across a 3-layer mesh. The sender node at Layer 3 fragmented and transmitted the image, the root reassembled it, and streamed it back to the receiver PC via UART.

---

## Milestone 2 — Custom Fragmentation Protocol Implemented

**What happened:**
A fully custom application-layer protocol was designed and implemented in `mesh_main.c`. ESP-IDF's ESP-MESH stack provides only a raw byte pipeline — no message types, fragmentation, reliability, or session state.

**Protocol messages built:**
- `IMG_REQ / IMG_GRANT / IMG_BUSY` — distributed exclusive lock
- `IMG_START` — transfer metadata (session ID, filename, size, chunks, file CRC32)
- `IMG_DATA` — per-chunk (0-based index, CRC32, Base64-encoded 800B payload)
- `IMG_END` — trigger root reassembly check
- `IMG_ACK:OK / IMG_ACK:RETRY` — selective retransmission with 1-based missing indices
- `IMG_RELEASE` — lock release with OK/FAIL result

---

## Milestone 3 — ARQ Retry Proven Under Real Disconnection

**What happened:**
Button B on the M5StickC Plus was used mid-transfer to force `esp_mesh_disconnect()`, deliberately simulating a link failure. The selective ARQ retry mechanism recovered all missing chunks without data corruption or transfer failure.

**Why it matters:**
Validates the protocol handles real-world intermittent connectivity — the primary requirement for a disaster-zone deployment scenario.

---

## Milestone 4 — UART Buffer Overflow Bug Found and Fixed

**What happened:**
Early testing with a 12KB image produced only 2,255 bytes received — less than 20% of the file. The UART driver buffer (2,048 bytes) overflowed within 178ms of the PC's full burst at 115,200 baud.

**Fix:**
Sized the UART driver buffer to `IMG_MAX_BYTES + 512 = 51,712 bytes`, sufficient to absorb the entire image burst before the firmware drain loop processes it.

---

## Milestone 5 — Duplicate Image Save Bug Found and Fixed

**What happened:**
During Layer 3 testing the root node saved three copies of the same image in a single transfer. Layer 3 ACK round-trip latency caused the node to time out and retransmit `IMG_END` twice more. The root had no guard against saving on repeated `IMG_END` with `missing=0`.

**Fix:**
Added `img_saved` boolean flag per session. Root resends `IMG_ACK:OK` on duplicate `IMG_END` frames but only streams `IMG_SAVE` once per session.

---

## Milestone 6 — Session ID Ghost Chunk Protection

**What happened:**
Identified that if a node crashes mid-transfer and restarts, chunks still in-flight from the dead session could arrive at the root during a new transfer and corrupt the reassembly buffer.

**Fix:**
Each transfer increments a monotonic `s_session_counter`. Root discards any `IMG_DATA` frame whose session ID does not match the active session, preventing cross-transfer contamination entirely.

---

## Milestone 7 — 0-based vs 1-based Index Bug Fixed

**What happened:**
The RETRY list was built using 0-based internal chunk indices but chunk progress logs printed `idx + 1`. The log line `RETRY:1,3,8,9,15` was actually requesting chunks 2,4,9,10,16 — confirmed by matching subsequent retry logs.

**Fix:**
Root adds 1 when building the RETRY string. Node subtracts 1 when parsing. RETRY indices are now human-readable and consistent with chunk progress logs.

---

## Milestone 8 — Dynamic ACK Timeout Scaling with Mesh Layer

**What happened:**
A fixed 30-second ACK timeout was insufficient for deeper mesh topologies. A dynamic formula was derived and implemented:

```
layer_mult  = max(1, layer - 2)
ack_timeout = 30s × layer_mult
pc_timeout  = 40s + (6 × ack_timeout) + 10s
```

| Layer | PC Timeout |
|---|---|
| 2 / 3 | 230s |
| 4 | 410s |
| 5 | 590s |

`benchmark.py` auto-detects the mesh layer from firmware UART logs and updates the timeout live if the node reconnects at a different layer mid-transfer.

---

## Milestone 9 — Variable Send Phase Eliminated

**What happened:**
The original `mesh_send_to_root()` waited up to 5 seconds per disconnected chunk hoping for reconnection, adding up to 320 seconds of unpredictable time to the send phase in worst case — making the timeout budget impossible to calculate.

**Fix:**
Removed the wait entirely. Disconnected chunks return `false` immediately and appear in the RETRY list. The send budget is now a fixed 40 seconds regardless of disconnection events.

**Send budget breakdown:**
- 64 chunks × 500ms rate limit = 32s
- 5 handshake messages × 500ms = 2.5s
- IMG_GRANT wait (up to 3s) = 3s
- 2.5s rounding buffer = 2.5s
- **Total = 40s fixed**

---

## Milestone 10 — Benchmark Tool with Full Metrics Logging

**What happened:**
`benchmark.py` was developed to automate sequential multi-image transfers and log seven performance metrics per transfer to a timestamped CSV.

**Metrics:** latency (s), throughput (B/s), initial packet loss (%), retry rounds, extra chunks retransmitted, ACK timeouts, all recovered (YES/NO).

**First benchmark run results (2026-03-31, Layer 3):**

| File | Size | Time (s) | B/s | Loss% | Recovered |
|---|---|---|---|---|---|
| photo1.jpg | 17,404 B | 53.88 | 323.0 | 0.0 | YES |
| photo2.jpg | 32,146 B | 64.09 | 501.6 | 0.0 | YES |
| photo3.jpg | 23,625 B | 56.03 | 421.7 | 0.0 | YES |
| photo4.jpg | 36,153 B | 84.44 | 428.1 | 0.0 | YES |
| photo5.jpg | 44,919 B | 83.82 | 535.9 | 0.0 | YES |

**Average:** 68.45s | 442.1 B/s | 0.0% loss | 5/5 fully recovered — **100% PDR achieved.**

---

## Milestone 11 — RSSI Display Added to Non-Root Nodes

**What happened:**
`esp_wifi_sta_get_rssi()` was integrated into the display task, showing each node's parent signal strength in real time updated every 2 seconds with colour coding.

**Key observation:**
Placing all nodes in close physical proximity caused co-channel interference on the shared 2.4GHz channel — RSSI degraded even as physical distance decreased. This is documented as an important finding for dense mesh deployments.

**RSSI colour coding:**
- Green: > −60 dBm (strong)
- Yellow: −60 to −75 dBm (moderate)
- Red: < −75 dBm (weak)

---
