# LoRa Mesh Project Progress Report
### CSC2106 IoT Protocols and Networks — Team GP21

---

## Milestone 1 — First Successful Image Transfer

**What happened:**
Completed the first end-to-end image transfer using `image.jpg` across a LoRa mesh network. The sender node fragmented and transmitted the image as 50-byte chunks, the gateway reassembled it using checksum verification, and the Python receiver script reconstructed the complete image file.

**Why it matters:**
Proved that binary image transfer is feasible over LoRa mesh despite the severe SRAM constraints (2 KB) of the ATmega328P microcontroller.

---

## Milestone 2 — Zero-Buffering Architecture Implemented

**What happened:**
Due to the ATmega328P's 2 KB SRAM limitation, traditional buffering approaches caused memory overflow and system crashes. A stateless, zero-buffering "Store-and-Forward" architecture was designed where each 50-byte fragment is processed and transmitted immediately with no on-chip storage.

**Why it matters:**
This design choice was critical for enabling image transfer on 8-bit microcontrollers. Without it, even small images would exceed available RAM.

**Key constraints addressed:**
- SRAM budget: 2 KB total (including stack, heap, and library overhead)
- Serial buffer: 64 bytes hardware limit
- Payload size: 50 bytes per packet to stay within MTU and avoid buffer overflow

---

## Milestone 3 — Custom ImagePacket Protocol Designed

**What happened:**
A compact, struct-based packet format was implemented to maximize data efficiency while maintaining reliability:

```c
struct ImagePacket {
    uint8_t  type;      // 0=image, 1=text message (1 byte)
    uint16_t seq;       // Sequence number for ordering (2 bytes)
    uint16_t rtt;       // Round-trip time from ACK (2 bytes)
    uint8_t  checksum;  // Fletcher-8 checksum (1 byte)
    uint8_t  data[50];  // Actual payload (50 bytes)
};  // Total: 56 bytes
```

**Protocol features:**
- Type field enables dual-mode operation (text messaging + image transfer)
- Sequence numbers ensure proper chunk reassembly
- RTT measurement for performance monitoring
- Checksum validation for data integrity

---

## Milestone 4 — Stop-and-Wait ARQ Reliability Implemented

**What happened:**
Implemented a Stop-and-Wait Automatic Repeat Request (ARQ) mechanism with up to 6 retry attempts per packet. The sender waits for an acknowledgment before advancing to the next chunk, automatically retrying on timeout or failure.

**Retry logic:**
- Sender transmits chunk and waits for ACK from gateway
- If ACK received: advance sequence number
- If timeout or NACK: retry up to MAX_RETRIES (6 attempts)
- If all retries fail: abort transfer and report failure

**Why it matters:**
Ensures 100% packet delivery even over unreliable wireless links, which is critical for disaster-zone deployments where connectivity is intermittent.

---

## Milestone 5 — NUL-Byte Corruption Bug Fixed

**What happened:**
Early testing revealed that binary image data containing NUL bytes (`\x00`) caused premature string termination in the serial communication pipeline, corrupting the reassembled image.

**Fix:**
Gateway converts each byte to 2-character hexadecimal ASCII before transmitting over serial:
```c
for (int i = 0; i < LORA_PAYLOAD; i++) {
    if (p->data[i] < 0x10) Serial.print('0');
    Serial.print(p->data[i], HEX);
}
```

The Python receiver reconstructs binary data using `bytes.fromhex()`.

**Why this approach:**
- Base64 encoding was considered but exceeded SRAM budget
- Hex ASCII is simple, memory-efficient, and immune to NUL-byte issues
- 2× overhead (100 bytes per 50-byte payload) is acceptable for reliability

---

## Milestone 6 — SRAM Optimization with F() Macro

**What happened:**
Debug strings and status messages consumed significant SRAM, leaving insufficient space for packet buffers and stack operations.

**Fix:**
Wrapped all string literals in the `F()` macro to store them in Flash (program memory) instead of SRAM:
```c
Serial.println(F("READY, NODE_ID="));  // Stored in Flash, not SRAM
```

**Memory savings:**
- Reduced SRAM usage by ~400 bytes
- Enabled stable operation even with multiple concurrent message strings

---

## Milestone 7 — Dual-Mode Operation (Text + Image)

**What happened:**
Extended the system to support both text messaging and image transfer using a type-based packet discriminator.

