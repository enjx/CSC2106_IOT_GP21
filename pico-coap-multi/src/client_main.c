#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/cyw43_arch.h"
#include "hardware/gpio.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "config.h"
#include "coap_min.h"
#include "f_util.h"
#include "ff.h"
#include "hw_config.h"

#ifndef CLIENT_ID_STR
#error "CLIENT_ID_STR must be defined by CMake"
#endif

#define MAX_FILE_SIZE 8192

/* ----------- Buttons ----------- */
#define BTN_REQ  21  // ask server to push hello.txt
#define BTN_LOG  20  // append line to hello.txt
#define BTN_SEND 22  // copy + POST hello_updated.txt to server

static bool btn_last_req  = true; // pull-up idle HIGH
static bool btn_last_log  = true;
static bool btn_last_send = true;
static absolute_time_t deb_req_until, deb_log_until, deb_send_until;

/* ----------- RX (incoming file from server/laptop) ----------- */
typedef struct {
    uint8_t  data[MAX_FILE_SIZE];
    uint16_t bytes_received;
    uint32_t last_block_num;
    bool     in_progress;
    char     filename[32];
    uint32_t expected_crc32;
    uint32_t expected_size;
    bool     have_expectations;
} file_rx_ctx_t;

/* ----------- TX (outgoing file to server) + retransmit ----------- */
typedef struct {
    uint8_t  data[MAX_FILE_SIZE];
    uint16_t total_bytes;
    uint16_t bytes_sent;
    uint32_t current_block;
    uint8_t  block_szx;          // 4 => 256B
    bool     in_progress;
    uint16_t msg_id;             // next MID to allocate
    uint16_t last_mid;           // MID in-flight
    uint8_t  token[2];
    uint8_t  tkl;
    char     dest_filename[32];
    // retransmit
    absolute_time_t ack_deadline;
    uint8_t  retries;            // 0..4
} file_tx_ctx_t;

static coap_ctx_t        coap;
static struct udp_pcb*   pcb;
static file_rx_ctx_t     file_ctx = {0};
static file_tx_ctx_t     tx_ctx   = {0};
static FATFS             fs;
static bool              sd_mounted = false;


static void send_observe_temp_request_to_server(void) {
    ip_addr_t dst; if (!ip4addr_aton(SERVER_IP, ip_2_ip4(&dst))) return;
    uint8_t tok[2] = {0xEE, 0x01};
    uint16_t mid = coap_next_msg_id(&coap);
    uint8_t buf[160];

    uint16_t mlen = coap_build_observe_get(buf, sizeof(buf),
        COAP_TYPE_CON, mid, tok, sizeof(tok), "temp");
    if (!mlen) { printf("Build observe GET failed\n"); return; }

    struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
    if (!p) return;
    memcpy(p->payload, buf, mlen);
    udp_sendto(pcb, p, &dst, COAP_DEFAULT_PORT);
    pbuf_free(p);

    printf("Observe=0 GET /temp -> server %s (MID=%u)\n",
           SERVER_IP, mid);
}

