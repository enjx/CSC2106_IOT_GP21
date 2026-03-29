#include <SPI.h>
#include <RH_RF95.h>
#include <RHMesh.h>

#define RFM95_CS 10
#define RFM95_RST 9
#define RFM95_INT 2
#define RF95_FREQ 923.0

#define NODE_ID 1
#define GATEWAY_ID 2

#define LORA_PAYLOAD 42 // Bytes per LoRa packet

RH_RF95 rf95(RFM95_CS, RFM95_INT);
RHMesh manager(rf95, NODE_ID);

struct __attribute__((__packed__)) ImagePacket {
    uint8_t  type;
    uint16_t seq;
    uint32_t timestamp;
    uint8_t  checksum;
    uint8_t  data[LORA_PAYLOAD];
};

// Globals
unsigned long startTime = 0;
uint32_t totalBytesReceived = 0;
bool firstPacket = true;
unsigned long lastDataTime = 0;
uint16_t imgSeq = 0;

uint8_t calculateChecksum(uint8_t* data, int len) {
    uint16_t sum = 0;
    for (int i = 0; i < len; i++) sum = (sum + data[i]) % 255;
    return (uint8_t)sum;
}

void setup() {
    Serial.begin(9600);
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
// Sends a LoRa packet. Returns true on success.
// ----------------------------------------------------------------
uint8_t sendPacket(ImagePacket& pkg) {
    pkg.timestamp = millis();
    pkg.checksum  = calculateChecksum(pkg.data, LORA_PAYLOAD);
    return manager.sendtoWait((uint8_t*)&pkg, sizeof(pkg), GATEWAY_ID);
}

void loop() {

    // ===== SENDER LOGIC (NODE 1) =====
    if (NODE_ID == 1) {

        if (Serial.available() > 0) {
            if (lastDataTime == 0) lastDataTime = millis();

            // --- Text message command ---
            if (Serial.peek() == '/') {
                String cmd = Serial.readStringUntil('\n');
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
                        Serial.println(F("MSG_SENT_OK"));
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

                uint8_t status = sendPacket(pkg);
                if (status == RH_ROUTER_ERROR_NONE) {
                    imgSeq++;
                    Serial.print(F("ACK:"));
                    Serial.println(pkg.seq);
                } else {
                    Serial.println(F("RETRY"));
                    Serial.println(status);
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
    if (NODE_ID == GATEWAY_ID) {
        uint8_t buf[sizeof(ImagePacket)];
        uint8_t len  = sizeof(buf);
        uint8_t from;

        if (manager.recvfromAck(buf, &len, &from)) {

            ImagePacket* p = (ImagePacket*)buf;

            if (calculateChecksum(p->data, LORA_PAYLOAD) == p->checksum) {

                if (p->type == 0 && firstPacket) {
                    imgSeq = 0;
                    startTime = millis();
                    firstPacket = false;
                    totalBytesReceived = 0;
                }
                if (p->type == 0) totalBytesReceived += LORA_PAYLOAD;

                unsigned long elapsed = millis() - startTime;
                float throughput = (!firstPacket && elapsed > 0)
                    ? (totalBytesReceived * 1000.0f / elapsed)
                    : 0.0f;

                // Prefix changed to METRIC: to match receiver.py
                int32_t latency = (int32_t)(millis() - p->timestamp);
                Serial.print(F("METRIC:RSSI="));  Serial.print(rf95.lastRssi());
                Serial.print(F("|LATENCY(ms)="));    Serial.print(latency);
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