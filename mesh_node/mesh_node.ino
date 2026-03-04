#include <SPI.h>
#include <RH_RF95.h>
#include <RHMesh.h>

// LoRa Pins (Maker Uno)
#define RFM95_CS 10
#define RFM95_RST 9
#define RFM95_INT 2
#define RF95_FREQ 923.0

// ===== MESH CONFIGURATION =====
#define NODE_ID 3      // CHANGE THIS FOR EACH BOARD (1, 2, 3, or 4)
#define GATEWAY_ID 4   // The gateway node
// ===============================

#define RH_MESH_MAX_MESSAGE_LEN 60

RH_RF95 rf95(RFM95_CS, RFM95_INT);
RHMesh manager(rf95, NODE_ID);

void setup() {
    Serial.begin(9600);

    // LoRa Manual Reset
    pinMode(RFM95_RST, OUTPUT);
    digitalWrite(RFM95_RST, LOW); delay(10);
    digitalWrite(RFM95_RST, HIGH); delay(10);

    if (!manager.init()) {
        Serial.println("Mesh init failed");
        while (1);
    }
    rf95.setFrequency(RF95_FREQ);
    rf95.setTxPower(23, false);
}

void loop() {
    // --- PART 1: SENDING DATA (Only if Node 1) ---
    if (NODE_ID == 1) {
        if (Serial.available() > 0) {
            char input[RH_MESH_MAX_MESSAGE_LEN];
            uint8_t len = Serial.readBytesUntil('\n', input, sizeof(input)-1);
            input[len] = '\0';

            Serial.print(F("Sending to Node "));
            Serial.print(GATEWAY_ID);
            Serial.print(F(": "));
            Serial.println(input);

            uint8_t error = manager.sendtoWait((uint8_t*)input, len + 1, GATEWAY_ID);
            if (error == RH_ROUTER_ERROR_NONE) {
                Serial.println(F(" -> [SUCCESS] ACK received."));
            } else {
                Serial.print(F(" -> [FAILED] Error Code: "));
                Serial.println(error); 
            }
        }
    }

    // --- PART 2: RECEIVING/FORWARDING (All Nodes) ---
    uint8_t buf[RH_MESH_MAX_MESSAGE_LEN];
    uint8_t len = sizeof(buf);
    uint8_t from;
    
    // recvfromAck handles both messages for US and messages we need to FORWARD
    if (manager.recvfromAck(buf, &len, &from)) {
        // Get signal strength of the last hop
        int lastRssi = rf95.lastRssi();

        // 1. Handle Serial Output for Laptop/RPi
        if (NODE_ID == GATEWAY_ID) {
            Serial.print("FROM:"); Serial.print(from);
            Serial.print("|MSG:"); Serial.print((char*)buf);
            Serial.print("|RSSI:"); Serial.println(lastRssi);
        } else {
            // Just a log for intermediate relay nodes
            Serial.print("Forwarding msg from Node "); Serial.println(from);
        }
    }
}