/* ---------------- CRC32 (IEEE) ---------------- */
static uint32_t crc32_calc(const uint8_t* data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (int k=0; k<8; k++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

/* ---------------- URI query helpers ---------------- */
static bool query_get_u32_hex(const char* q, const char* key, uint32_t* out) {
    if (!q) return false;
    char pat[16];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char* p = strstr(q, pat);
    if (!p) return false;
    p += strlen(pat);
    uint32_t v = 0; int n = 0;
    while (*p && *p!='&' && n < 8) {
        char ch = *p++;
        uint8_t d = (ch>='0'&&ch<='9')? ch-'0' :
                    (ch>='a'&&ch<='f')? ch-'a'+10 :
                    (ch>='A'&&ch<='F')? ch-'A'+10 : 0xFF;
        if (d==0xFF) break;
        v = (v<<4) | d; n++;
    }
    *out = v; return n>0;
}
static bool query_get_u32_dec(const char* q, const char* key, uint32_t* out) {
    if (!q) return false;
    char pat[16];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char* p = strstr(q, pat);
    if (!p) return false;
    p += strlen(pat);
    uint32_t v = 0; int n = 0;
    while (*p && *p!='&') {
        if (*p<'0'||*p>'9') break;
        v = v*10 + (uint32_t)(*p - '0'); p++; n++;
    }
    *out = v; return n>0;
}

/* ---------------- SD helpers ---------------- */
static bool save_to_sd(const char* filename, const uint8_t* data, uint32_t size) {
    if (!sd_mounted) { printf("SD not mounted\n"); return false; }
    FIL file; FRESULT fr; UINT bw=0;
    fr = f_open(&file, filename, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) { printf("SD: open(w) %s err=%d\n", filename, fr); return false; }
    fr = f_write(&file, data, size, &bw);
    f_close(&file);
    if (fr==FR_OK && bw==size) { printf("Saved: %s (%lu bytes)\n", filename, bw); return true; }
    printf("SD: write err=%d (wrote %lu/%lu)\n", fr, (unsigned long)bw, (unsigned long)size);
    return false;
}
static bool append_line_sd(const char* filename, const char* line) {
    if (!sd_mounted) { printf("SD not mounted\n"); return false; }
    FIL file; FRESULT fr; UINT bw=0;
    fr = f_open(&file, filename, FA_OPEN_APPEND | FA_WRITE);
    if (fr == FR_NO_FILE) fr = f_open(&file, filename, FA_CREATE_NEW | FA_WRITE);
    if (fr != FR_OK) { printf("SD: open(append) %s err=%d\n", filename, fr); return false; }
    fr = f_write(&file, line, (UINT)strlen(line), &bw);
    if (fr == FR_OK) { const char nl = '\n'; UINT bw2=0; f_write(&file, &nl, 1, &bw2); }
    f_close(&file);
    if (fr == FR_OK) { printf("Appended to %s: %s\n", filename, line); return true; }
    printf("SD: append err=%d\n", fr); return false;
}
static bool copy_file_sd(const char* src, const char* dst, uint8_t* buf, uint32_t* out_len, uint32_t max_len) {
    if (!sd_mounted) { printf("SD not mounted\n"); return false; }
    FIL fsrc, fdst; FRESULT fr; UINT br=0, bw=0;
    fr = f_open(&fsrc, src, FA_READ);
    if (fr != FR_OK) { printf("SD: open(r) %s err=%d\n", src, fr); return false; }
    DWORD sz = f_size(&fsrc);
    if (sz == 0 || sz > max_len) { printf("SD: size %s = %lu (bad)\n", src, (unsigned long)sz); f_close(&fsrc); return false; }
    fr = f_read(&fsrc, buf, sz, &br);
    f_close(&fsrc);
    if (fr != FR_OK || br != sz) { printf("SD: read err=%d (%lu/%lu)\n", fr, (unsigned long)br, (unsigned long)sz); return false; }
    *out_len = (uint32_t)sz;

    fr = f_open(&fdst, dst, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) { printf("SD: open(w) %s err=%d\n", dst, fr); return false; }
    fr = f_write(&fdst, buf, *out_len, &bw);
    f_close(&fdst);
    if (fr != FR_OK || bw != *out_len) { printf("SD: copy write err=%d\n", fr); return false; }
    printf("Copied %s -> %s (%lu bytes)\n", src, dst, (unsigned long)*out_len);
    return true;
}

/* ---------------- Network helpers ---------------- */
static void send_register() {
    ip_addr_t dst; if (!ip4addr_aton(SERVER_IP, ip_2_ip4(&dst))) { printf("Bad SERVER_IP\n"); return; }
    uint8_t tok[2] = {0xAB,0xCD};
    uint16_t mid = coap_next_msg_id(&coap);
    char payload[16]; snprintf(payload, sizeof(payload), "id=%s", CLIENT_ID_STR);
    uint8_t buf[128];
    uint16_t mlen = coap_build_request(buf, sizeof(buf),
        COAP_POST, COAP_TYPE_NON, mid, tok, sizeof(tok),
        "register", (const uint8_t*)payload, strlen(payload));
    if (!mlen) { printf("Build register failed\n"); return; }
    struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
    memcpy(p->payload, buf, mlen); udp_sendto(pcb, p, &dst, COAP_DEFAULT_PORT); pbuf_free(p);
    printf("Registered id=%s\n", CLIENT_ID_STR);
}
// --ACK server notifications on the client--
static void send_empty_ack(uint16_t msg_id, const ip_addr_t* addr, u16_t port) {
    uint8_t out[32];
    uint16_t mlen = coap_build_response(out, sizeof(out),
        COAP_TYPE_ACK, COAP_EMPTY, msg_id,
        NULL, 0,
        -1, NULL, NULL, 0);
    if (!mlen) return;
    struct pbuf* rp = pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
    if (!rp) return;
    memcpy(rp->payload, out, mlen);
    udp_sendto(pcb, rp, addr, port);
    pbuf_free(rp);
}

/* ---- Request resource discovery from server (GP21 long press) ---- */
static void request_discovery_from_server(void) {
    ip_addr_t dst; 
    if (!ip4addr_aton(SERVER_IP, ip_2_ip4(&dst))) return;
    
    uint8_t tok[2] = {0xD1, 0x5C}; // unique discovery token
    uint16_t mid = coap_next_msg_id(&coap);
    uint8_t buf[128];
    
    uint16_t mlen = coap_build_request(buf, sizeof(buf),
                                       COAP_GET, COAP_TYPE_CON, mid,
                                       tok, sizeof(tok),
                                       "discover", NULL, 0);
    
    if (!mlen) { printf("ERROR: build GET /discover\n"); return; }
    
    struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
    memcpy(p->payload, buf, mlen);
    udp_sendto(pcb, p, &dst, COAP_DEFAULT_PORT);
    pbuf_free(p);
    
    printf("Sent GET /discover -> server %s (MID=%u)\n", SERVER_IP, mid);
}



/* ---- Ask server to push a file to us (GP21) ---- */
static void request_file_from_server(const char* filename) {
    ip_addr_t dst; if (!ip4addr_aton(SERVER_IP, ip_2_ip4(&dst))) return;
    char payload[64]; snprintf(payload, sizeof(payload), "target=%s,filename=%s", CLIENT_ID_STR, filename);
    uint8_t tok[2] = {0xB1, 0x21};
    uint16_t mid = coap_next_msg_id(&coap);
    uint8_t buf[256];
    uint16_t mlen = coap_build_request(buf, sizeof(buf),
        COAP_GET, COAP_TYPE_CON, mid, tok, sizeof(tok),
        "send", (const uint8_t*)payload, strlen(payload));
    if (!mlen) { printf("ERROR: build GET /send\n"); return; }
    struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
    memcpy(p->payload, buf, mlen); udp_sendto(pcb, p, &dst, COAP_DEFAULT_PORT); pbuf_free(p);
    printf("Sent GET /send for '%s'\n", filename);
}

/* -------- Long-press state machine for BTN_SEND (GP22) -------- */
#define DEBOUNCE_MS 30
#define LONG_MS     1000

typedef struct {
    bool stable_level;         // 1 = not pressed (pull-up), 0 = pressed
    bool last_sample;
    absolute_time_t last_change;
    bool pressed;
    bool consumed;
    absolute_time_t pressed_at;
    bool long_active;
} btn_t;

static btn_t btn_send_sm;
static btn_t btn_req_sm;
static btn_t btn_log_sm;

static void btn_send_init(void) {
    gpio_init(BTN_SEND);
    gpio_set_dir(BTN_SEND, GPIO_IN);
    gpio_pull_up(BTN_SEND);
    bool level = gpio_get(BTN_SEND);
    btn_send_sm.stable_level = level;
    btn_send_sm.last_sample  = level;
    btn_send_sm.last_change  = get_absolute_time();
    btn_send_sm.pressed      = false;
    btn_send_sm.consumed     = false;
    btn_send_sm.long_active  = false;
}
static void btn_req_init(void) {
    gpio_init(BTN_REQ);
    gpio_set_dir(BTN_REQ, GPIO_IN);
    gpio_pull_up(BTN_REQ);
    
    bool level = gpio_get(BTN_REQ);
    btn_req_sm.stable_level = level;
    btn_req_sm.last_sample  = level;
    btn_req_sm.last_change  = get_absolute_time();
    btn_req_sm.pressed      = false;
    btn_req_sm.consumed     = false;
    btn_req_sm.long_active  = false;
}

static void btn_log_init(void) {
    gpio_init(BTN_LOG);
    gpio_set_dir(BTN_LOG, GPIO_IN);
    gpio_pull_up(BTN_LOG);

    bool level = gpio_get(BTN_LOG);
    btn_log_sm.stable_level = level;
    btn_log_sm.last_sample  = level;
    btn_log_sm.last_change  = get_absolute_time();
    btn_log_sm.pressed      = false;
    btn_log_sm.consumed     = false;
    btn_log_sm.long_active  = false;
}

typedef struct { bool short_press; bool long_press; } btn_events_t;

static btn_events_t btn_req_update(void) {
    btn_events_t ev = {0};
    bool sample = gpio_get(BTN_REQ);

    if (sample != btn_req_sm.last_sample) {
        btn_req_sm.last_sample = sample;
        btn_req_sm.last_change = get_absolute_time();
    }

    bool debounced = btn_req_sm.stable_level;
    if (to_ms_since_boot(get_absolute_time()) - to_ms_since_boot(btn_req_sm.last_change) >= DEBOUNCE_MS) {
        debounced = sample;
    }

    if (debounced != btn_req_sm.stable_level) {
        btn_req_sm.stable_level = debounced;
        if (debounced == 0) {
            // pressed
            btn_req_sm.pressed     = true;
            btn_req_sm.consumed    = false;
            btn_req_sm.long_active = false;
            btn_req_sm.pressed_at  = get_absolute_time();
        } else {
            // released
            if (btn_req_sm.pressed && !btn_req_sm.consumed) {
                ev.short_press = true;
            }
            btn_req_sm.pressed     = false;
            btn_req_sm.consumed    = false;
            btn_req_sm.long_active = false;
        }
    }

    // Long-press while held
    if (btn_req_sm.pressed && !btn_req_sm.long_active) {
        int held_ms = (int)(absolute_time_diff_us(btn_req_sm.pressed_at, get_absolute_time()) / 1000);
        if (held_ms >= LONG_MS) {
            ev.long_press = true;
            btn_req_sm.long_active = true;
            btn_req_sm.consumed    = true;
        }
    }

    return ev;
}

static btn_events_t btn_send_update(void) {
    btn_events_t ev = {0};
    bool sample = gpio_get(BTN_SEND);

    if (sample != btn_send_sm.last_sample) {
        btn_send_sm.last_sample = sample;
        btn_send_sm.last_change = get_absolute_time();
    }

    // Debounce: accept new level after DEBOUNCE_MS
    bool debounced = btn_send_sm.stable_level;
    if (absolute_time_diff_us(btn_send_sm.last_change, get_absolute_time()) <= 0) {
        // time passed since change
    }
    if (to_ms_since_boot(get_absolute_time()) - to_ms_since_boot(btn_send_sm.last_change) >= DEBOUNCE_MS) {
        debounced = sample;
    }

    if (debounced != btn_send_sm.stable_level) {
        btn_send_sm.stable_level = debounced;
        if (debounced == 0) {
            // pressed
            btn_send_sm.pressed     = true;
            btn_send_sm.consumed    = false;
            btn_send_sm.long_active = false;
            btn_send_sm.pressed_at  = get_absolute_time();
        } else {
            // released
            if (btn_send_sm.pressed && !btn_send_sm.consumed) {
                ev.short_press = true;  // short only if long didn't fire
            }
            btn_send_sm.pressed     = false;
            btn_send_sm.consumed    = false;
            btn_send_sm.long_active = false;
        }
    }

    // Long-press while held
    if (btn_send_sm.pressed && !btn_send_sm.long_active) {
        int held_ms = (int)(absolute_time_diff_us(btn_send_sm.pressed_at, get_absolute_time()) / 1000);
        if (held_ms >= LONG_MS) {
            ev.long_press = true;
            btn_send_sm.long_active = true;
            btn_send_sm.consumed    = true;   // block short on release
        }
    }

    return ev;
}

static btn_events_t btn_log_update(void) {
    btn_events_t ev = {0};
    bool sample = gpio_get(BTN_LOG);

    if (sample != btn_log_sm.last_sample) {
        btn_log_sm.last_sample = sample;
        btn_log_sm.last_change = get_absolute_time();
    }

    bool debounced = btn_log_sm.stable_level;
    if (to_ms_since_boot(get_absolute_time()) - to_ms_since_boot(btn_log_sm.last_change) >= DEBOUNCE_MS) {
        debounced = sample;
    }

    if (debounced != btn_log_sm.stable_level) {
        btn_log_sm.stable_level = debounced;
        if (debounced == 0) {
            // pressed
            btn_log_sm.pressed     = true;
            btn_log_sm.consumed    = false;
            btn_log_sm.long_active = false;
            btn_log_sm.pressed_at  = get_absolute_time();
        } else {
            // released
            if (btn_log_sm.pressed && !btn_log_sm.consumed) {
                ev.short_press = true;
            }
            btn_log_sm.pressed     = false;
            btn_log_sm.consumed    = false;
            btn_log_sm.long_active = false;
        }
    }

    // Long-press while held
    if (btn_log_sm.pressed && !btn_log_sm.long_active) {
        int held_ms = (int)(absolute_time_diff_us(btn_log_sm.pressed_at, get_absolute_time()) / 1000);
        if (held_ms >= LONG_MS) {
            ev.long_press = true;
            btn_log_sm.long_active = true;
            btn_log_sm.consumed    = true;
        }
    }

    return ev;
}

/* --------- Send a CON DELETE /delete with filename in payload --------- */
static void start_delete(const char* remote_filename) {
    ip_addr_t dst; if (!ip4addr_aton(SERVER_IP, ip_2_ip4(&dst))) return;

    char payload[64];
    snprintf(payload, sizeof(payload), "filename=%s", remote_filename);

    uint8_t tok[2] = {0xD0, 0x1E};           // arbitrary 2-byte token
    uint16_t mid = coap_next_msg_id(&coap);

    uint8_t buf[256];
    uint16_t mlen = coap_build_request(buf, sizeof(buf),
        COAP_DELETE, COAP_TYPE_CON, mid,
        tok, sizeof(tok),
        "delete",
        (const uint8_t*)payload, strlen(payload));   // (server also accepts Uri-Query, but payload is simplest)
    if (!mlen) { printf("ERROR: build DELETE /delete\n"); return; }

    struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
    memcpy(p->payload, buf, mlen);
    udp_sendto(pcb, p, &dst, COAP_DEFAULT_PORT);
    pbuf_free(p);
    printf("Sent DELETE /delete filename=%s (MID=%u)\n", remote_filename, mid);
}

/* --------- Outgoing Block1 POST to server with retransmit (GP22) ---------- */
static void build_upload_query(char* out, size_t out_sz,
                               const char* dest_filename,
                               const uint8_t* data, uint16_t len) {
    uint32_t crc = crc32_calc(data, len);
    snprintf(out, out_sz, "filename=%s&crc32=%08lX&size=%u",
             dest_filename, (unsigned long)crc, (unsigned)len);
}
static void send_tx_block_with_mid(uint32_t block_num, uint16_t mid, bool count_as_new_send) {
    ip_addr_t dst; if (!ip4addr_aton(SERVER_IP, ip_2_ip4(&dst))) return;
    uint32_t block_size = 1u << (tx_ctx.block_szx + 4); // 256
    uint32_t byte_offset = block_num * block_size;
    uint16_t payload_len = (byte_offset + block_size > tx_ctx.total_bytes)
        ? (tx_ctx.total_bytes - byte_offset) : (uint16_t)block_size;
    bool more = (byte_offset + payload_len < tx_ctx.total_bytes);

    uint8_t buf[768];
    char uri_query[64];
    build_upload_query(uri_query, sizeof(uri_query),
                       tx_ctx.dest_filename, tx_ctx.data, tx_ctx.total_bytes);

    uint16_t mlen = coap_build_block1_request(buf, sizeof(buf),
        COAP_POST, COAP_TYPE_CON, mid,
        tx_ctx.token, tx_ctx.tkl,
        "upload", uri_query,
        block_num, more, tx_ctx.block_szx,
        &tx_ctx.data[byte_offset], payload_len);
    if (!mlen) { printf("ERROR: build tx block\n"); return; }

    struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
    memcpy(p->payload, buf, mlen); udp_sendto(pcb, p, &dst, COAP_DEFAULT_PORT); pbuf_free(p);

    if (count_as_new_send) {
        tx_ctx.bytes_sent += payload_len;
        tx_ctx.current_block = block_num;
    }
    tx_ctx.ack_deadline = make_timeout_time_ms(2000); // wait for ACK

    printf("TX -> server Block=%lu, size=%u, more=%d, szx=%u, MID=%u %s\n",
           block_num, payload_len, more, tx_ctx.block_szx, mid,
           count_as_new_send ? "(new)" : "(retry)");
}
static void send_tx_block(uint32_t block_num) {
    tx_ctx.last_mid = tx_ctx.msg_id;
    send_tx_block_with_mid(block_num, tx_ctx.last_mid, true);
    tx_ctx.msg_id++;
    tx_ctx.retries = 0;
}
static bool start_upload_to_server(const char* src_local, const char* dest_remote) {
    if (!sd_mounted) { printf("SD not mounted\n"); return false; }
    FIL f; FRESULT fr; UINT br=0;
    fr = f_open(&f, src_local, FA_READ);
    if (fr != FR_OK) { printf("Open error: %d (%s)\n", fr, src_local); return false; }
    DWORD sz = f_size(&f);
    if (sz == 0 || sz > MAX_FILE_SIZE) { printf("Size error: %lu\n", (unsigned long)sz); f_close(&f); return false; }

    /* IMPORTANT: init tx_ctx BEFORE reading into tx_ctx.data (fixes zeroed payload bug) */
    memset(&tx_ctx, 0, sizeof(tx_ctx));
    tx_ctx.total_bytes = (uint16_t)sz;
    tx_ctx.block_szx   = 4; // 256B
    tx_ctx.in_progress = true;
    tx_ctx.msg_id      = coap_next_msg_id(&coap);
    tx_ctx.tkl         = 2; tx_ctx.token[0]=0xEE; tx_ctx.token[1]=0x22;
    strncpy(tx_ctx.dest_filename, dest_remote, sizeof(tx_ctx.dest_filename)-1);
    tx_ctx.retries = 0;
    tx_ctx.ack_deadline = get_absolute_time();

    /* Now read the file DIRECTLY into tx_ctx.data (no wipe afterwards) */
    fr = f_read(&f, tx_ctx.data, sz, &br);
    f_close(&f);
    if (fr != FR_OK || br != sz) { printf("Read error: %d (%lu/%lu)\n", fr, (unsigned long)br, (unsigned long)sz); tx_ctx.in_progress=false; return false; }

    printf("Start POST to server: %s (%u bytes) as %s\n", src_local, tx_ctx.total_bytes, tx_ctx.dest_filename);
    send_tx_block(0);
    return true;
}

/* ---------------- Incoming /upload from server/laptop (we are the client) ---------------- */
static void send_block1_ack(const coap_pkt_t* req, const ip_addr_t* addr, u16_t port,
                            uint8_t code, uint32_t block_num, bool more, uint8_t szx) {
    uint8_t out[128];
    uint16_t mlen = coap_build_block1_response(out, sizeof(out),
        COAP_TYPE_ACK, code, req->msg_id, req->token, req->tkl,
        block_num, more, szx, NULL, 0);
    if (!mlen) return;
    struct pbuf* rp = pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
    memcpy(rp->payload, out, mlen); udp_sendto(pcb, rp, addr, port); pbuf_free(rp);

    printf("ACK -> sender  Block=%lu, more=%d, szx=%u, code=0x%02X, MID=%u\n",
           block_num, more, szx, code, req->msg_id);
}
static void handle_file_upload(const coap_pkt_t* req, const ip_addr_t* addr, u16_t port) {
    if (!req->has_block1) {
        char filename[32] = "received.bin";
        const char* fn = strstr(req->uri_query, "filename=");
        if (fn) { fn+=9; size_t i=0; while (fn[i] && fn[i]!='&' && i<31){ filename[i]=fn[i]; i++; } filename[i]='\0'; }
        printf("RX single-block %u bytes -> %s\n", req->payload_len, filename);
        save_to_sd(filename, req->payload, req->payload_len);
        // Send a plain ACK (Changed)
        send_block1_ack(req, addr, port, COAP_CHANGED, 0, false, 0);
        return;
    }

    uint32_t block_size = 1u << (req->block1_szx + 4);
    uint32_t byte_offset = req->block1_num * block_size;

    printf("RX <- sender Block=%lu, size=%u/%lu, more=%d, szx=%u, offset=%lu, MID=%u\n",
           req->block1_num, req->payload_len, block_size, req->block1_more, req->block1_szx, byte_offset, req->msg_id);

    if (req->block1_num == 0) {
        file_ctx.bytes_received = 0; file_ctx.in_progress = true; file_ctx.last_block_num = 0;
        const char* fn = strstr(req->uri_query, "filename=");
        if (fn) { fn+=9; size_t i=0; while (fn[i] && fn[i]!='&' && i<31){ file_ctx.filename[i]=fn[i]; i++; } file_ctx.filename[i]='\0'; }
        else strcpy(file_ctx.filename, "received.bin");
        file_ctx.have_expectations =
            query_get_u32_hex(req->uri_query, "crc32", &file_ctx.expected_crc32) &&
            query_get_u32_dec(req->uri_query, "size",  &file_ctx.expected_size);
        printf("Begin file: %s (expect_crc:%s size:%s)\n",
               file_ctx.filename,
               file_ctx.have_expectations?"yes":"no",
               file_ctx.have_expectations?"yes":"no");
    } else if (!file_ctx.in_progress || req->block1_num != file_ctx.last_block_num + 1) {
        printf("RX error: out of sequence (expected %lu)\n", file_ctx.last_block_num + 1);
        send_block1_ack(req, addr, port, COAP_REQUEST_ENTITY_INCOMPLETE, req->block1_num, false, req->block1_szx);
        file_ctx.in_progress = false; return;
    }

    if (byte_offset + req->payload_len > MAX_FILE_SIZE) {
        printf("RX error: file too large\n");
        send_block1_ack(req, addr, port, COAP_REQUEST_ENTITY_TOO_LARGE, req->block1_num, false, req->block1_szx);
        file_ctx.in_progress = false; return;
    }

    if (req->payload_len) {
        memcpy(&file_ctx.data[byte_offset], req->payload, req->payload_len);
        file_ctx.bytes_received += req->payload_len;
        file_ctx.last_block_num = req->block1_num;
    }

    if (req->block1_more) {
        send_block1_ack(req, addr, port, COAP_CONTINUE, req->block1_num, false, req->block1_szx);
    } else {
        if (file_ctx.have_expectations) {
            bool size_ok = (file_ctx.bytes_received == file_ctx.expected_size);
            uint32_t got_crc = crc32_calc(file_ctx.data, file_ctx.bytes_received);
            bool crc_ok  = (got_crc == file_ctx.expected_crc32);
            printf("End file: %s bytes=%u size_ok=%d crc_ok=%d (crc=0x%08lX)\n",
                   file_ctx.filename, file_ctx.bytes_received, size_ok, crc_ok, (unsigned long)got_crc);
            if (!size_ok || !crc_ok) {
                send_block1_ack(req, addr, port, COAP_REQUEST_ENTITY_INCOMPLETE, req->block1_num, false, req->block1_szx);
                file_ctx.in_progress = false; return;
            }
        }
        save_to_sd(file_ctx.filename, file_ctx.data, file_ctx.bytes_received);
        send_block1_ack(req, addr, port, COAP_CHANGED, req->block1_num, false, req->block1_szx);
        file_ctx.in_progress = false;
    }
}

/* ---------------- UDP RX ---------------- */
static void rx_handler(void* arg, struct udp_pcb* upcb, struct pbuf* p,
                       const ip_addr_t* addr, u16_t port) {
    (void)arg; (void)upcb; if (!p) return;

    // parse full datagram
    coap_pkt_t req; uint16_t pkt_len = p->tot_len; uint8_t buf[1536];
    if (pkt_len > sizeof(buf)) pkt_len = sizeof(buf);
    pbuf_copy_partial(p, buf, pkt_len, 0);
    bool ok = coap_parse(buf, pkt_len, &req);
    if (!ok) { pbuf_free(p); return; }

    // Handle ACKs for our outgoing upload with retransmit
    if (req.type == COAP_TYPE_ACK && tx_ctx.in_progress) {
        printf("RX ACK <- server code=0x%02X, MID=%u", req.code, req.msg_id);
        if (req.has_block1)
            printf(", ack-block=%lu, more=%d, szx=%u", req.block1_num, req.block1_more, req.block1_szx);
        printf("\n");

        if (req.code == COAP_CONTINUE || req.code == COAP_CHANGED) {
            if (tx_ctx.bytes_sent < tx_ctx.total_bytes) {
                send_tx_block(tx_ctx.current_block + 1);
            } else {
                printf("Upload complete: %s (%u bytes)\n", tx_ctx.dest_filename, tx_ctx.total_bytes);
                tx_ctx.in_progress = false;
            }
        } else {
            printf("Upload aborted by server (code=0x%02X)\n", req.code);
            tx_ctx.in_progress = false;
        }
        pbuf_free(p);
        return;
    }

    // Observe notifications from SERVER: CON 2.05 Content with Observe
if (req.type == COAP_TYPE_CON && req.code == COAP_CONTENT && req.has_observe) {
    printf("OBS <- server %s:%u seq=%lu %.*s\n",
        ipaddr_ntoa(addr), port,
        (unsigned long)req.observe_seq,
        (int)req.payload_len, (const char*)req.payload);
    
    // CoAP requires empty ACK (code=0.00, no token)
    send_empty_ack(req.msg_id, addr, port);
    pbuf_free(p);
    return;
}

// Handle discovery response by TOKEN (responses don't have URIs)
if (req.type == COAP_TYPE_ACK && req.code == COAP_CONTENT && 
    req.tkl == 2 && req.token[0] == 0xD1 && req.token[1] == 0x5C) {
    printf("\n========== SERVER ENDPOINTS ==========\n");
    printf("%.*s\n", (int)req.payload_len, (const char*)req.payload);
    printf("======================================\n");
    pbuf_free(p);
    return;
}

    /* Generic responses (e.g. to register/send). These are not requests that we
       need to service, but the server may still be expecting an ACK when it
       replies with a confirmable message. */
    if ((req.code >> 5) >= 2) { // any 2.xx/4.xx/5.xx response class
        printf("RESP <- %s:%u type=%d code=0x%02X MID=%u\n",
               ipaddr_ntoa(addr), port, req.type, req.code, req.msg_id);
        if (req.type == COAP_TYPE_CON) {
            send_empty_ack(req.msg_id, addr, port);
        }
        pbuf_free(p);
        return;
    }

    if (req.code == COAP_POST && strcmp(req.uri,"upload")==0) {
        handle_file_upload(&req, addr, port);

    } else if (req.code == COAP_PUT && strcmp(req.uri,"led")==0) {
        int val = (req.payload && req.payload_len>0 && req.payload[0]=='1') ? 1 : 0;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, val);
        printf("LED=%d\n", val);

    } else {
            // For non-observe GET, just send a quick response
            uint8_t out[64];
            coap_type_t rt = (req.type == COAP_TYPE_CON) ? COAP_TYPE_ACK : COAP_TYPE_NON;
            uint16_t mlen = coap_build_response(out, sizeof(out),
                rt, COAP_CONTENT, req.msg_id,
                req.token, req.tkl,
                -1, NULL,
                (const uint8_t*)"ok", 2);

            if (mlen > 0) {
                struct pbuf* rp = pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
                if (rp) {
                    memcpy(rp->payload, out, mlen);
                    udp_sendto(pcb, rp, addr, port);
                    pbuf_free(rp);
                }
            }
        }    
    pbuf_free(p);
}


/* ---------------- main ---------------- */
int main() {
    stdio_init_all(); sleep_ms(1200); 
    
    btn_send_init();
    btn_req_init();
    printf("\nPico Client %s starting.\n", CLIENT_ID_STR);

    // Mount SD
    sleep_ms(1000);
    sd_card_t *pSD = sd_get_by_num(0);
    if (pSD) {
        FRESULT fr = f_mount(&fs, pSD->pcName, 1);
        if (fr == FR_OK) {
            sd_mounted = true;
            FATFS *fs_ptr; DWORD fre_clust;
            if (f_getfree("0:", &fre_clust, &fs_ptr) == FR_OK) {
                uint32_t free_mb = (fre_clust * fs_ptr->csize) / (1024*2);
                printf("SD Card: %lu MB free\n", free_mb);
            }
        } else {
            printf("SD Card: Mount failed (%d)\n", fr);
        }
    } else { printf("SD Card: Not detected\n"); }

    if (cyw43_arch_init()) { printf("WiFi init failed\n"); return -1; }
    cyw43_arch_enable_sta_mode();
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASS,
        CYW43_AUTH_WPA2_AES_PSK, 15000)) { printf("WiFi connect failed\n"); return -1; }
    printf("WiFi connected\n");

    // Blink 5s
    absolute_time_t start = get_absolute_time();
    while (absolute_time_diff_us(start, get_absolute_time()) < 5000000) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1); sleep_ms(200);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0); sleep_ms(200);
    }

    // Buttons (handled by state machines)
    // BTN_REQ handled by btn_req_init(); BTN_SEND by btn_send_init(); add BTN_LOG init
    btn_log_init();
    gpio_init(BTN_SEND); gpio_set_dir(BTN_SEND, GPIO_IN); gpio_pull_up(BTN_SEND);
    btn_last_send = gpio_get(BTN_SEND);

    coap_init(&coap);
    pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    udp_bind(pcb, IP_ANY_TYPE, COAP_DEFAULT_PORT);
    udp_recv(pcb, rx_handler, NULL);

    send_register();

    absolute_time_t last_reg = get_absolute_time();

    printf("Client ready. Endpoints it accepts: /upload and /led\n");

    while (true) {
        // Re-register every 60s
        if (absolute_time_diff_us(last_reg, get_absolute_time()) >= 60*1000000) {
            last_reg = get_absolute_time(); send_register();
        }

        // GP21: short press = request file, long press = discover endpoints
btn_events_t e_req = btn_req_update();
if (e_req.long_press) {
    request_discovery_from_server();
    // LED feedback: three quick blinks
    for (int i = 0; i < 3; i++) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1); 
        sleep_ms(100); 
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        sleep_ms(100);
    }
}
if (e_req.short_press) {
    request_file_from_server("hello.txt");
    // LED feedback: one blink
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1); 
    sleep_ms(120); 
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
}
        // GP20: short = append a line, long = subscribe/observe temp
        btn_events_t e_log = btn_log_update();
        if (e_log.long_press) {
            send_observe_temp_request_to_server();
            // LED feedback: two long blinks
            for (int i = 0; i < 2; i++) {
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1); sleep_ms(200);
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0); sleep_ms(200);
            }
        }
        if (e_log.short_press) {
            append_line_sd("hello.txt", "Client 1 has written to the SD CARD");
        }

        // GP22: short = SEND/upload, long (>=1s) = DELETE
        btn_events_t e = btn_send_update();
        if (e.long_press) {
            // long-hold to delete the same file we normally send
            start_delete("hello_updated.txt");
            // optional LED feedback: two quick blinks
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1); sleep_ms(120); cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
            sleep_ms(120);
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1); sleep_ms(120); cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        }
        if (e.short_press) {
            uint32_t copied = 0;
            if (copy_file_sd("hello.txt", "hello_updated.txt", tx_ctx.data, &copied, sizeof(tx_ctx.data))) {
                start_upload_to_server("hello_updated.txt", "hello_updated.txt");
                // optional LED feedback: one blink
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1); sleep_ms(120); cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
            }
        }

        // --- retransmit timer for outgoing upload ---
        if (tx_ctx.in_progress) {
            if (absolute_time_diff_us(get_absolute_time(), tx_ctx.ack_deadline) <= 0) {
                if (tx_ctx.retries < 4) {
                    tx_ctx.retries++;
                    send_tx_block_with_mid(tx_ctx.current_block, tx_ctx.last_mid, false);
                } else {
                    printf("Upload aborted: no ACK after %u retries (block %lu)\n",
                           tx_ctx.retries, tx_ctx.current_block);
                    tx_ctx.in_progress = false;
                }
            }
        }
        sleep_ms(10);
    }
}