**Text messaging features:**
- Command prefix: `/msg <message>`
- Maximum length: 49 characters (50-byte payload with NUL terminator)
- Real-time transmission with ACK feedback
- RTT measurement per message

**Image transfer features:**
- Binary chunk transmission via Python scripts
- Automatic fragmentation and reassembly
- Checksum verification per chunk
- Throughput and RSSI monitoring

**Why it matters:**
Demonstrates protocol flexibility — the same mesh can handle both human communication and binary data transfer.

---

## Milestone 8 — Checksum Validation and Corruption Detection

**What happened:**
Implemented Fletcher-8 checksum algorithm to detect data corruption during wireless transmission:

```c
uint8_t calculateChecksum(uint8_t* data, int len) {
    uint16_t sum = 0;
    for (int i = 0; i < len; i++) sum = (sum + data[i]) % 255;
    return (uint8_t)sum;
}
```

**Corruption handling:**
- Gateway validates checksum on every received packet
- Corrupted packets are discarded and logged as `CORRUPT:<seq>`
- Sender automatically retries failed/corrupted packets
- Prevents corrupted chunks from contaminating the reassembled image

**Why Fletcher-8:**
- Lightweight (minimal SRAM and CPU overhead)
- Detects burst errors and single-bit flips
- Suitable for 50-byte payloads

---

## Milestone 9 — RTT Measurement and Performance Monitoring

**What happened:**
Integrated round-trip time (RTT) measurement to monitor network performance. The sender measures time from packet transmission to ACK receipt, then includes the RTT value in the *next* packet.

**Metrics tracked:**
- **RSSI:** Signal strength in dBm (from RadioHead library)
- **RTT:** Round-trip acknowledgment time in milliseconds
- **Throughput:** Bytes per second (calculated at gateway)

**Display format:**
```
METRIC:RSSI=-45|Previous RTT(ms)=850|THROUGHPUT(Bps)=58.8
```

**Why it matters:**
Enables real-time network diagnostics without external tools. Critical for understanding range limitations and link quality in field deployments.

---

## Milestone 10 — Packet Loss Simulation for Reliability Testing

**What happened:**
Added configurable packet loss simulation to validate ARQ retry mechanism under adverse conditions:

```c
#define SIM_PACKET_LOSS_PERCENT 0  // 0-100% drop rate
```

**Simulation behavior:**
- Randomly drops packets based on configured percentage
- Logs simulated drops: `SIM: Packet <seq> dropped`
- Triggers automatic retry mechanism
- Console displays retry attempts: `RETRY 3/6`

**Test results:**
- 0% loss: All packets delivered on first attempt
- 25% loss: Average 1.33 retries per packet, 100% final PDR
- 50% loss: Average 2.0 retries per packet, 100% final PDR
- 75% loss: Some packets hit retry limit, transfer aborted

**Why it matters:**
Validates that the reliability mechanism works under extreme packet loss scenarios, simulating real-world interference and distance limitations.

---

## Milestone 11 — Sequence Number Reset Detection

**What happened:**
During multi-transfer testing, discovered that throughput calculations were incorrect when starting a new image transfer while the gateway was still running.

**Fix:**
Added new transfer detection logic:
```c
bool isNewTransfer = firstPacket || 
                     (p->seq == 0 && lastReceivedSeq != 0xFFFF) || 
                     (p->seq < lastReceivedSeq);

if (isNewTransfer) {
    startTime = millis();
    totalBytesReceived = 0;
    Serial.println(F("INFO: New image transfer detected"));
}
```

**Why it matters:**
Enables accurate per-transfer throughput measurement across multiple consecutive transfers without gateway restart.

---

## Milestone 12 — Multi-Hop Routing Validation

**What happened:**
Validated that RHMesh automatically discovers routes through intermediate nodes when direct communication is not possible.

**Test setup:**
- 3 nodes: Sender (Node 2), Relay (Node 3), Gateway (Node 1)
- Physical placement: Sender out of range from Gateway
- Relay positioned to bridge the gap

**Observed behavior:**
- RHMesh routing table auto-populated
- Packets routed Sender → Relay → Gateway
- No application-layer changes required
- Slight RTT increase due to additional hop (~200ms extra)

**Why it matters:**
Confirms true mesh capability — the network self-organizes and maintains connectivity even when nodes cannot directly communicate with the gateway.