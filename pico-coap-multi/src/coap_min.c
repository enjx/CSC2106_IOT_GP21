#include "coap_min.h"
#include <string.h>

static void put_u16(uint8_t* p, uint16_t v){ p[0]=v>>8; p[1]=v&0xFF; }

void coap_init(coap_ctx_t* ctx){ memset(ctx,0,sizeof(*ctx)); ctx->next_msg_id=1; }

uint16_t coap_next_msg_id(coap_ctx_t* ctx){ if(++ctx->next_msg_id==0) ctx->next_msg_id=1; return ctx->next_msg_id; }

static bool read_options(const uint8_t* p, uint16_t rem, coap_pkt_t* out, uint16_t* consumed){
    uint16_t option_num=0, idx=0;
    out->uri[0]='\0';
    out->uri_query[0]='\0';
    out->has_block1 = false;
    out->block1_num = 0;
    out->block1_more = false;
    out->block1_szx = 0;
    out->size1 = 0;
    
    while (idx<rem){
        if (p[idx]==0xFF) break;
        uint8_t b=p[idx++];
        uint16_t delta=(b>>4)&0x0F, length=b&0x0F;
        uint16_t od=delta, ol=length;
        
        if (delta==13){ if(idx>=rem) return false; od=13+p[idx++]; }
        else if (delta==14){ if(idx+1>=rem) return false; od=269+(p[idx]<<8)+p[idx+1]; idx+=2; }
        else if (delta==15) return false;
        
        if (length==13){ if(idx>=rem) return false; ol=13+p[idx++]; }
        else if (length==14){ if(idx+1>=rem) return false; ol=269+(p[idx]<<8)+p[idx+1]; idx+=2; }
        else if (length==15) return false;
        
        option_num += od;
        if (idx+ol>rem) return false;
        
        if (option_num==COAP_OPT_URI_PATH){
            if (out->uri[0]!='\0') strncat(out->uri,"/",sizeof(out->uri)-1);
            strncat(out->uri,(const char*)&p[idx], ol < (sizeof(out->uri)-1)?ol:(sizeof(out->uri)-1));
        }
        else if (option_num==COAP_OPT_URI_QUERY){
            if (out->uri_query[0]!='\0') strncat(out->uri_query,"&",sizeof(out->uri_query)-1);
            strncat(out->uri_query,(const char*)&p[idx], ol < (sizeof(out->uri_query)-1)?ol:(sizeof(out->uri_query)-1));
        }
        else if (option_num==COAP_OPT_OBSERVE){
             out->has_observe = true;

            // Check if this is a request (code 0.01-0.31) or response (2.xx-5.xx)
            bool is_request = (out->code >= 1 && out->code <= 31);
            
            if (is_request && (ol == 0 || (ol == 1 && p[idx] == 0))) {
                // Request with Observe=0 means "register me"
                out->observe_register = true;
                out->observe_seq = 0;  // Store actual value, not -1
            } else {
                // Response with Observe option contains sequence number
                out->observe_register = false;
                uint32_t val = 0;
                if (ol == 0) {
                    val = 0;  // Empty option means 0
                } else {
                    for (int i = 0; i < ol; i++)
                        val = (val << 8) | p[idx + i];
                }
                out->observe_seq = (int32_t)val;
            }
        }
        else if (option_num==COAP_OPT_BLOCK1){
            out->has_block1 = true;
            uint32_t val = 0;
            for (int i=0; i<ol; i++) val = (val << 8) | p[idx+i];
            out->block1_num = val >> 4;
            out->block1_more = (val & 0x08) != 0;
            out->block1_szx = val & 0x07;
        }
        else if (option_num==COAP_OPT_SIZE1){
            uint32_t val = 0;
            for (int i=0; i<ol; i++) val = (val << 8) | p[idx+i];
            out->size1 = (uint16_t)val;
        }
        
        idx += ol;
    }
    
    *consumed=idx;
    out->payload = (idx<rem && p[idx]==0xFF) ? &p[idx+1] : NULL;
    out->payload_len = (idx<rem && p[idx]==0xFF) ? rem-idx-1 : 0;
    return true;
}

bool coap_parse(const uint8_t* buf, uint16_t len, coap_pkt_t* out){

    out->observe_seq = -1;
    out->has_observe = false;   

    if (len<4) return false;
    uint8_t v=(buf[0]>>6), t=(buf[0]>>4)&0x03, tkl=buf[0]&0x0F;
    if (v!=COAP_VER || tkl>8 || len<4+tkl) return false;
    
    out->type=(coap_type_t)t; out->code=buf[1];
    out->msg_id=(buf[2]<<8)|buf[3]; out->tkl=tkl; out->observe_register=false;
    out->payload=NULL; out->payload_len=0;
    out->uri[0]='\0';
    out->uri_query[0]='\0';
    
    if (tkl) memcpy(out->token,&buf[4],tkl);
    
    uint16_t consumed=0;
    return read_options(&buf[4+tkl], len-(4+tkl), out, &consumed);
}

