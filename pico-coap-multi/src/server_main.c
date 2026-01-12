#include <stdio.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/cyw43_arch.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "config.h"
#include "coap_min.h"
#include "f_util.h"
#include "ff.h"
#include "hw_config.h"
#include "hardware/adc.h"   // Temperature + Observe state (server is producer)


#define LED_GPIO_PIN 2  // GP2 LED
#define BTN_VIEW_OBSERVERS_PIN 21  // GP21 - View all observers
#define BTN_CLEAR_OBSERVERS_PIN 22 // GP22 - Clear all observers

static bool led_state = false;  // Tracks LED status

/* ---------- Client registry ---------- */
typedef struct {
    char      id[8];
    ip_addr_t addr;
    u16_t     port;
    uint32_t  last_seen_ms;
    bool      in_use;
} entry_t;

#define MAX_FILE_SIZE 8192

typedef struct {
    uint8_t  data[MAX_FILE_SIZE];
    uint16_t bytes_received;
    uint32_t last_block_num;
    uint32_t last_seen_ms;
    bool     in_progress;
    uint8_t  expected_szx;
    char     filename[32];
    bool     have_expectations;
    uint32_t expected_crc32;
    uint32_t expected_size;
} upload_ctx_t;

typedef struct {
    uint8_t  data[MAX_FILE_SIZE];
    uint16_t total_bytes;
    uint16_t bytes_sent;
    uint32_t current_block;
    uint8_t  block_szx; // 4 => 256B
    char     filename[32];
    char     target_id[8];
    ip_addr_t target_addr;
    u16_t     target_port;
    bool      in_progress;
    uint16_t  msg_id;
    uint8_t   token[8];
    uint8_t   tkl;
    uint32_t  crc32;
    uint32_t  total_size;
} forward_ctx_t;

typedef struct {
    uint8_t  data[MAX_FILE_SIZE];
    uint16_t bytes_received;
    uint32_t last_block_num;
    bool     in_progress;
    char     filename[32];
    char     target_id[8];
    uint32_t last_seen_ms;
} forward_upload_ctx_t;

typedef struct {
    bool in_use;
    ip_addr_t addr;
    u16_t port;
    uint8_t token[8];
    uint8_t tkl;
    uint32_t seq;

    // NEW FIELDS for retransmission
    uint8_t last_payload[64];   // last sent payload (small temp string)
    uint16_t last_mid;          // last message ID
    absolute_time_t ack_deadline;
    uint8_t retries;
    bool awaiting_ack;
} observer_t;

#define MAX_OBSERVERS 2
static observer_t observers[MAX_OBSERVERS];
static absolute_time_t next_obs_push;
static float last_temp = 9999.0f;        // sentinel to force first notify

/* ---------- Globals ---------- */
static entry_t              table[8];
static coap_ctx_t           coap;
static struct udp_pcb*      pcb;
static upload_ctx_t         upload_ctx = {0};
static forward_ctx_t        forward_ctx = {0};
static forward_upload_ctx_t fwd_rx = {0};
static FATFS                fs;
static bool                 sd_mounted = false;

/* ---------- CRC32 ---------- */
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

// Function to initialize LED pin
void led_init() {
    gpio_init(LED_GPIO_PIN);
    gpio_set_dir(LED_GPIO_PIN, GPIO_OUT);
    gpio_put(LED_GPIO_PIN, led_state);
    printf("LED GPIO %d initialized.\n", LED_GPIO_PIN);
}

// Function to initialize button pins
void buttons_init() {
    // GP21 - View observers button
    gpio_init(BTN_VIEW_OBSERVERS_PIN);
    gpio_set_dir(BTN_VIEW_OBSERVERS_PIN, GPIO_IN);
    gpio_pull_up(BTN_VIEW_OBSERVERS_PIN);
    
    // GP22 - Clear observers button
    gpio_init(BTN_CLEAR_OBSERVERS_PIN);
    gpio_set_dir(BTN_CLEAR_OBSERVERS_PIN, GPIO_IN);
    gpio_pull_up(BTN_CLEAR_OBSERVERS_PIN);
    
    printf("Buttons initialized: GP%d (view), GP%d (clear)\n", 
           BTN_VIEW_OBSERVERS_PIN, BTN_CLEAR_OBSERVERS_PIN);
}

// Function to view all active observers
void view_all_observers() {
    printf("\n=== Active Observers ===\n");
    int count = 0;
    for (int i = 0; i < MAX_OBSERVERS; i++) {
        if (observers[i].in_use) {
            count++;
            printf("Slot %d: %s:%u, seq=%lu, tkl=%u\n", 
                   i + 1,
                   ipaddr_ntoa(&observers[i].addr),
                   observers[i].port,
                   (unsigned long)observers[i].seq,
                   observers[i].tkl);
        }
    }
    if (count == 0) {
        printf("No active observers\n");
    } else {
        printf("Total: %d/%d slots used\n", count, MAX_OBSERVERS);
    }
    printf("========================\n\n");
}

// Function to clear all observers
void clear_all_observers() {
    printf("\n=== Clearing All Observers ===\n");
    int cleared = 0;
    for (int i = 0; i < MAX_OBSERVERS; i++) {
        if (observers[i].in_use) {
            printf("Clearing slot %d: %s:%u (seq=%lu)\n",
                   i + 1,
                   ipaddr_ntoa(&observers[i].addr),
                   observers[i].port,
                   (unsigned long)observers[i].seq);
            
            // Send unsubscribe notification to client
            uint8_t out[128];
            uint16_t mid = coap_next_msg_id(&coap);
            const char* msg = "Server cleared all observers";
            uint16_t mlen = coap_build_response(out, sizeof(out),
                COAP_TYPE_CON,
                COAP_CONTENT,
                mid,
                observers[i].token, observers[i].tkl,
                -1, NULL,
                (const uint8_t*)msg, strlen(msg));
            
            if (mlen) {
                struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
                if (p) {
                    memcpy(p->payload, out, mlen);
                    udp_sendto(pcb, p, &observers[i].addr, observers[i].port);
                    pbuf_free(p);
                    printf("  → Sent unsubscribe notification\n");
                }
            }
            
            memset(&observers[i], 0, sizeof(observer_t));
            cleared++;
        }
    }
    printf("Cleared %d observer(s)\n", cleared);
    printf("==============================\n\n");
}

// Debounce state for buttons
static bool btn_view_last_state = true;  // true = not pressed (pull-up)
static bool btn_clear_last_state = true;
static absolute_time_t btn_view_last_time;
static absolute_time_t btn_clear_last_time;

