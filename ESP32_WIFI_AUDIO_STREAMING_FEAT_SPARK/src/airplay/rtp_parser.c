#include "rtp_parser.h"

static inline uint16_t rd_be16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
}

static inline uint32_t rd_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8  | (uint32_t)p[3];
}

int rtp_parse(const uint8_t *pkt, size_t len, rtp_header_t *out) {
    if (pkt == NULL || out == NULL || len < RTP_HEADER_LEN) return -1;

    out->payload_type = pkt[1] & 0x7f;   // strip the marker bit
    out->marker       = (pkt[1] & 0x80) != 0;
    out->seq          = rd_be16(pkt + 2);
    out->timestamp    = rd_be32(pkt + 4);
    out->ssrc         = rd_be32(pkt + 8);
    out->payload_len  = len - RTP_HEADER_LEN;
    out->payload      = (out->payload_len > 0) ? (pkt + RTP_HEADER_LEN) : NULL;
    return 0;
}
