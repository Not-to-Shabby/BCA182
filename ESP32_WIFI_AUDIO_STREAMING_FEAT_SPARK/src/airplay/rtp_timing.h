// Pure codec for the RAOP timing channel (shairport rtp_timing_sender / _receiver).
// The RECEIVER is the initiator: it SENDS 0xd2 requests and CONSUMES 0xd3 responses
// (verified against shairport rtp.c 2026-07-17 — the task's "reply to requests"
// premise was inverted). We free-run: responses are parsed but NOT used for clock
// discipline; sending requests at a steady cadence keeps the sender from tearing us
// down. The request-parse / response-build pair exists ONLY for the documented
// defensive belt (answer a non-standard sender's 0xd2). No ESP-IDF deps.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define TIMING_PKT_LEN   32
#define TIMING_REQ_TYPE  0x52   // (pkt[1] & 0x7f); wire 0xd2
#define TIMING_RESP_TYPE 0x53   // (pkt[1] & 0x7f); wire 0xd3

typedef struct {                 // three NTP-64 stamps (sec<<32 | frac)
    uint64_t origin;             // bytes  8..15
    uint64_t receive;            // bytes 16..23
    uint64_t transmit;           // bytes 24..31
} timing_stamps_t;

// Build a 0xd2 REQUEST: transmit = our clock at send (t1); origin/receive = 0.
void rtp_timing_build_request(uint8_t out[TIMING_PKT_LEN], uint64_t transmit_ntp);

// Parse a 32-byte datagram; sets *out_type to TIMING_REQ_TYPE / TIMING_RESP_TYPE
// and fills *st. Returns 0 on success, -1 if len<32 or pkt[1]&0x7f is neither type.
int  rtp_timing_parse(const uint8_t *pkt, size_t len,
                      uint8_t *out_type, timing_stamps_t *st);

// Defensive belt only: build a 0xd3 RESPONSE echoing a client's transmit as origin,
// and stamping our receive/transmit clocks.
void rtp_timing_build_response(uint8_t out[TIMING_PKT_LEN],
                               uint64_t origin_ntp, uint64_t receive_ntp,
                               uint64_t transmit_ntp);