// Function to check button presses (call in main loop)
void check_buttons() {
    const uint32_t debounce_ms = 200;  // 200ms debounce
    
    // Check GP21 - View observers
    bool btn_view_current = gpio_get(BTN_VIEW_OBSERVERS_PIN);
    if (!btn_view_current && btn_view_last_state) {  // Button pressed (LOW)
        if (absolute_time_diff_us(btn_view_last_time, get_absolute_time()) > debounce_ms * 1000) {
            view_all_observers();
            btn_view_last_time = get_absolute_time();
        }
    }
    btn_view_last_state = btn_view_current;
    
    // Check GP22 - Clear observers
    bool btn_clear_current = gpio_get(BTN_CLEAR_OBSERVERS_PIN);
    if (!btn_clear_current && btn_clear_last_state) {  // Button pressed (LOW)
        if (absolute_time_diff_us(btn_clear_last_time, get_absolute_time()) > debounce_ms * 1000) {
            clear_all_observers();
            btn_clear_last_time = get_absolute_time();
        }
    }
    btn_clear_last_state = btn_clear_current;
}

// LED Resource Handler
static void handle_led_request(coap_pkt_t* pkt, const ip_addr_t* addr, u16_t port) {
    uint8_t buf[256];
    uint16_t len = 0;
    
    // Process PUT request (modify state)
    if (pkt->code == COAP_PUT) {
        if (pkt->payload && pkt->payload_len > 0) {
            // Expecting payload to be "on" or "off"
            if (strncmp((const char*)pkt->payload, "on", pkt->payload_len) == 0) {
                led_state = true;
                gpio_put(LED_GPIO_PIN, led_state);
                printf("LED/GP2 set to ON\n");
                len = coap_build_response(buf, sizeof(buf),
                    COAP_TYPE_ACK, COAP_CHANGED, pkt->msg_id,
                    pkt->token, pkt->tkl, -1, NULL, NULL, 0);
            } else if (strncmp((const char*)pkt->payload, "off", pkt->payload_len) == 0) {
                led_state = false;
                gpio_put(LED_GPIO_PIN, led_state);
                printf("LED/GP2 set to OFF\n");
                len = coap_build_response(buf, sizeof(buf),
                    COAP_TYPE_ACK, COAP_CHANGED, pkt->msg_id,
                    pkt->token, pkt->tkl, -1, NULL, NULL, 0);
            } else {
                // Bad Request if payload is neither "on" nor "off"
                len = coap_build_response(buf, sizeof(buf),
                    COAP_TYPE_ACK, COAP_BAD_REQUEST, pkt->msg_id,
                    pkt->token, pkt->tkl, -1, NULL, NULL, 0);
            }
        } else {
            // Bad Request if no payload
            len = coap_build_response(buf, sizeof(buf),
                COAP_TYPE_ACK, COAP_BAD_REQUEST, pkt->msg_id,
                pkt->token, pkt->tkl, -1, NULL, NULL, 0);
        }
    } 
    // Process GET request (query state)
    else if (pkt->code == COAP_GET) {
        const char* status = led_state ? "ON" : "OFF";
        printf("LED/GP2 GET status: %s\n", status);
        len = coap_build_response(buf, sizeof(buf),
            COAP_TYPE_ACK, COAP_CONTENT, pkt->msg_id,
            pkt->token, pkt->tkl, -1, NULL, 
            (const uint8_t*)status, strlen(status));
    }
    // Other methods are not allowed
    else {
        len = coap_build_response(buf, sizeof(buf),
            COAP_TYPE_ACK, COAP_METHOD_NOT_ALLOWED, pkt->msg_id,
            pkt->token, pkt->tkl, -1, NULL, NULL, 0);
    }

    if (len > 0) {
        struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
if (p) {
    memcpy(p->payload, buf, len);
    udp_sendto(pcb, p, addr, port);
    pbuf_free(p);
}
    }
}

static void temp_sensor_init(void) {
    cyw43_arch_lwip_begin();
    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(4);
    cyw43_arch_lwip_end();
    sleep_ms(10);
}
static float read_temp_c(void) {
    const int N = 8;
    uint32_t acc = 0;
    for (int i = 0; i < N; i++) { acc += adc_read(); sleep_us(200); }
    float v = ((float)acc / (float)N) * 3.3f / 4096.0f;
    return 27.0f - (v - 0.706f) / 0.001721f;
}

static void notify_one_observer(observer_t* ob, float temp) {
    char payload[32];
    snprintf(payload, sizeof(payload), "seq=%lu t=%.2f\n", (unsigned long)ob->seq, temp);

    uint8_t out[160];
    uint16_t mid = coap_next_msg_id(&coap);
    uint16_t mlen = coap_build_response(
        out, sizeof(out),
        COAP_TYPE_CON,
        COAP_CONTENT,
        mid,
        ob->token, ob->tkl,
        (int32_t)(ob->seq & 0x00FFFFFF),
        "temp",
        (const uint8_t*)payload, strlen(payload)
    );
    if (!mlen) return;

    struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
    if (!p) return;
    memcpy(p->payload, out, mlen);
    udp_sendto(pcb, p, &ob->addr, ob->port);
    pbuf_free(p);

    printf("Sent notification to %s:%u seq=%lu t=%.2f\n",
           ipaddr_ntoa(&ob->addr), ob->port, (unsigned long)ob->seq, temp);

    // --- Save for retransmission ---
    ob->last_mid = mid;
    memcpy(ob->last_payload, payload, sizeof(ob->last_payload));
    ob->ack_deadline = make_timeout_time_ms(2000);
    ob->retries = 0;
    ob->awaiting_ack = true;
}

/* ---------- Helpers ---------- */
static void upsert_client(const char* id, const ip_addr_t* a, u16_t port) {
    for (int i=0;i<8;i++) if (table[i].in_use && strcmp(table[i].id,id)==0) {
        ip_addr_set(&table[i].addr,a); table[i].port=port;
        table[i].last_seen_ms = to_ms_since_boot(get_absolute_time()); return;
    }
    for (int i=0;i<8;i++) if (!table[i].in_use) {
        table[i].in_use=true; strncpy(table[i].id,id,sizeof(table[i].id)-1);
        ip_addr_set(&table[i].addr,a); table[i].port=port;
        table[i].last_seen_ms = to_ms_since_boot(get_absolute_time()); return;
    }
}
static entry_t* find_id(const char* id) {
    for (int i=0;i<8;i++) if (table[i].in_use && strcmp(table[i].id,id)==0) return &table[i];
    return NULL;
}

