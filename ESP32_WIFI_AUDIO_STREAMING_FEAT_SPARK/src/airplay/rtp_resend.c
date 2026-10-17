#include "rtp_resend.h"
#define RTP_HDR_MIN 12
void rtp_resend_build(uint8_t out[RESEND_REQ_LEN], uint16_t first_missing, uint16_t count){
    out[0]=0x80; out[1]=0xD5;                 // 0x55 | 0x80
    out[2]=0x00; out[3]=0x01;                 // htons(1)
    out[4]=(uint8_t)(first_missing>>8); out[5]=(uint8_t)first_missing;
    out[6]=(uint8_t)(count>>8);         out[7]=(uint8_t)count;
}
int rtp_resend_unwrap(const uint8_t *pkt, size_t len,
                      const uint8_t **inner, size_t *inner_len){
    if (pkt == NULL || len < (size_t)(RESEND_RESP_HDR + RTP_HDR_MIN)) return -1;
    if ((pkt[1] & 0x7f) != RESEND_RESP_TYPE) return -1;   // require 0xd6
    if (inner) *inner = pkt + RESEND_RESP_HDR;
    if (inner_len) *inner_len = len - RESEND_RESP_HDR;
    return 0;
}
