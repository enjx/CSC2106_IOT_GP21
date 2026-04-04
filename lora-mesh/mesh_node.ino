#include <SPI.h>
#include <RH_RF95.h>
#include <RHMesh.h>

#define RFM95_CS 10
#define RFM95_RST 9
#define RFM95_INT 2
#define RF95_FREQ 923.0

#define NODE_ID 1
#define GATEWAY_ID 1

#define LORA_PAYLOAD 42 // Bytes per LoRa packet
#define MAX_RETRIES 6   // Retry attempts for failed packets

// Simulated packet loss (set to 0 to disable, 1-100 for drop percentage)
#define SIM_PACKET_LOSS_PERCENT 0

RH_RF95 rf95(RFM95_CS, RFM95_INT);
RHMesh manager(rf95, NODE_ID);

struct __attribute__((__packed__)) ImagePacket {
    uint8_t  type;      // 0=image, 1=text
    uint16_t seq;
    uint32_t timestamp;
    uint16_t rtt;       // Round-trip time measured by sender (in ms)
    uint8_t  checksum;
    uint8_t  data[LORA_PAYLOAD];
};

// Globals
unsigned long startTime = 0;
uint32_t totalBytesReceived = 0;
bool firstPacket = true;
unsigned long lastDataTime = 0;
uint16_t imgSeq = 0;
int32_t prevRTT = 0; // RTT from sender's ACK measurement
uint8_t targetNodeId = 0; // Store the node ID for routing
unsigned long lastDataReceived = 0; // Track when we last received data
uint16_t lastMeasuredRTT = 0; // Store previous RTT to send in next packet
uint16_t lastReceivedSeq = 0xFFFF; // Track last received sequence number

uint8_t calculateChecksum(uint8_t* data, int len) {
    uint16_t sum = 0;
    for (int i = 0; i < len; i++) sum = (sum + data[i]) % 255;
    return (uint8_t)sum;
}

void setup() {
    Serial.begin(9600);
    randomSeed(analogRead(A0)); // Seed RNG for packet loss simulation
    pinMode(RFM95_RST, OUTPUT);
    digitalWrite(RFM95_RST, LOW);  delay(10);
    digitalWrite(RFM95_RST, HIGH); delay(10);
    if (!manager.init()) {
        Serial.println(F("INIT FAILED"));
        while (1);
    }
    rf95.setFrequency(RF95_FREQ);
    rf95.setTxPower(23, false);
    Serial.print(F("READY, NODE_ID="));
    Serial.println(NODE_ID);
}

// ----------------------------------------------------------------
// Sends a LoRa packet and measures RTT. Returns RadioHead status code.
// Packet contains RTT from PREVIOUS transmission.
// ----------------------------------------------------------------
uint8_t sendPacket(ImagePacket& pkg) {
    pkg.timestamp = millis();
    pkg.rtt = lastMeasuredRTT; // Include RTT from previous packet
    pkg.checksum  = calculateChecksum(pkg.data, LORA_PAYLOAD);
    
    unsigned long sendStart = millis();
    uint8_t status = manager.sendtoWait((uint8_t*)&pkg, sizeof(pkg), GATEWAY_ID);
    
    if (status == RH_ROUTER_ERROR_NONE) {
        // Measure RTT and store for NEXT packet
        lastMeasuredRTT = (uint16_t)(millis() - sendStart);
    }
    
    return status;
}