/* Length-aware key=value parser for raw payloads */
static bool parse_kv_len(const char* s, size_t len,
                         const char* key, char* out, size_t out_sz) {
    if (!s||!len||!key||!out||!out_sz) return false;
    size_t klen=strlen(key), i=0;
    while (i<len) {
        while (i<len && (s[i]==' '||s[i]==','||s[i]==';')) i++;
        if (i+klen+1>len) break;
        if (!memcmp(&s[i], key, klen) && s[i+klen]=='=') {
            i += klen+1; size_t j=0;
            while (i<len && s[i]!=',' && s[i]!=';' && j+1<out_sz) out[j++]=s[i++];
            out[j]='\0'; return j>0;
        }
        while (i<len && s[i]!=',' && s[i]!=';') i++;
        if (i<len) i++;
    }
    return false;
}
static void extract_filename(const char* q, char* out, size_t out_sz) {
    const char* fn = strstr(q ? q : "", "filename=");
    if (!fn) { strncpy(out,"upload.bin",out_sz-1); out[out_sz-1]='\0'; return; }
    fn+=9; size_t i=0; while (fn[i] && fn[i]!='&' && i+1<out_sz) { out[i]=fn[i]; i++; }
    out[i]='\0';
}
static bool extract_query_value(const char* q, const char* key, char* out, size_t out_sz) {
    if (!q) return false;
    char pat[16]; snprintf(pat,sizeof(pat),"%s=",key);
    const char* p=strstr(q,pat); if(!p) return false; p+=strlen(pat);
    size_t i=0; while (p[i] && p[i]!='&' && i+1<out_sz) out[i]=p[i], i++;
    out[i]='\0'; return i>0;
}
static bool query_get_u32_hex(const char* q, const char* key, uint32_t* out) {
    if (!q) return false;
    char pat[16]; snprintf(pat,sizeof(pat),"%s=",key);
    const char* p = strstr(q, pat); if (!p) return false; p += strlen(pat);
    uint32_t v = 0; int n = 0;
    while (*p && *p!='&' && n < 8) {
        char c=*p++; uint8_t d=(c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:(c>='A'&&c<='F')?c-'A'+10:0xFF;
        if (d==0xFF) {
            break;
        } 
        v=(v<<4)|d; 
        n++;
    }
    *out = v; return n>0;
}
static bool query_get_u32_dec(const char* q, const char* key, uint32_t* out) {
    if (!q) return false;
    char pat[16]; snprintf(pat,sizeof(pat),"%s=",key);
    const char* p = strstr(q, pat); if (!p) return false; p += strlen(pat);
    uint32_t v=0; int n=0; while (*p && *p!='&') { if(*p<'0'||*p>'9') break; v=v*10+(*p-'0'); p++; n++; }
    *out=v; return n>0;
}



static void send_simple(const coap_pkt_t* req, const ip_addr_t* addr, u16_t port,
                        uint8_t code, const char* text) {
    uint8_t out[128];
    coap_type_t rt = (req->type==COAP_TYPE_CON)? COAP_TYPE_ACK : COAP_TYPE_NON;
    uint16_t mlen = coap_build_response(out,sizeof(out), rt, code, req->msg_id,
                                        req->token, req->tkl, -1, NULL,
                                        (const uint8_t*)text, text?strlen(text):0);
    if (!mlen) return;
    struct pbuf* rp=pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
    if (!rp) return; 
    memcpy(rp->payload,out,mlen);
    udp_sendto(pcb,rp,addr,port);
    pbuf_free(rp);
}

// Send 2.05 Content with arbitrary payload (binary or text)
static void send_content(const coap_pkt_t* req, const ip_addr_t* addr, u16_t port,
                         const uint8_t* payload, uint16_t plen) {
    uint8_t out[768];
    coap_type_t rt = (req->type==COAP_TYPE_CON)? COAP_TYPE_ACK : COAP_TYPE_NON;
    uint16_t mlen = coap_build_response(out, sizeof(out), rt, COAP_CONTENT, req->msg_id,
                                        req->token, req->tkl, -1, NULL, payload, plen);
    if (!mlen) return;
    struct pbuf* rp = pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
    if (!rp) return;
    memcpy(rp->payload, out, mlen);
    udp_sendto(pcb, rp, addr, port);
    pbuf_free(rp);
}

// Read a range from file (offset,len) -> buffer
static bool sd_read_range(const char* filename, uint32_t offset, uint32_t len,
                          uint8_t* out, uint32_t* out_len) {
    if (!sd_mounted) return false;
    FIL f; FRESULT fr; UINT rd=0;
    fr = f_open(&f, filename, FA_READ);
    if (fr != FR_OK) return false;
    DWORD fsz = f_size(&f);
    if (offset > fsz) { f_close(&f); return false; }
    f_lseek(&f, offset);
    fr = f_read(&f, out, len, &rd);
    f_close(&f);
    if (fr != FR_OK || rd == 0) return false;
    *out_len = rd; return true;
}

// Overwrite at offset with payload
static bool sd_write_at(const char* filename, uint32_t offset,
                        const uint8_t* data, uint32_t len) {
    if (!sd_mounted) return false;
    FIL f; FRESULT fr; UINT wr=0;
    fr = f_open(&f, filename, FA_WRITE | FA_OPEN_EXISTING);
    if (fr != FR_OK) return false;
    if (f_lseek(&f, offset) != FR_OK) { f_close(&f); return false; }
    fr = f_write(&f, data, len, &wr);
    f_close(&f);
    return (fr == FR_OK && wr == len);
}

// Handler: COAP FETCH /files?name=...&offset=...&len=...
static void handle_fetch(const coap_pkt_t* req, const ip_addr_t* addr, u16_t port) {
    // Route guard (if URI is not "files", reply 4.04)
    if (strcmp(req->uri, "files") != 0) { send_simple(req, addr, port, COAP_NOT_FOUND, "Not Found"); return; }
    char name[64]=""; if (!extract_query_value(req->uri_query, "name", name, sizeof(name))) {
        send_simple(req, addr, port, COAP_BAD_REQUEST, "name"); return;
    }
    // Parse filename
    uint32_t offset=0,len=0;
    if (!query_get_u32_dec(req->uri_query, "offset", &offset) ||
        !query_get_u32_dec(req->uri_query, "len", &len) || len == 0 || len > MAX_FILE_SIZE) {
        send_simple(req, addr, port, COAP_REQUEST_ENTITY_TOO_LARGE, "Request Entity Too Large"); return;
    }
    // Parse and validate range
    if (len > 768) { // current static buf size below
        send_simple(req, addr, port, COAP_BAD_REQUEST, "Too Big"); return;
    }
    
    uint8_t buf[768]; uint32_t got=0;
    if (!sd_read_range(name, offset, len, buf, &got)) {
        send_simple(req, addr, port, COAP_NOT_FOUND, "Not Found"); return;
    }
    send_content(req, addr, port, buf, (uint16_t)got);
}

// Handler: COAP PATCH /files?name=...&offset=...   (payload = bytes to write)
static void handle_patch(const coap_pkt_t* req, const ip_addr_t* addr, u16_t port) {
    if (strcmp(req->uri, "files") != 0) { send_simple(req, addr, port, COAP_NOT_FOUND, "nf"); return; }
    char name[64]=""; if (!extract_query_value(req->uri_query, "name", name, sizeof(name))) {
        send_simple(req, addr, port, COAP_BAD_REQUEST, "name"); return;
    }
    uint32_t offset=0; if (!query_get_u32_dec(req->uri_query, "offset", &offset)) {
        send_simple(req, addr, port, COAP_BAD_REQUEST, "offset"); return;
    }
    if (!req->payload || req->payload_len == 0 || req->payload_len > MAX_FILE_SIZE) {
        send_simple(req, addr, port, COAP_BAD_REQUEST, "data"); return;
    }
    if (!sd_write_at(name, offset, req->payload, req->payload_len)) {
        send_simple(req, addr, port, COAP_INTERNAL_SERVER_ERROR, "write"); return;
    }
    send_simple(req, addr, port, COAP_CHANGED, "OK");
}

/* ---------- DELETE helpers ---------- */
static bool is_valid_filename(const char* name) {
    if (!name || !*name) return false;
    for (const char* p = name; *p; ++p) {
        char c = *p;
        if (c == '/' || c == '\\') return false;      // no paths
        if (c == '.' && p[1] == '.') return false;    // no traversal
        if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))
            return false;
    }
    return true;
}

