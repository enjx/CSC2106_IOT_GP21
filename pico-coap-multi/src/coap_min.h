#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "lwip/udp.h"
#include "lwip/ip_addr.h"

#define COAP_VER 1

typedef enum {
    COAP_TYPE_CON = 0, COAP_TYPE_NON = 1, COAP_TYPE_ACK = 2, COAP_TYPE_RST = 3
} coap_type_t;

typedef enum {
    COAP_EMPTY=0x00, 
    COAP_GET=0x01, 
    COAP_POST=0x02, 
    COAP_PUT=0x03, 
    COAP_DELETE=0x04,
    COAP_FETCH= 0x05,
    COAP_PATCH = 0x06,
    COAP_CREATED=0x41, 
    COAP_DELETED=0x42, 
    COAP_VALID=0x43, 
    COAP_CHANGED=0x44, 
    COAP_CONTENT=0x45,
    COAP_CONTINUE=0x5F,
    COAP_BAD_REQUEST=0x80, 
    COAP_NOT_FOUND=0x84, 
    COAP_METHOD_NOT_ALLOWED=0x85,
    COAP_REQUEST_ENTITY_INCOMPLETE=0x88, 
    COAP_REQUEST_ENTITY_TOO_LARGE=0x8D,
    COAP_GATEWAY_TIMEOUT=0xA4, 
    COAP_INTERNAL_SERVER_ERROR = 0xA0, 
    COAP_CONFLICT = 0x89
} coap_code_t;

#define COAP_OPT_OBSERVE 6
#define COAP_OPT_URI_PATH 11
#define COAP_OPT_CONTENT_FORMAT 12
#define COAP_OPT_URI_QUERY 15
#define COAP_OPT_BLOCK1 27
#define COAP_OPT_SIZE1 28

typedef struct {
    coap_type_t type;
    uint8_t code;
    uint16_t msg_id;
    uint8_t token[8];
    uint8_t tkl;
    char uri[64];
    char uri_query[64];
    bool observe_register;
    const uint8_t* payload;
    uint16_t payload_len;
    
    // Block1 fields
    bool has_block1;
    uint32_t block1_num;
    bool block1_more;
    uint8_t block1_szx;
    uint16_t size1;

    int32_t observe_seq;   // -1 if absent; otherwise 0..(2^24-1)
    bool has_observe;   // true if option 6 present (register or seq)

} coap_pkt_t;

typedef struct {
    struct udp_pcb* pcb;
    uint16_t next_msg_id;
} coap_ctx_t;

void coap_init(coap_ctx_t* ctx);
uint16_t coap_next_msg_id(coap_ctx_t* ctx);
bool coap_parse(const uint8_t* buf, uint16_t len, coap_pkt_t* out);

uint16_t coap_build_response(uint8_t* out, uint16_t out_max,
    coap_type_t type, uint8_t code, uint16_t msg_id,
    const uint8_t* token, uint8_t tkl,
    int32_t observe_seq,
    const char* uri_ignored,
    const uint8_t* payload, uint16_t payload_len);

uint16_t coap_build_request(uint8_t* out, uint16_t out_max,
    uint8_t method, coap_type_t type,
    uint16_t msg_id, const uint8_t* token, uint8_t tkl,
    const char* uri_path, const uint8_t* payload, uint16_t payload_len);

uint16_t coap_build_block1_response(uint8_t* out, uint16_t out_max,
    coap_type_t type, uint8_t code, uint16_t msg_id,
    const uint8_t* token, uint8_t tkl,
    uint32_t block_num, bool more, uint8_t szx,
    const uint8_t* payload, uint16_t payload_len);

uint16_t coap_build_block1_request(uint8_t* out, uint16_t out_max,
    uint8_t method, coap_type_t type,
    uint16_t msg_id, const uint8_t* token, uint8_t tkl,
    const char* uri_path, const char* uri_query,
    uint32_t block_num, bool more, uint8_t szx,
    const uint8_t* payload, uint16_t payload_len);

uint16_t coap_build_observe_get(
    uint8_t* out, uint16_t out_max,
    coap_type_t type, uint16_t msg_id,
    const uint8_t* token, uint8_t tkl,
    const char* uri_path);
