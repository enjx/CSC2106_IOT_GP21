#include <SPI.h>
#include <RH_RF95.h>
#include <RHMesh.h>

// LoRa Pins (Maker Uno)
#define RFM95_CS 10
#define RFM95_RST 9
#define RFM95_INT 2
#define RF95_FREQ 923.0

// ===== MESH CONFIGURATION =====
#define NODE_ID 1      // CHANGE THIS FOR EACH BOARD (1, 2, 3, or 4)
#define GATEWAY_ID 2   // The gateway node
// ===============================

#define RH_MESH_MAX_MESSAGE_LEN 50

RH_RF95 rf95(RFM95_CS, RFM95_INT);
RHMesh manager(rf95, NODE_ID);

struct __attribute__((__packed__)) ImagePacket {
    uint16_t seq;
    uint8_t checksum;
    uint8_t data[50];    
};

// Simple Fletcher-8 Checksum
uint8_t calculateChecksum(uint8_t* data, int len) {
    uint16_t sum = 0;
    for (int i = 0; i < len; i++) {
        sum = (sum + data[i]) % 255;
    }
    return (uint8_t)sum;
}

void setup() {
    Serial.begin(9600);
    pinMode(RFM95_RST, OUTPUT);
    digitalWrite(RFM95_RST, LOW); delay(10);
    digitalWrite(RFM95_RST, HIGH); delay(10);
    if (!manager.init()) while (1);
    rf95.setFrequency(RF95_FREQ);
    rf95.setTxPower(23, false);
}

void loop() {
    if (NODE_ID == 1) { // SENDER
        if (Serial.available() >= 50) {
            static uint16_t currentSeq = 0;
            ImagePacket pkg;
            pkg.seq = currentSeq;
            Serial.readBytes((char*)pkg.data, 50); 
            pkg.checksum = calculateChecksum(pkg.data, 50); // Calculate before sending

            if (manager.sendtoWait((uint8_t*)&pkg, sizeof(pkg), GATEWAY_ID) == RH_ROUTER_ERROR_NONE) {
                Serial.print(F("ACK:")); Serial.println(currentSeq++); 
            } else {
                Serial.print(F("RETRY:")); Serial.println(currentSeq);
            }
        }
    }

    uint8_t buf[sizeof(ImagePacket)];
    uint8_t len = sizeof(buf);
    uint8_t from;
    if (manager.recvfromAck(buf, &len, &from)) {
        if (NODE_ID == GATEWAY_ID) {
            ImagePacket* p = (ImagePacket*)buf;

            // Verify checksum on receipt
            uint8_t check = calculateChecksum(p->data, 50);
            if (check == p->checksum) {
                Serial.print(F("DATA:")); // Using "DATA" tag
                Serial.print(p->seq);
                Serial.print(F(":"));
                for(int i=0; i<50; i++) {
                    if(p->data[i] < 0x10) Serial.print('0'); // Leading zero fix
                    Serial.print(p->data[i], HEX);
                }
                Serial.println();
            }
        }
    }
}