static FRESULT delete_file_on_sd(const char* filename) {
    if (!sd_mounted) return FR_NOT_READY;
    return f_unlink(filename);
}

/* Accept filename from payload "filename=..." or from Uri-Query "filename=" */
static bool extract_filename_any(const coap_pkt_t* req, char* out, size_t out_sz) {
    if (req->payload && req->payload_len) {
        if (parse_kv_len((const char*)req->payload, req->payload_len, "filename", out, out_sz))
            return true;
    }
    if (req->uri_query) {
        if (extract_query_value(req->uri_query, "filename", out, out_sz)) return true;
        extract_filename(req->uri_query, out, out_sz); // fallback default if present
        return out[0] != '\0';
    }
    return false;
}

/* ---------- Handler: DELETE /delete?filename=<name> ---------- */
static void handle_delete(const coap_pkt_t* req, const ip_addr_t* addr, u16_t port) {
    char filename[64] = "";
    if (!extract_filename_any(req, filename, sizeof(filename)) || !is_valid_filename(filename)) {
        send_simple(req, addr, port, COAP_BAD_REQUEST, "bad filename");
        return;
    }

    // /* Optional: refuse if file is currently being written/forwarded */
    // extern struct {
    //     bool in_progress;
    //     char filename[32];
    // } file_ctx; /* declared earlier in this file */
    // extern struct {
    //     bool in_progress;
    //     char filename[32];
    // } fwd_rx;   /* receive-from-laptop context */

    // if ( (file_ctx.in_progress  && strcmp(file_ctx.filename, filename) == 0) ||
    //      (fwd_rx.in_progress    && strcmp(fwd_rx.filename, filename) == 0) ) {
    //     send_simple(req, addr, port, COAP_CONFLICT, "busy");
    //     return;
    // }

    FRESULT fr = delete_file_on_sd(filename);
    if (fr == FR_OK) {
        send_simple(req, addr, port, COAP_DELETED, "Deleted");
        printf("DELETE: removed '%s'\n", filename);
    } else if (fr == FR_NO_FILE) {
        send_simple(req, addr, port, COAP_NOT_FOUND, "Not found");
    } else {
        char msg[24]; snprintf(msg, sizeof(msg), "FS err=%d", (int)fr);
        send_simple(req, addr, port, COAP_INTERNAL_SERVER_ERROR, msg);
    }
}

/* Save to SD */
static bool save_to_sd(const char* filename, const uint8_t* data, uint32_t size) {
    if (!sd_mounted) { printf("SD not mounted\n"); return false; }
    FIL file; FRESULT fr; UINT bw=0;
    fr=f_open(&file, filename, FA_CREATE_ALWAYS|FA_WRITE);
    if (fr!=FR_OK){ printf("SD: open(w) %s err=%d\n", filename, fr); return false; }
    fr=f_write(&file, data, size, &bw); f_close(&file);
    if (fr==FR_OK && bw==size){ printf("Saved: %s (%lu bytes)\n",filename,bw); return true; }
    printf("SD: write err=%d (wrote %lu/%lu)\n", fr, (unsigned long)bw, (unsigned long)size);
    return false;
}

/* Load from SD */
static bool load_from_sd(const char* filename, uint8_t* out, uint32_t* out_len, uint32_t max_len) {
    if (!sd_mounted) { printf("SD not mounted\n"); return false; }
    FIL f; FRESULT fr; UINT br=0;
    fr=f_open(&f, filename, FA_READ);
    if(fr!=FR_OK){ printf("SD: open(r) %s err=%d\n",filename,fr); return false; }
    DWORD sz=f_size(&f);
    if(sz==0||sz>max_len){ printf("SD: size %s=%lu (bad)\n",filename,(unsigned long)sz); f_close(&f); return false; }
    fr=f_read(&f,out,sz,&br); f_close(&f);
    if(fr!=FR_OK||br!=sz){ printf("SD: read err=%d (%lu/%lu)\n",fr,(unsigned long)br,(unsigned long)sz); return false; }
    *out_len=(uint32_t)sz; return true;
}

