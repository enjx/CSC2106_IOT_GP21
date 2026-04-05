# CSC2106 IoT Mesh Network Implementations

This repository contains two mesh networking implementations developed for IoT applications, demonstrating different wireless technologies and use cases.

## Project Overview

This project explores mesh networking for IoT devices through two distinct implementations:

1. **ESP-MESH** - High-throughput WiFi-based mesh using ESP32 microcontrollers
2. **LoRa Mesh** - Long-range, low-power mesh using LoRa radio modules

Both implementations support:
- **Image transfer** across multi-hop mesh networks
- **Automatic routing** and packet forwarding
- **Reliability mechanisms** with automatic retries
- **Performance monitoring** (RSSI, throughput, latency)
- **Data integrity** verification (CRC32/checksum)

---

## ESP-MESH Implementation

**Location:** [`ESP-MESH/`](./ESP-MESH/)

### Technology Stack
- **Hardware:** M5StickC Plus (ESP32, ST7789 LCD)
- **Protocol:** ESP-IDF ESP-MESH (WiFi 2.4 GHz)
- **Payload:** 800 bytes per packet
- **IDE:** VSCode with ESP-IDF extension

### Key Features
- High throughput (~442 B/s average)
- elective ARQ retry (only missing chunks retransmitted)
- LCD display showing mesh status and RSSI
- Benchmark testing with CSV logging
- Exclusive transfer lock (distributed mutex)
- CRC32 verification per chunk + full file

### Performance Highlights
Based on Layer 3 node testing (March 2026):
- **Average throughput:** 442.1 bytes/second
- **Average transfer time:** 68.45 seconds
- **Packet loss:** 0.0% (with automatic recovery)
- **Success rate:** 5/5 images fully recovered

### Use Cases
- Indoor IoT deployments where WiFi infrastructure exists
- Applications requiring higher throughput
- Environments with power availability
- Multi-hop scenarios (up to Layer 4 tested)


**[Full ESP-MESH Documentation →](./ESP-MESH/README.md)**

---

## LoRa Mesh Implementation

**Location:** [`lora-mesh/`](./lora-mesh/)

### Technology Stack
- **Hardware:** Arduino + RFM95 LoRa modules
- **Protocol:** RadioHead RHMesh (923.0 MHz)
- **Payload:** 50 bytes per packet
- **IDE:** Arduino IDE

### Key Features
- Long-range communication (sub-GHz LoRa)
- Low power consumption
- Text messaging + image transfer
- Packet loss simulation (0-100% configurable)
- Checksum validation
- RTT measurement per packet

### Capabilities
- **Text messaging** with `/msg` prefix commands
- **Image transfer** via Python serial scripts
- **Reliability testing** with simulated packet loss
- **Multi-hop routing** through intermediate nodes

### Use Cases
- Long-range outdoor deployments
- Battery-powered sensor networks
- Rural/remote IoT applications
- Low-bandwidth monitoring systems
- Testing mesh reliability under packet loss

---

## Comparison: ESP-MESH vs LoRa Mesh

| Feature | ESP-MESH | LoRa Mesh |
|---------|----------|-----------|
| **Frequency** | 2.4 GHz (WiFi) | 923 MHz (LoRa) |
| **Range** | ~100m indoor | Several km outdoor |
| **Throughput** | ~442 B/s (tested) | Lower (50B packets) |
| **Power Consumption** | Higher | Very low |
| **Payload Size** | 800 bytes | 50 bytes |
| **Hardware** | ESP32 | Arduino + RFM95 |
| **Best For** | Indoor, high-speed | Outdoor, long-range |
| **Retry Mechanism** | Selective ARQ | Full packet retry |
| **Display** | Built-in LCD | None |
| **Benchmarking** | CSV logging | Manual testing |

---

## Repository Structure

```
CSC2106_IOT_GP21/
├── ESP-MESH/
│   ├── internal_communication/
│   │   ├── main/
│   │   │   ├── mesh_main.c          # ESP32 firmware
│   │   │   ├── Kconfig.projbuild    # Configuration options
│   │   │   └── CMakeLists.txt       # Build configuration
│   │   ├── send_image.py            # Sender Python script
│   │   ├── receive_image.py         # Receiver Python script
│   │   ├── benchmark.py             # Benchmarking tool
│   │   ├── testImage.jpg            # Sample test image
│   │   ├── photo1-5.jpg             # Benchmark images
│   │   └── CMakeLists.txt           # Project build file
│   ├── Progress.md                  # Development progress
│   └── README.md                    # ESP-MESH documentation
│
├── lora-mesh/
│   ├── mesh_node.ino                # Arduino firmware
│   ├── sender.py                    # Image sender script
│   ├── receiver.py                  # Image receiver script
│   ├── image.jpg                    # Sample image
│   └── README.md                    # LoRa Mesh documentation
│
└── README.md                        # This file
```

---

## Getting Started

### Prerequisites
- **Python 3.x** with `pyserial` library: `pip install pyserial`
- **For ESP-MESH:** VSCode with ESP-IDF extension (v5.5.3)
- **For LoRa Mesh:** Arduino IDE with RadioHead library

### General Workflow
1. Choose your implementation based on use case requirements
2. Navigate to the respective folder
3. Follow the detailed setup instructions in each README

---

## Project Goals

This project demonstrates:
- Mesh networking fundamentals with different technologies
- Reliability mechanisms (retries, acknowledgments, checksums)
- Performance monitoring and benchmarking
- Multi-hop routing capabilities
- Trade-offs between range, throughput, and power consumption

---

## Testing & Validation

Both implementations have been tested with:
- Single-hop and multi-hop configurations
- Image transfer integrity verification
- Performance metric collection
- Reliability under packet loss (simulated/real)
- RSSI monitoring and signal strength analysis

---

## Documentation

Detailed implementation documentation, test cases, and troubleshooting guides are available in each folder:

- **[ESP-MESH Documentation](./ESP-MESH/README.md)** - WiFi mesh implementation
- **[LoRa Mesh Documentation](./lora-mesh/README.md)** - LoRa mesh implementation
