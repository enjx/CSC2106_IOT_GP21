# LoRa Mesh Network Implementation

A mesh networking system using LoRa radios (RFM95) with the RadioHead library's RHMesh protocol for automatic routing and packet forwarding.

## Overview

This implementation demonstrates a LoRa mesh network capable of:
- **Text messaging** between nodes with acknowledgment
- **Image transfer** with automatic retries and checksum verification
- **Packet loss simulation** for testing reliability
- **Performance metrics** including RSSI, RTT, and throughput

## Architecture

- **Mesh Protocol**: RHMesh from RadioHead library handles automatic routing and packet forwarding
- **Frequency**: 923.0 MHz
- **Node Types**: Sender nodes and gateway (receiver) node
- **Payload Size**: 50 bytes per LoRa packet
- **Packet Structure**: Type (text/image), sequence number, RTT, checksum, and data payload

### Key Features

- **Automatic Routing**: RHMesh automatically finds routes to the gateway, even through intermediate nodes
- **Reliability**: Built-in acknowledgments with up to 6 retries for failed packets
- **Data Integrity**: Checksum validation for all packets
- **Performance Monitoring**: Real-time RSSI, round-trip time (RTT), and throughput metrics
- **Packet Loss Simulation**: Configurable packet drop percentage for testing resilience

## Hardware Requirements

- **LoRa Modules**: RFM95 LoRa radio modules (2 or more)
- **Microcontroller**: Arduino-compatible boards (e.g., Arduino Uno, Nano)
- **Wiring**: Connect RFM95 to Arduino:
  - CS → Pin 10
  - RST → Pin 9
  - INT → Pin 2
  - Additional standard SPI connections (MISO, MOSI, SCK)

## Software Requirements

- Arduino IDE with RadioHead library installed
- Python 3.x with `pyserial` library for image transfer scripts

## Configuration

Edit the following parameters in `mesh_node.ino` before uploading:

```c++
#define NODE_ID 1           // Unique ID for each node (1, 2, 3, etc.)
#define GATEWAY_ID 1        // ID of the gateway/receiver node
#define LORA_PAYLOAD 50     // Bytes per packet
#define MAX_RETRIES 6       // Retry attempts for failed packets
#define SIM_PACKET_LOSS_PERCENT 0  // 0-100% simulated packet loss
```

**Important**: 
- Each node must have a unique `NODE_ID`
- All nodes must have the same `GATEWAY_ID` configured
- The gateway node should have `NODE_ID` equal to `GATEWAY_ID`

## Usage & Test Cases

### Text Messaging
1. Configure the NODE_ID and GATEWAY_ID in mesh_node.ino and upload the copies with different NODE_ID to the LoRa mesh nodes.

2. Open the serial monitors of the node you want to send the message from and the gateway node.

3. Ensure serial monitor allows ending in newline and enter messages starting with the prefix "/msg ".

**Outcome:** Message should be received by gateway node with provided metrics. Sender node will be able to see the ACK from gateway.

### Image Transfer
1. Configure receiver.py and ensure gateway node port is correct (where your receiver/gateway node is connected to)

2. Configure sender.py and ensure sender node port is correct (E.g. "COM9").

3. Configure the NODE_ID and GATEWAY_ID in mesh_node.ino and upload the copies with different NODE_ID to the LoRa mesh nodes.

4. Ensure serial monitors for receiver and sender nodes are CLOSED before running the Python scripts.

5. Have an image ready named "image.jpg" (default) or whatever name is configured in the sender.py and reciever.py files.

6. Ensure your current working directory has that image file before running.

7. Run receiver.py, then sender.py to transfer the image.

**Outcome:** Received image should appear in your current directory as `received_image_verified.jpg`.

**Metrics Displayed:**
- RSSI (Received Signal Strength Indicator)
- RTT (Round-Trip Time in milliseconds)
- Throughput (Bytes per second)
- Verification status for each chunk

### Packet Drop Simulation
1. Configure the NODE_ID and GATEWAY_ID in mesh_node.ino.

2. Configure the simulated packet loss percentage in the gateway mesh node file (0-100%).
```c++
// Simulated packet loss (set to 0 to disable, 1-100 for drop percentage)
#define SIM_PACKET_LOSS_PERCENT 0
```

3. Upload the copies with different NODE_ID to the LoRa mesh nodes.

4. Try whichever method above you see fit.

**Outcome:**
- Messaging will sometimes show errors (depending on what % you input), with automatic retries up to MAX_RETRIES
- Image transfer will retry failed packets up to 6 times until successful image reassembly or abort if the retry limit is hit
- Console will display simulated packet drops and retry attempts

## Implementation Details

### Packet Structure

Each LoRa packet contains:
- **Type** (1 byte): 0 = image data, 1 = text message
- **Sequence Number** (2 bytes): Packet ordering for reassembly
- **RTT** (2 bytes): Round-trip time from previous transmission
- **Checksum** (1 byte): Data integrity verification
- **Data Payload** (50 bytes): Actual message or image chunk

### Sender Node Behavior

- Accepts serial input (text commands or binary image data)
- Text messages start with `/msg` prefix
- Image chunks are sent as 50-byte blocks
- Measures RTT by timing acknowledgments
- Automatically retries failed packets up to 6 times
- Reports transmission status via serial

### Gateway Node Behavior

- Continuously listens for incoming packets
- Validates checksums and discards corrupted data
- Tracks throughput and performance metrics
- Outputs verified data in hex format for Python receiver
- Displays RSSI, RTT, and throughput for monitoring

### Data Flow

1. **Text Message**: User → Serial → Sender Node → LoRa Mesh → Gateway → Serial Monitor
2. **Image Transfer**: Image File → sender.py → Serial → Sender Node → LoRa Mesh → Gateway → Serial → receiver.py → Received Image

## Performance Metrics

The system provides real-time monitoring of:
- **RSSI**: Signal strength between nodes (dBm)
- **RTT**: Round-trip acknowledgment time (milliseconds)
- **Throughput**: Data transfer rate (bytes per second)
- **Success Rate**: Packet delivery vs. retries

## Troubleshooting

- **Serial monitor interference**: Close all serial monitors before running Python scripts
- **Port configuration**: Verify correct COM port in Python scripts and serial monitor
- **Upload verification**: Ensure `NODE_ID` is unique for each physical device
- **Image corruption**: Check checksum errors in metrics output
- **No response**: Verify frequency (923.0 MHz) and that RHMesh is initialized successfully

## Files

- `mesh_node.ino` - Main Arduino firmware for both sender and gateway nodes
- `sender.py` - Python script to send image files via serial
- `receiver.py` - Python script to receive and reassemble images
- `image.jpg` - Sample image for testing (user-provided)

## License & Credits

Built using the RadioHead library's RHMesh protocol for automatic mesh routing.