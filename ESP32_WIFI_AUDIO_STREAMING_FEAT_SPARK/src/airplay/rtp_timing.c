#include "rtp_timing.h"
static void wr_be64(uint8_t *p, uint64_t v){ for(int i=0;i<8;i++) p[i]=(uint8_t)(v>>(56-8*i)); }
static uint64_t rd_be64(const uint8_t *p){ uint64_t v=0; for(int i=0;i<8;i++) v=(v<<8)|p[i]; return v; }

void rtp_timing_build_request(uint8_t out[TIMING_PKT_LEN], uint64_t transmit_ntp){
    for (int i=0;i<TIMING_PKT_LEN;i++) out[i]=0;
    out[0]=0x80; out[1]=0xD2; out[2]=0x00; out[3]=0x07;   // 0x52|0x80, htons(7)
    wr_be64(out+24, transmit_ntp);                        // t1
}
void rtp_timing_build_response(uint8_t out[TIMING_PKT_LEN],
                               uint64_t origin_ntp, uint64_t receive_ntp,
                               uint64_t transmit_ntp){
    for (int i=0;i<TIMING_PKT_LEN;i++) out[i]=0;
    out[0]=0x80; out[1]=0xD3; out[2]=0x00; out[3]=0x07;   // 0x53|0x80, htons(7)
    wr_be64(out+8,  origin_ntp);
    wr_be64(out+16, receive_ntp);
    wr_be64(out+24, transmit_ntp);
}
int rtp_timing_parse(const uint8_t *pkt, size_t len,
                     uint8_t *out_type, timing_stamps_t *st){
    if (pkt == NULL || len < TIMING_PKT_LEN) return -1;
    uint8_t t = pkt[1] & 0x7f;
    if (t != TIMING_REQ_TYPE && t != TIMING_RESP_TYPE) return -1;
    if (out_type) *out_type = t;
    if (st){ st->origin=rd_be64(pkt+8); st->receive=rd_be64(pkt+16); st->transmit=rd_be64(pkt+24); }
    return 0;
}