void loop() {
    // ===== SENDER LOGIC (NODE 1) =====
    if (NODE_ID != GATEWAY_ID) {

        if (Serial.available() > 0) {
            if (lastDataTime == 0) lastDataTime = millis();

            // --- Command handling ---
            if (Serial.peek() == '/') {
                String cmd = Serial.readStringUntil('\n');
                
                // Text message command
                if (cmd.startsWith("/msg ")) {
                    String msg = cmd.substring(5);
                    if (msg.length() > LORA_PAYLOAD-1) {
                        Serial.println(F("WARN: Message truncated"));
                    }
                    ImagePacket pkg;
                    pkg.type = 1;
                    pkg.seq  = 999;
                    memset(pkg.data, 0, LORA_PAYLOAD);
                    msg.toCharArray((char*)pkg.data, LORA_PAYLOAD);

                    uint8_t status = sendPacket(pkg);
                    if (status == RH_ROUTER_ERROR_NONE) {
                        Serial.print(F("MSG_SENT_OK|RTT(ms)="));
                        Serial.println(lastMeasuredRTT);
                    } else {
                        Serial.print(F("MSG_FAIL, Error: "));
                        Serial.println(status);
                    }
                } else {
                    Serial.println(F("WARN: Unknown command"));
                }
                lastDataTime = 0;
            }

            // --- Image chunk: wait for full LORA_PAYLOAD bytes ---
            else if (Serial.available() >= LORA_PAYLOAD) {
                ImagePacket pkg;
                pkg.type = 0;
                pkg.seq  = imgSeq;
                Serial.readBytes((char*)pkg.data, LORA_PAYLOAD);

                bool success = false;
                uint8_t retryCount = 0;

                while (!success && retryCount <= MAX_RETRIES) {
                    // Simulate random packet loss
                    #if SIM_PACKET_LOSS_PERCENT > 0
                    if (random(100) < SIM_PACKET_LOSS_PERCENT) {
                        Serial.print(F("SIM: Packet "));
                        Serial.print(pkg.seq);
                        Serial.println(F(" dropped"));
                        retryCount++;
                        if (retryCount <= MAX_RETRIES) {
                            Serial.print(F("RETRY "));
                            Serial.print(retryCount);
                            Serial.print(F("/"));
                            Serial.println(MAX_RETRIES);
                        }
                        continue;
                    }
                    #endif

                    uint8_t status = sendPacket(pkg);
                    if (status == RH_ROUTER_ERROR_NONE) {
                        success = true;
                        imgSeq++;
                        Serial.print(F("ACK:"));
                        Serial.print(pkg.seq);
                        Serial.print(F("|RTT(ms)="));
                        Serial.println(lastMeasuredRTT);
                    } else {
                        retryCount++;
                        if (retryCount <= MAX_RETRIES) {
                            Serial.print(F("RETRY "));
                            Serial.print(retryCount);
                            Serial.print(F("/"));
                            Serial.println(MAX_RETRIES);
                        }
                    }
                }

                if (!success) {
                    Serial.print(F("FAIL:"));
                    Serial.println(pkg.seq);
                }

                lastDataTime = 0;
            }

            // --- Safety flush for stale/partial data ---
            else if (millis() - lastDataTime > 500) {
                Serial.println(F("SYSTEM: Invalid input. Flushing buffer..."));
                while (Serial.available() > 0) Serial.read();
                lastDataTime = 0;
            }

        } else {
            lastDataTime = 0;
        }
    }

    // ===== RECEIVER LOGIC (GATEWAY) =====
    else {
        uint8_t buf[sizeof(ImagePacket)];
        uint8_t len  = sizeof(buf);
        uint8_t from;

        if (manager.recvfromAck(buf, &len, &from)) {

            ImagePacket* p = (ImagePacket*)buf;

            if (calculateChecksum(p->data, LORA_PAYLOAD) == p->checksum) {

                // Detect new image transfer and handle throughput tracking
                if (p->type == 0) {
                    // Check if this is a new transfer (first packet OR seq reset/backwards)
                    bool isNewTransfer = firstPacket || 
                                        (p->seq == 0 && lastReceivedSeq != 0xFFFF) || 
                                        (p->seq < lastReceivedSeq);
                    
                    if (isNewTransfer) {
                        // Reset throughput tracking for new transfer
                        startTime = millis();
                        totalBytesReceived = 0;
                        firstPacket = false;
                        lastReceivedSeq = 0xFFFF; // Reset for next transfer detection
                        
                        if (!firstPacket && p->seq == 0) {
                            Serial.println(F("INFO: New image transfer detected"));
                        }
                    }
                    
                    lastReceivedSeq = p->seq;
                    totalBytesReceived += LORA_PAYLOAD;
                }

                // Store the node ID for RTT measurements
                if (targetNodeId == 0) {
                    targetNodeId = from;
                }

                // Track when we last received data
                lastDataReceived = millis();

                // Use RTT from the packet (measured by sender during ACK)
                if (p->rtt > 0) {
                    prevRTT = p->rtt;
                }

                unsigned long elapsed = millis() - startTime;
                float throughput = (!firstPacket && elapsed > 0)
                    ? (totalBytesReceived * 1000.0f / elapsed)
                    : 0.0f;

                // RTT measurement from data packet ACKs (measured by sender)
                Serial.print(F("METRIC:RSSI="));  Serial.print(rf95.lastRssi());
                Serial.print(F("|Previous RTT(ms)="));
                if (prevRTT > 0) {
                    Serial.print(prevRTT);
                } else {
                    Serial.print(F("N/A"));
                }
                Serial.print(F("|THROUGHPUT(Bps)=")); Serial.println(throughput);
                

                if (p->type == 1) {
                    Serial.print(F("NODE_")); Serial.print(from);
                    Serial.print(F(": "));              Serial.println((char*)p->data);
                } else {

                    // DATA:seq:hex — format receiver.py expects
                    Serial.print(F("DATA:"));
                    Serial.print(p->seq);
                    Serial.print(F(":"));
                    for (int i = 0; i < LORA_PAYLOAD; i++) {
                        if (p->data[i] < 0x10) Serial.print('0');
                        Serial.print(p->data[i], HEX);
                    }
                    Serial.println();
                }

            } else {
                // Notify receiver.py of corruption so it can log it
                Serial.print(F("CORRUPT:"));
                Serial.println(p->seq);
            }
        }
    }
}