/* ---------- Forwarding (server -> client using /upload) ---------- */
static forward_ctx_t* FCTX=&forward_ctx;
static void build_forward_query(char* out, size_t out_sz) {
    snprintf(out,out_sz,"filename=%s&crc32=%08lX&size=%lu",
             FCTX->filename,(unsigned long)FCTX->crc32,(unsigned long)FCTX->total_size);
}
static void send_file_block(uint32_t block_num) {
    uint32_t block_size = 1u << (FCTX->block_szx + 4); // 256
    uint32_t byte_offset = block_num * block_size;
    uint16_t payload_len = (byte_offset + block_size > FCTX->total_bytes)
        ? (FCTX->total_bytes - byte_offset) : (uint16_t)block_size;
    bool more = (byte_offset + payload_len < FCTX->total_bytes);

    uint8_t buf[768];
    char uri_query[64]; build_forward_query(uri_query,sizeof(uri_query));
    uint16_t mlen = coap_build_block1_request(buf,sizeof(buf),
        COAP_POST, COAP_TYPE_CON, FCTX->msg_id, FCTX->token, FCTX->tkl,
        "upload", uri_query, block_num, more, FCTX->block_szx,
        &FCTX->data[byte_offset], payload_len);
    if (!mlen) { printf("ERROR: build block req\n"); return; }

    struct pbuf* p=pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
    memcpy(p->payload,buf,mlen); udp_sendto(pcb,p,&FCTX->target_addr,FCTX->target_port); pbuf_free(p);

    printf("TX -> client Block=%lu, size=%u, more=%d, szx=%u, MID=%u\n",
           block_num, payload_len, more, FCTX->block_szx, FCTX->msg_id);

    FCTX->bytes_sent += payload_len;
    FCTX->current_block = block_num;
    FCTX->msg_id++;
}
static void start_forward_to_client(const entry_t* e, const char* filename,
                                    const uint8_t* buf, uint16_t len) {
    memset(FCTX,0,sizeof(*FCTX));
    memcpy(FCTX->data,buf,len);
    FCTX->total_bytes=len; FCTX->total_size=len; FCTX->block_szx=4;
    strncpy(FCTX->filename,filename,sizeof(FCTX->filename)-1);
    strncpy(FCTX->target_id,e->id,sizeof(FCTX->target_id)-1);
    ip_addr_copy(FCTX->target_addr,e->addr); FCTX->target_port=e->port;
    FCTX->in_progress=true; FCTX->msg_id=coap_next_msg_id(&coap);
    FCTX->tkl=2; FCTX->token[0]=0xFD; FCTX->token[1]=0x01;
    FCTX->crc32=crc32_calc(FCTX->data,FCTX->total_bytes);
    printf("Now forwarding %u bytes to client %s (crc32=%08lX)\n",
           FCTX->total_bytes,FCTX->target_id,(unsigned long)FCTX->crc32);
    send_file_block(0);
}

/* ---------- Handlers ---------- */
static void handle_upload(const coap_pkt_t* req, const ip_addr_t* addr, u16_t port) {
    if (!req->has_block1) {
        char filename[32]; extract_filename(req->uri_query,filename,sizeof(filename));
        printf("RX <- client single %u bytes -> %s\n", req->payload_len, filename);
        if (req->payload_len && save_to_sd(filename, req->payload, req->payload_len))
            send_simple(req,addr,port,COAP_CHANGED,"Saved");
        else
            send_simple(req,addr,port,COAP_BAD_REQUEST,"No payload");
        return;
    }

    uint32_t block_size = 1u << (req->block1_szx + 4);
    uint32_t byte_offset = req->block1_num * block_size;

    printf("RX <- client Block=%lu, size=%u/%lu, more=%d, szx=%u, offset=%lu, MID=%u\n",
           req->block1_num, req->payload_len, block_size, req->block1_more, req->block1_szx, byte_offset, req->msg_id);

    if (req->block1_num == 0) {
        upload_ctx.bytes_received=0; upload_ctx.in_progress=true;
        upload_ctx.last_block_num=0; upload_ctx.expected_szx=req->block1_szx;
        extract_filename(req->uri_query, upload_ctx.filename, sizeof(upload_ctx.filename));
        upload_ctx.have_expectations =
            query_get_u32_hex(req->uri_query, "crc32", &upload_ctx.expected_crc32) &&
            query_get_u32_dec(req->uri_query, "size",  &upload_ctx.expected_size);
        printf("Begin upload: %s (expect_crc:%s size:%s)\n",
               upload_ctx.filename,
               upload_ctx.have_expectations?"yes":"no",
               upload_ctx.have_expectations?"yes":"no");
    } else if (!upload_ctx.in_progress || req->block1_num != upload_ctx.last_block_num+1) {
        send_simple(req,addr,port,COAP_REQUEST_ENTITY_INCOMPLETE,"order"); upload_ctx.in_progress=false; return;
    }

    if (byte_offset + req->payload_len > MAX_FILE_SIZE) {
        send_simple(req,addr,port,COAP_REQUEST_ENTITY_TOO_LARGE,"Too large"); upload_ctx.in_progress=false; return;
    }
    if (req->payload_len) {
        memcpy(&upload_ctx.data[byte_offset], req->payload, req->payload_len);
        upload_ctx.bytes_received += req->payload_len;
        upload_ctx.last_block_num = req->block1_num;
        upload_ctx.last_seen_ms = to_ms_since_boot(get_absolute_time());
    }

    if (req->block1_more) {
        uint8_t out[128];
        uint16_t mlen = coap_build_block1_response(out,sizeof(out),
            COAP_TYPE_ACK, COAP_CONTINUE, req->msg_id, req->token, req->tkl,
            req->block1_num, false, req->block1_szx, NULL, 0);
        struct pbuf* rp=pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
        memcpy(rp->payload,out,mlen); udp_sendto(pcb,rp,addr,port); pbuf_free(rp);
        printf("ACK -> client  Block=%lu, more=%d, szx=%u, code=2.31, MID=%u\n",
               req->block1_num, 0, req->block1_szx, req->msg_id);
    } else {
        if (upload_ctx.have_expectations) {
            if (upload_ctx.bytes_received != upload_ctx.expected_size) {
                send_simple(req, addr, port, COAP_REQUEST_ENTITY_INCOMPLETE, "size mismatch");
                upload_ctx.in_progress = false; return;
            }
            uint32_t got = crc32_calc(upload_ctx.data, upload_ctx.bytes_received);
            if (got != upload_ctx.expected_crc32) {
                send_simple(req, addr, port, COAP_REQUEST_ENTITY_INCOMPLETE, "crc mismatch");
                upload_ctx.in_progress = false; return;
            }
        }
        bool ok = save_to_sd(upload_ctx.filename, upload_ctx.data, upload_ctx.bytes_received);
        uint8_t out[128];
        uint16_t mlen = coap_build_block1_response(out,sizeof(out),
            COAP_TYPE_ACK, ok?COAP_CHANGED:COAP_GATEWAY_TIMEOUT, req->msg_id, req->token, req->tkl,
            req->block1_num, false, req->block1_szx, NULL, 0);
        struct pbuf* rp=pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
        memcpy(rp->payload,out,mlen); udp_sendto(pcb,rp,addr,port); pbuf_free(rp);
        printf("ACK -> client  Block=%lu, final, code=%s, MID=%u\n",
               req->block1_num, ok?"2.04 Changed":"5.04 Gateway Timeout", req->msg_id);
        upload_ctx.in_progress=false;
    }
}