static uint16_t write_option(uint8_t* out, uint16_t max, uint16_t* last_num,
                           uint16_t number, const uint8_t* val, uint16_t vlen){
    if (max<1) return 0;
    
    uint16_t delta = number - *last_num;
    uint8_t d,l, ext[4]; uint8_t el=0;
    
    if (delta<13) d=delta; else if (delta<269){ d=13; ext[el++]=delta-13; }
    else { d=14; uint16_t x=delta-269; ext[el++]=x>>8; ext[el++]=x&0xFF; }
    
    if (vlen<13) l=vlen; else if (vlen<269){ l=13; ext[el++]=vlen-13; }
    else { l=14; uint16_t x=vlen-269; ext[el++]=x>>8; ext[el++]=x&0xFF; }
    
    uint16_t need = 1+el+vlen; if (need>max) return 0;
    
    out[0]=(d<<4)|l; memcpy(&out[1],ext,el); memcpy(&out[1+el],val,vlen);
    *last_num=number; return need;
}

uint16_t coap_build_response(uint8_t* out, uint16_t out_max,
                           coap_type_t type, uint8_t code, uint16_t msg_id,
                           const uint8_t* token, uint8_t tkl, int32_t observe_seq,
                           const char* uri_path, const uint8_t* payload, uint16_t payload_len){
    if (out_max<4+tkl) return 0;
    
    out[0]=(COAP_VER<<6)|((type&3)<<4)|(tkl&0x0F);
    out[1]=code; put_u16(&out[2], msg_id);
    if (tkl && token) memcpy(&out[4],token,tkl);
    
    uint16_t idx=4+tkl, last=0;
    
    if (observe_seq>=0){
        uint8_t obs[3]; uint8_t olen=0;
        obs[0]=(observe_seq>>16)&0xFF; obs[1]=(observe_seq>>8)&0xFF; obs[2]=observe_seq&0xFF;
        olen = obs[0]?3:(obs[1]?2:1); if (!obs[0] && !obs[1]) obs[0]=obs[2];
        uint16_t w=write_option(&out[idx], out_max-idx, &last, COAP_OPT_OBSERVE, obs, olen);
        if (!w) return 0;
        idx+=w;
    }
    
    if (uri_path && *uri_path){
        const char* p=uri_path;
        while (*p){
            while (*p=='/') p++;
            if (!*p) break;
            const char* start=p;
            while (*p && *p!='/') p++;
            uint16_t seg_len=(uint16_t)(p-start);
            uint16_t w=write_option(&out[idx], out_max-idx, &last, COAP_OPT_URI_PATH, (const uint8_t*)start, seg_len);
            if (!w) return 0;
            idx+=w;
        }
    }
    
    if (payload && payload_len){
        if (idx+1>out_max) return 0;
        out[idx++]=0xFF;
        if (idx+payload_len>out_max) return 0;
        memcpy(&out[idx],payload,payload_len);
        idx+=payload_len;
    }
    
    return idx;
}

uint16_t coap_build_request(uint8_t* out, uint16_t out_max,
                          uint8_t method, coap_type_t type, uint16_t msg_id,
                          const uint8_t* token, uint8_t tkl, const char* uri_path,
                          const uint8_t* payload, uint16_t payload_len){
    if (out_max<4+tkl) return 0;
    
    out[0]=(COAP_VER<<6)|((type&3)<<4)|(tkl&0x0F);
    out[1]=method; put_u16(&out[2], msg_id);
    if (tkl && token) memcpy(&out[4],token,tkl);
    
    uint16_t idx=4+tkl, last=0;
    
    const char* p=uri_path;
    while (*p){
        while (*p=='/') p++;
        if (!*p) break;
        const char* start=p;
        while (*p && *p!='/') p++;
        uint16_t seg_len=(uint16_t)(p-start);
        uint16_t w=write_option(&out[idx], out_max-idx, &last, COAP_OPT_URI_PATH, (const uint8_t*)start, seg_len);
        if (!w) return 0;
        idx+=w;
    }
    
    if (payload && payload_len){
        if (idx+1>out_max) return 0;
        out[idx++]=0xFF;
        if (idx+payload_len>out_max) return 0;
        memcpy(&out[idx],payload,payload_len);
        idx+=payload_len;
    }
    
    return idx;
}

//get+observe
uint16_t coap_build_observe_get(uint8_t* out, uint16_t out_max,
    coap_type_t type, uint16_t msg_id,
    const uint8_t* token, uint8_t tkl,
    const char* uri_path)
{
    if (out_max < 4 + tkl) return 0;
    out[0] = (COAP_VER << 6) | ((type & 3) << 4) | (tkl & 0x0F);
    out[1] = COAP_GET;
    out[2] = msg_id >> 8;
    out[3] = msg_id & 0xFF;
    if (tkl && token) memcpy(&out[4], token, tkl);
    uint16_t idx = 4 + tkl;
    uint16_t last = 0;

    // URI path
    const char* p = uri_path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char* s = p;
        while (*p && *p != '/') p++;
        uint16_t seg_len = p - s;
        uint16_t w = write_option(&out[idx], out_max - idx, &last,
            COAP_OPT_URI_PATH, (const uint8_t*)s, seg_len);
        if (!w) return 0;
        idx += w;
    }

    // Observe=0 register
    uint16_t w = write_option(&out[idx], out_max - idx, &last,
        COAP_OPT_OBSERVE, NULL, 0);
    if (!w) return 0;
    idx += w;

    return idx;
}

