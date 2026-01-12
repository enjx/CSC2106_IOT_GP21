# CSC2106_IOT_GP21
CSC2106 IOT Group 21

# INF2004 Project by CS08 (CoAP)

## Project Description
The goal of the project is to demonstrate that the CoAP (Constrained Application Protocol) stack works on a Pi Pico W and how CoAP facilitates communication between a server and client to transfer both small messages and large files reliably, and demonstrate event handling in Pico W using CoAP protocols.

## Purpose of Files & Folders

### no-OS-FatFS-SD-SPI-RPi-Pico
A simple library for SD cards on the Pico from https://github.com/carlk3/no-OS-FatFS-SD-SPI-RPi-Pico.
It provides FatFS filesystem support and SD card driver for microSD storage on Pico W. The CoAP server and client use it to:

* Read files from SD (server sends files via CoAP).
* Write files to SD (client receives and stores files).
* Verify file integrity (CRC32 checks after transfer).

### client_main.c
CoAP client application with these features:
* Request file from server
* Upload file to server
* Register with server (POST /register)
* Button triggers (file request, upload, delete)
* Receive temperature notifications (Observe)
* Blink LED during transfers
* Mounts SD, connects Wi-Fi, binds UDP.

### server_main.c
CoAP server application that handles:
* File upload
* File download/push
* FETCH (partial read)
* PATCH (in-place edit)
* LED control
* GET Temperature + Observe
* Resource discovery
* File deletion
* Mounts SD, initializes Wi-Fi, binds UDP, dispatches CoAP requests.

### coap_min.c
CoAP protocol core with these functions:
* coap_parse: decode CoAP PDU (header/options/payload)
* coap_build_request / coap_build_response: encode PDUs
* coap_build_block1_request / coap_build_block1_response: Block1 helpers
* coap_build_observe_get: Observe subscription
* write_option: option encoding
* Implements RFC 7252 (CoAP), RFC 7959 (Block), RFC 7641 (Observe) subset.

### coap_min.h
CoAP protocol definitions:
* Types: coap_type_t (CON/NON/ACK/RST)
* Codes: coap_code_t (methods: GET/POST/PUT/DELETE/FETCH/PATCH; responses: 2.xx/4.xx/5.xx)
* Options: Block1, Observe, Uri-Path, Uri-Query, Content-Format
* Structures: coap_pkt_t (parsed packet), coap_ctx_t (MsgID counter)

### config.h
Build-time config:
* WIFI_SSID, WIFI_PASS: Wi-Fi credentials
* SERVER_IP: CoAP server address
* CLIENT_ID_STR: client identifier (set by CMake -DCLIENT_ID_STR)

### hw_config.c
SD card hardware config:
* Defines SPI pins (MISO/MOSI/SCK/CS), card detect pin
* Called by FatFS SD driver (from no-OS-FatFS-SD-SPI-RPi-Pico) during sd_init_driver()

### lwipopts.h
lwIP stack config:
* Tuned for CoAP over UDP (MEM_SIZE, PBUF_POOL_SIZE, etc.)
* Enables DHCP, UDP, raw sockets
* Overrides example defaults if needed

## Testing Instructions
### Prerequisites
* Raspberry Pi Pico W boards (1 server + 1 or more clients)
* Formatted microSD card (FAT32)
* USB cables
* Wi-Fi network or mobile hotspot
* Pico SDK v1.5.1+ installed
* CMake and build tools configured
* libcoap (for testing laptop as CoAP client)

### Configuration of CoAP Server & Client Pico W
1. Move folders **pico-coap-multi** and **no-OS-FatFS-SD-SPI-RPi-Pico** to **\Pico-v1.5.1\pico-examples\pico_w\wifi**
2. Add subdirectory in CMake file under **pico_w\wifi** folder: **add_subdirectory(pico-coap-multi)**
3. Change values of **WIFI_SSID** & **WIFI_PASS** to use WiFi network/hotspot you plan to connect the CoAP Server and Clients to in **config.h**
4. CMake build **pico_coap_server** and transfer the built .uf2 file (can be found in **\Pico-v1.5.1\pico-examples\build\pico_w\wifi\pico-coap-multi**) to a designated CoAP Server Pico W.
5. Connect to the CoAP Server Pico W via USB and check its serial monitoring output. Take note of its generated IP.
6. Change **SERVER_IP** in **config.h** to the IP of CoAP Server.
7. CMake build **pico_coap_client_A0** and transfer to a designated CoAP Client Pico W.
8. Check serial monitoring output of CoAP Server and Client Pico Ws to verify connection.

**Note:** Reset connection to Pico W each time after removing/inserting the SD card for detection.

### Verification Steps
Check Serial Outputs:
* Server: Should show "SD mounted", "WiFi connected", "Server IP: X.X.X.X"
* Client: Should show "SD mounted", "WiFi connected", "Registered with server"

### Testing with Laptop CoAP Client
Install libcoap on WSL to test from Laptop CoAP Client:
**sudo apt install libcoap3-bin  # Debian/Ubuntu**

### LED Control via Laptop Client
Turn on server LED:
**coap-client-gnutls -m put coap://SERVER_IP/led -e "on"**

Turn off server LED: 
**coap-client-gnutls -m put coap://SERVER_IP/led -e "off"**

Check server LED status: 
**coap-client-gnutls -m get coap://SERVER_IP/led**

### Observe Temperature Function via Laptop Client:
Subscribe:
**coap-client-gnutls -m get -s 60 coap://SERVER_IP/temp -O 6,0x00**

Unsubscribe:
**coap-client-gnutls -m get coap://SERVER_IP/temp -O 6,0x01**

### FETCH partial file
FETCH example (fetching data from hello.txt): **coap-client-gnutls -m fetch "coap://SERVER_IP/files?name=hello.txt&offset=0&len=30"**

* "offset=0" : start reading from byte position 0.
* "len=30" : read 30 bytes of data.

### PATCH (overwrite)
PATCH example (contents of testing.txt written to hello.txt): **coap-client-gnutls -m patch -f testing.txt "coap://SERVER_IP/files?name=hello.txt&offset=32"**

* "offset=32" : start overwriting at byte position 32.