/* ---------- Endpoint Registry ---------- */
typedef struct {
    const char* path;       // URI path (e.g. "upload")
    const char* method;     // HTTP-like method (GET, POST, DELETE)
    const char* desc;       // Description
} endpoint_t;

static const endpoint_t endpoints[] = {
    { "upload",   "POST",   "Upload file to server" },
    { "forward",  "POST",   "Forward file to another client" },
    { "register", "POST",   "Register client with ID" },
    { "send",     "GET",    "Push file to registered client" },
    { "delete",   "DELETE", "Delete file from server SD" },
    { "temp",     "GET",    "Read temperature (observable)" },
    { "discover", "GET",    "List all available endpoints" }
};

static const size_t endpoint_count = sizeof(endpoints) / sizeof(endpoints[0]);

/* ---------- Handler: GET /discover (dynamic) ---------- */
static void handle_discover(const coap_pkt_t* req, const ip_addr_t* addr, u16_t port) {
    char buffer[512];
    size_t offset = 0;
    int ret;

    // Build response in readable format: METHOD /path - description
    for (size_t i = 0; i < endpoint_count; i++) {
        ret = snprintf(buffer + offset, sizeof(buffer) - offset,
                      "%s /%s - %s\n",
                      endpoints[i].method,
                      endpoints[i].path,
                      endpoints[i].desc);
        
        // Check for overflow or error
        if (ret < 0 || (size_t)ret >= sizeof(buffer) - offset) {
            printf("WARN: /discover response truncated\n");
            break;
        }
        offset += (size_t)ret;
    }

    // Remove trailing newline if present
    if (offset > 0 && buffer[offset - 1] == '\n') {
        buffer[--offset] = '\0';
    }

    // Build CoAP response
    uint8_t out[768];
    coap_type_t rt = (req->type == COAP_TYPE_CON) ? COAP_TYPE_ACK : COAP_TYPE_NON;
    uint16_t mlen = coap_build_response(out, sizeof(out),
                                        rt, COAP_CONTENT, req->msg_id,
                                        req->token, req->tkl,
                                        -1, NULL,
                                        (const uint8_t*)buffer, (uint16_t)offset);

    if (!mlen) {
        printf("ERROR: failed to build /discover response\n");
        return;
    }

    struct pbuf* rp = pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
    if (!rp) {
        printf("ERROR: pbuf alloc failed for /discover\n");
        return;
    }

    memcpy(rp->payload, out, mlen);
    udp_sendto(pcb, rp, addr, port);
    pbuf_free(rp);

    printf("Sent /discover (%zu endpoints, %zu bytes) to %s:%u\n",
           endpoint_count, offset, ipaddr_ntoa(addr), port);
}

static void handle_forward(const coap_pkt_t* req, const ip_addr_t* addr, u16_t port) {
    char target[8]="", filename[32]="upload.bin";
    const char* q = req->uri_query ? req->uri_query : "";
    const char* tpos = strstr(q,"target=");
    if (tpos) { tpos+=7; size_t i=0; while (tpos[i] && tpos[i]!='&' && i<sizeof(target)-1){ target[i]=tpos[i]; i++; } target[i]='\0'; }
    extract_filename(q, filename, sizeof(filename));
    entry_t* e = target[0] ? find_id(target) : NULL; if (!e){ send_simple(req,addr,port,COAP_NOT_FOUND,"unknown id"); return; }

    if (!req->has_block1) {
        if (!req->payload_len) { send_simple(req,addr,port,COAP_BAD_REQUEST,"no payload"); return; }
        start_forward_to_client(e, filename, req->payload, req->payload_len);
        send_simple(req,addr,port,COAP_CHANGED,"Forwarding..."); return;
    }

    uint32_t block_size = 1u << (req->block1_szx + 4);
    uint32_t byte_offset = req->block1_num * block_size;

    if (req->block1_num == 0) {
        memset(&fwd_rx,0,sizeof(fwd_rx));
        fwd_rx.in_progress=true; strncpy(fwd_rx.filename,filename,sizeof(fwd_rx.filename)-1);
        strncpy(fwd_rx.target_id,target,sizeof(fwd_rx.target_id)-1);
        fwd_rx.last_block_num=0; fwd_rx.last_seen_ms=to_ms_since_boot(get_absolute_time());
        printf("Forward receive start: %s -> %s\n", fwd_rx.filename, fwd_rx.target_id);
    } else if (!fwd_rx.in_progress || req->block1_num != fwd_rx.last_block_num+1) {
        send_simple(req,addr,port,COAP_REQUEST_ENTITY_INCOMPLETE,"bad order"); fwd_rx.in_progress=false; return;
    }

    if (byte_offset + req->payload_len > MAX_FILE_SIZE) {
        send_simple(req,addr,port,COAP_REQUEST_ENTITY_TOO_LARGE,"Too large"); fwd_rx.in_progress=false; return;
    }

    if (req->payload_len) {
        memcpy(&fwd_rx.data[byte_offset], req->payload, req->payload_len);
        fwd_rx.bytes_received += req->payload_len;
        fwd_rx.last_block_num = req->block1_num;
        fwd_rx.last_seen_ms = to_ms_since_boot(get_absolute_time());
    }

    if (req->block1_more) {
        uint8_t out[128];
        uint16_t mlen = coap_build_block1_response(out,sizeof(out),
            COAP_TYPE_ACK, COAP_CONTINUE, req->msg_id, req->token, req->tkl,
            req->block1_num, false, req->block1_szx, NULL, 0);
        struct pbuf* rp=pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
        memcpy(rp->payload,out,mlen); udp_sendto(pcb,rp,addr,port); pbuf_free(rp);
        printf("ACK -> laptop  Block=%lu, more=%d, szx=%u, code=2.31, MID=%u\n",
               req->block1_num, 0, req->block1_szx, req->msg_id);
    } else {
        entry_t* ee = find_id(fwd_rx.target_id);
        if (!ee) { send_simple(req,addr,port,COAP_NOT_FOUND,"id gone"); fwd_rx.in_progress=false; return; }
        start_forward_to_client(ee, fwd_rx.filename, fwd_rx.data, fwd_rx.bytes_received);
        send_simple(req,addr,port,COAP_CHANGED,"Forwarding...");
        fwd_rx.in_progress=false;
    }
}
static void handle_send(const coap_pkt_t* req, const ip_addr_t* addr, u16_t port) {
    char target[8]=""; char filename[32]="hello.txt";
    bool ok_t=false, ok_f=false;
    if (req->payload && req->payload_len) {
        ok_t = parse_kv_len((const char*)req->payload, req->payload_len, "target", target, sizeof(target));
        ok_f = parse_kv_len((const char*)req->payload, req->payload_len, "filename", filename, sizeof(filename));
    }
    if (!ok_t && req->uri_query) ok_t = extract_query_value(req->uri_query,"target",target,sizeof(target));
    if (!ok_f && req->uri_query) { extract_filename(req->uri_query,filename,sizeof(filename)); ok_f=true; }
    if (!ok_t) { send_simple(req,addr,port,COAP_BAD_REQUEST,"need target"); return; }

    entry_t* e = find_id(target); if (!e) { send_simple(req,addr,port,COAP_NOT_FOUND,"unknown id"); return; }

    uint8_t buf[MAX_FILE_SIZE]; uint32_t len=0;
    if (!load_from_sd(filename, buf, &len, sizeof(buf))) { send_simple(req,addr,port,COAP_NOT_FOUND,"file"); return; }

    start_forward_to_client(e, filename, buf, (uint16_t)len);
    send_simple(req,addr,port,COAP_CONTENT,"sending");
    printf("SEND: pushing '%s' (%lu bytes) to %s\n", filename, (unsigned long)len, target);
}

