// Pure-C parser for the 12-byte RAOP RTP header (audio UDP datagrams). No
// ESP-IDF/lwip includes, so it is host-unit-testable. Big-endian fields are read
// with explicit shifts (no ntohs/ntohl) to stay portable and dependency-free.
//
// Wire layout (shairport-sync rtp.c; research-phase3-5.md agent 3, critic G1/G2):
//   offset size field
//     0     1   0x80  (V=2,P=0,X=0,CC=0)
//     1     1   payload type; MARKER is the HIGH bit of THIS byte. Audio = 0x60,
//               first audio packet = 0xE0 (0x60|0x80). Control types (0x52..0x56)
//               carry the 0x80 bit set. Dispatch on (byte1 & 0x7f).
//     2     2   sequence number   big-endian
//     4     4   RTP timestamp     big-endian (+frameLength per packet @44100)
//     8     4   SSRC              big-endian (ignored by the receiver)
//    12     N   payload (AES-128-CBC encrypted ALAC frame)
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define RTP_HEADER_LEN 12
#define RTP_PT_AUDIO   0x60   // payload type 96, marker bit clear

typedef struct {
    uint8_t        payload_type;  // packet[1] & 0x7f
    bool           marker;        // packet[1] & 0x80 (set on the first audio packet)
    uint16_t       seq;           // big-endian @2
    uint32_t       timestamp;     // big-endian @4
    uint32_t       ssrc;          // big-endian @8
    const uint8_t *payload;       // packet + 12 (NULL if payload_len == 0)
    size_t         payload_len;   // len - 12
} rtp_header_t;

// Parse one datagram. Returns 0 on success (len >= RTP_HEADER_LEN), -1 otherwise.
// Does NOT validate payload_type — the caller dispatches on out->payload_type.
int rtp_parse(const uint8_t *pkt, size_t len, rtp_header_t *out);