uint16_t coap_build_block1_response(uint8_t* out, uint16_t out_max,
                                   coap_type_t type, uint8_t code, uint16_t msg_id,
                                   const uint8_t* token, uint8_t tkl,
                                   uint32_t block_num, bool more, uint8_t szx,
                                   const uint8_t* payload, uint16_t payload_len){
    if (out_max<4+tkl) return 0;
    
    out[0]=(COAP_VER<<6)|((type&3)<<4)|(tkl&0x0F);
    out[1]=code; put_u16(&out[2], msg_id);
    if (tkl && token) memcpy(&out[4],token,tkl);
    
    uint16_t idx=4+tkl, last=0;
    
    uint32_t block_val = (block_num << 4) | (more ? 0x08 : 0x00) | (szx & 0x07);
    uint8_t block_bytes[3];
    uint8_t block_len = 0;
    
    if (block_val > 0xFFFF) {
        block_bytes[0] = (block_val >> 16) & 0xFF;
        block_bytes[1] = (block_val >> 8) & 0xFF;
        block_bytes[2] = block_val & 0xFF;
        block_len = 3;
    } else if (block_val > 0xFF) {
        block_bytes[0] = (block_val >> 8) & 0xFF;
        block_bytes[1] = block_val & 0xFF;
        block_len = 2;
    } else {
        block_bytes[0] = block_val & 0xFF;
        block_len = 1;
    }
    
    uint16_t w = write_option(&out[idx], out_max-idx, &last, COAP_OPT_BLOCK1, block_bytes, block_len);
    if (!w) return 0;
    idx += w;
    
    if (payload && payload_len){
        if (idx+1>out_max) return 0;
        out[idx++]=0xFF;
        if (idx+payload_len>out_max) return 0;
        memcpy(&out[idx],payload,payload_len);
        idx+=payload_len;
    }
    
    return idx;
}

uint16_t coap_build_block1_request(uint8_t* out, uint16_t out_max,
    uint8_t method, coap_type_t type,
    uint16_t msg_id, const uint8_t* token, uint8_t tkl,
    const char* uri_path, const char* uri_query,
    uint32_t block_num, bool more, uint8_t szx,
    const uint8_t* payload, uint16_t payload_len) {
    
    if (out_max < 4 + tkl) return 0;
    
    // CoAP header
    out[0] = (COAP_VER<<6) | (type<<4) | (tkl & 0x0F);
    out[1] = method;
    out[2] = (msg_id >> 8) & 0xFF;
    out[3] = msg_id & 0xFF;
    
    // Token
    for (int i=0; i<tkl && i<8; i++) {
        out[4+i] = token[i];
    }
    
    uint16_t idx = 4 + tkl;
    uint16_t last_opt = 0;
    
    // URI-Path segments
    if (uri_path && uri_path[0]) {
        const char* segment = uri_path;
        while (*segment) {
            const char* end = segment;
            while (*end && *end != '/') end++;
            
            uint16_t len = end - segment;
            if (len > 0) {
                uint16_t w = write_option(&out[idx], out_max-idx, &last_opt, COAP_OPT_URI_PATH, 
                                        (const uint8_t*)segment, len);
                if (!w) return 0;
                idx += w;
            }
            
            segment = end;
            if (*segment == '/') segment++;
        }
    }
    
    // URI-Query
    if (uri_query && uri_query[0]) {
        uint16_t query_len = strlen(uri_query);
        uint16_t w = write_option(&out[idx], out_max-idx, &last_opt, COAP_OPT_URI_QUERY,
                                (const uint8_t*)uri_query, query_len);
        if (!w) return 0;
        idx += w;
    }
    
    // Block1 option (option 27)
    uint32_t block_val = (block_num << 4) | (more ? 0x08 : 0x00) | (szx & 0x07);
    uint8_t block_bytes[3];
    uint8_t block_len = 0;
    
    if (block_val > 0xFFFF) {
        block_bytes[0] = (block_val >> 16) & 0xFF;
        block_bytes[1] = (block_val >> 8) & 0xFF;
        block_bytes[2] = block_val & 0xFF;
        block_len = 3;
    } else if (block_val > 0xFF) {
        block_bytes[0] = (block_val >> 8) & 0xFF;
        block_bytes[1] = block_val & 0xFF;
        block_len = 2;
    } else {
        block_bytes[0] = block_val & 0xFF;
        block_len = 1;
    }
    
    uint16_t w = write_option(&out[idx], out_max-idx, &last_opt, COAP_OPT_BLOCK1, 
                             block_bytes, block_len);
    if (!w) return 0;
    idx += w;
    
    // Payload marker and payload
    if (payload && payload_len > 0) {
        if (idx + 1 + payload_len > out_max) return 0;
        out[idx++] = 0xFF;
        memcpy(&out[idx], payload, payload_len);
        idx += payload_len;
    }
    
    return idx;
}