// Send an empty ACK (Type=ACK, Code=0.00, TKL=0, no payload)
static void send_empty_ack(uint16_t msg_id, const ip_addr_t* addr, u16_t port) {
    uint8_t out[32];
    // coap_build_response params:
    // type=ACK, code=COAP_EMPTY(0.00), msg_id=msg_id, token=NULL, tkl=0, observe_seq=-1, uri=NULL, payload=NULL
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

/* ---------- UDP RX ---------- */
static void rx_handler(void* arg, struct udp_pcb* upcb, struct pbuf* p,
                       const ip_addr_t* addr, u16_t port) {
    (void)arg; (void)upcb; if (!p) return;

    coap_pkt_t req; uint16_t pkt_len = p->tot_len; uint8_t buf[1536];
    if (pkt_len > sizeof(buf)) pkt_len = sizeof(buf);
    pbuf_copy_partial(p, buf, pkt_len, 0);
    if (!coap_parse(buf, pkt_len, &req)) { pbuf_free(p); return; }

    // --- Handle ACKs for forwarding ---
    if (req.type==COAP_TYPE_ACK && forward_ctx.in_progress) {
        printf("RX ACK <- client code=0x%02X, MID=%u", req.code, req.msg_id);
        if (req.has_block1)
            printf(", ack-block=%lu, more=%d, szx=%u", req.block1_num, req.block1_more, req.block1_szx);
        printf("\n");
        if (req.code==COAP_CONTINUE || req.code==COAP_CHANGED) {
            if (forward_ctx.bytes_sent < forward_ctx.total_bytes)
                send_file_block(forward_ctx.current_block + 1);
            else {
                printf("Forward complete: %u bytes to %s\n", forward_ctx.total_bytes, forward_ctx.target_id);
                forward_ctx.in_progress=false;
            }
        } else {
            printf("Forward aborted: code=0x%02X\n", req.code);
            forward_ctx.in_progress=false;
        }
        pbuf_free(p);
        return;
    }

    if (req.type == COAP_TYPE_ACK) {
    // Match with observers waiting for ACK
    for (int i = 0; i < MAX_OBSERVERS; i++) {
        if (observers[i].in_use &&
            observers[i].awaiting_ack &&
            req.msg_id == observers[i].last_mid &&
            ip_addr_cmp(&observers[i].addr, addr) &&
            observers[i].port == port) {

            printf("ACK received for seq=%lu (MID=%u)\n",
                   (observers[i].seq > 0) ? observers[i].seq - 1 : 0, req.msg_id);
            observers[i].awaiting_ack = false;
        }
    }
    pbuf_free(p);
    return;
}

    // --- Regular endpoints ---
    if (req.code==COAP_POST && strcmp(req.uri,"register")==0) {
        char id[8]="";
        if (req.payload && parse_kv_len((const char*)req.payload, req.payload_len, "id", id, sizeof(id))) {
            upsert_client(id, addr, port);
            entry_t* e = find_id(id);
            
            printf("Registered %s -> %s:%u\n", id, ipaddr_ntoa(addr), port);
            send_simple(&req, addr, port, COAP_CHANGED, "OK");
        } else {
            send_simple(&req, addr, port, COAP_BAD_REQUEST, "need id");
        }

    } else if (req.code==COAP_POST && strcmp(req.uri,"upload") == 0) {
        handle_upload(&req, addr, port);

    } else if (req.code==COAP_POST && strcmp(req.uri,"forward") == 0) {
        handle_forward(&req, addr, port);

    } else if (req.code==COAP_GET && strcmp(req.uri,"send") == 0) {
        handle_send(&req, addr, port);

    } else if (req.code==COAP_DELETE && strcmp(req.uri,"delete") == 0){ 
        handle_delete(&req, addr, port); 

    } else if (req.code==COAP_GET && strcmp(req.uri,"discover") == 0){
        handle_discover(&req, addr, port);
    } else if (req.code==COAP_FETCH && strcmp(req.uri,"files") == 0) {
        handle_fetch(&req, addr, port);
    } else if (req.code==COAP_PATCH && strcmp(req.uri,"files") == 0) {
        handle_patch(&req, addr, port);
    } else if (req.code == COAP_GET && strcmp(req.uri, "temp") == 0) {
        // Handle observe registration on the server
        if (req.has_observe && req.observe_register) {
            int slot = -1;
            // Reuse same addr:port if present
            for (int i = 0; i < MAX_OBSERVERS; i++) {
                if (observers[i].in_use &&
                    ip_addr_cmp(&observers[i].addr, addr) &&
                    observers[i].port == port) { slot = i; break; }
            }
            // Otherwise grab a free slot
            if (slot < 0) for (int i = 0; i < MAX_OBSERVERS; i++) if (!observers[i].in_use) { slot = i; break; }

            if (slot >= 0) {
                observers[slot].in_use = true;
                ip_addr_copy(observers[slot].addr, *addr);
                observers[slot].port = port;
                observers[slot].tkl = (req.tkl <= 8) ? req.tkl : 8;
                memcpy(observers[slot].token, req.token, observers[slot].tkl);
                observers[slot].seq = 0;

                // ACK the registration
                uint8_t out[64];
                coap_type_t rt = (req.type == COAP_TYPE_CON) ? COAP_TYPE_ACK : COAP_TYPE_NON;
                uint16_t mlen = coap_build_response(out, sizeof(out),
                    rt, COAP_CONTENT, req.msg_id,
                    req.token, req.tkl,
                    0, NULL,   // use 0 for observe
                    (const uint8_t*)"registered", 10);
                if (mlen) {
                    struct pbuf* rp = pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
                    if (rp) { memcpy(rp->payload, out, mlen); udp_sendto(pcb, rp, addr, port); pbuf_free(rp); }
                }
                printf("Observer registered: %s:%u (slot %d/%d)\n", ipaddr_ntoa(addr), port, slot + 1, MAX_OBSERVERS);
            } else {
                // No free slots - observer list is full
                uint8_t out[128];
                coap_type_t rt = (req.type == COAP_TYPE_CON) ? COAP_TYPE_ACK : COAP_TYPE_NON;
                const char* msg = "Observer list full";
                uint16_t mlen = coap_build_response(out, sizeof(out),
                    rt, COAP_INTERNAL_SERVER_ERROR, req.msg_id,
                    req.token, req.tkl,
                    -1, NULL,
                    (const uint8_t*)msg, strlen(msg));
                if (mlen) {
                    struct pbuf* rp = pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
                    if (rp) { memcpy(rp->payload, out, mlen); udp_sendto(pcb, rp, addr, port); pbuf_free(rp); }
                }
                printf("Observer registration REJECTED: %s:%u - list full (%d/%d)\n", 
                       ipaddr_ntoa(addr), port, MAX_OBSERVERS, MAX_OBSERVERS);
            }
        } else if (req.has_observe && !req.observe_register) {
            // Handle unsubscribe (observe option with value 1)
            int slot = -1;
            for (int i = 0; i < MAX_OBSERVERS; i++) {
                if (observers[i].in_use &&
                    ip_addr_cmp(&observers[i].addr, addr) &&
                    observers[i].port == port) { 
                    slot = i; 
                    break; 
                }
            }
            
            if (slot >= 0) {
                // Found the observer - remove them
                memset(&observers[slot], 0, sizeof(observer_t));  // Clear entire structure
                printf("Observer unregistered: %s:%u (slot %d freed)\n", 
                       ipaddr_ntoa(addr), port, slot + 1);
                
                // Send acknowledgment
                uint8_t out[64];
                coap_type_t rt = (req.type == COAP_TYPE_CON) ? COAP_TYPE_ACK : COAP_TYPE_NON;
                uint16_t mlen = coap_build_response(out, sizeof(out),
                    rt, COAP_CONTENT, req.msg_id,
                    req.token, req.tkl,
                    -1, NULL,
                    (const uint8_t*)"unsubscribed", 12);
                if (mlen) {
                    struct pbuf* rp = pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
                    if (rp) { memcpy(rp->payload, out, mlen); udp_sendto(pcb, rp, addr, port); pbuf_free(rp); }
                }
            } else {
                // Not found - not currently subscribed
                printf("Unsubscribe attempt from non-subscriber: %s:%u\n", ipaddr_ntoa(addr), port);
                send_simple(&req, addr, port, COAP_NOT_FOUND, "not subscribed");
            }
        } else {
            // Non-observe GET /temp: quick content
            uint8_t out[64];
            coap_type_t rt = (req.type == COAP_TYPE_CON) ? COAP_TYPE_ACK : COAP_TYPE_NON;
            uint16_t mlen = coap_build_response(out, sizeof(out),
                rt, COAP_CONTENT, req.msg_id,
                req.token, req.tkl,
                -1, NULL,
                (const uint8_t*)"ok", 2);
            if (mlen) {
                struct pbuf* rp = pbuf_alloc(PBUF_TRANSPORT, mlen, PBUF_RAM);
                if (rp) { memcpy(rp->payload, out, mlen); udp_sendto(pcb, rp, addr, port); pbuf_free(rp); }
            }
        }
    } else if ((req.code == COAP_GET || req.code == COAP_PUT) && strcmp(req.uri, "led") == 0) {
        handle_led_request(&req, addr, port);
    } else {
        send_simple(&req, addr, port, COAP_NOT_FOUND, "Not found");
    }

    pbuf_free(p);
}



/* ---------- main ---------- */
int main() {
    stdio_init_all(); sleep_ms(1200);
    printf("\nCoAP Server Starting.\n");

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
        } else { printf("SD Card: Failed (%d)\n", fr); }
    }

    if (cyw43_arch_init()){ printf("WiFi init failed\n"); return -1; }
    cyw43_arch_enable_sta_mode();
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASS,
        CYW43_AUTH_WPA2_AES_PSK, 15000)){ printf("WiFi connect failed\n"); return -1; }

    struct netif* n = netif_list;
    printf("IP: %s\n", ip4addr_ntoa(netif_ip4_addr(n)));

    led_init();
    buttons_init();

    temp_sensor_init();
    next_obs_push = make_timeout_time_ms(1000);
    
    // Initialize button debounce timers
    btn_view_last_time = get_absolute_time();
    btn_clear_last_time = get_absolute_time();

    coap_init(&coap);
    pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    udp_bind(pcb, IP_ANY_TYPE, COAP_DEFAULT_PORT);
    udp_recv(pcb, rx_handler, NULL);

    printf("CoAP Server Ready (port %u)\n", COAP_DEFAULT_PORT);
    printf("Endpoints: /upload /forward /register /send /delete\n");
    printf("Hardware: GP21=View Observers, GP22=Clear Observers\n");
    
    while (true) { 
    tight_loop_contents(); 
    sleep_ms(10);
    
    // Check button presses
    check_buttons();
    
    if (absolute_time_diff_us(get_absolute_time(), next_obs_push) <= 0) {
        float t = read_temp_c();
        const float EPS = 0.10f;
        if (fabsf(t - last_temp) > EPS) {
            for (int i = 0; i < MAX_OBSERVERS; i++) {
                if (observers[i].in_use) {
                    notify_one_observer(&observers[i], t);
                    observers[i].seq++;
                }
            }
            last_temp = t;
        }
        next_obs_push = make_timeout_time_ms(1000);
    } 
}
}
