// Pure builders/parsers for the RAOP retransmit control packets (shairport rtp.c
// rtp_request_resend + the packet[1]==0xd6 resend-response path). No ESP-IDF deps.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define RESEND_REQ_LEN 8
#define RESEND_RESP_TYPE 0x56   // packet[1] & 0x7f for a resend response (wire 0xd6)
#define RESEND_RESP_HDR  4      // 4-byte 0xd6 wrapper before the inner audio packet

// Build the 8-byte resend REQUEST (receiver -> sender control port):
//   [0]=0x80 [1]=0xD5 [2..3]=htons(1) [4..5]=htons(first) [6..7]=htons(count)
void rtp_resend_build(uint8_t out[RESEND_REQ_LEN], uint16_t first_missing, uint16_t count);

// Validate an incoming resend RESPONSE (control socket) and expose the wrapped
// original audio packet. Returns 0 and sets *inner/*inner_len (= pkt+4 / len-4) when
// pkt[1]==0xd6 and len is big enough to hold a wrapped 12-byte RTP header; -1 else.
int  rtp_resend_unwrap(const uint8_t *pkt, size_t len,
                       const uint8_t **inner, size_t *inner_len);
