// Pure seq-indexed RTP reorder / jitter buffer. Holds ENCRYPTED audio payloads
// keyed by 16-bit RTP sequence so late / out-of-order / retransmitted packets
// slot into play order BEFORE decode (spec §6c/§5c). No ESP-IDF/crypto/socket
// deps — host-unit-testable. Storage is caller-owned (target: PSRAM); this file
// is allocator-free, mirroring audio_ringbuf.
//
// 16-bit seq wraparound is handled everywhere via modular comparison:
//   seq_lt(a,b)   == (int16_t)(a - b) < 0        (a strictly "before" b)
//   seq_diff(a,b) == (uint16_t)(a - b)           (packets from b up to a)
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define RTP_REORDER_MAX_PKT 1536   // max stored encrypted audio payload (bytes)

typedef struct {
    bool     valid;                 // slot holds an unread packet
    uint16_t seq;                   // its RTP sequence
    uint16_t len;                   // payload byte count (<= RTP_REORDER_MAX_PKT)
    uint8_t  data[RTP_REORDER_MAX_PKT];
} rtp_reorder_slot_t;

typedef struct {
    rtp_reorder_slot_t *slots;      // caller-owned, `window` entries
    uint16_t window;                // ring size in packets (target latency budget)
    uint16_t hold;                  // conceal threshold: max packets to hold at a gap
    uint16_t next;                  // next seq to pop (play cursor)
    uint16_t head;                  // highest seq seen + 1 (write head)
    uint16_t count;                 // packets currently buffered
    bool     started;               // false until anchored
} rtp_reorder_t;

typedef enum {
    RTP_INS_STORED = 0,  // accepted into its slot
    RTP_INS_DUP,         // slot already held this seq (retransmit of a pkt we have)
    RTP_INS_TOO_OLD,     // seq < next: already played/concealed — dropped
    RTP_INS_TOO_NEW,     // seq >= next+window: outside the window — dropped
    RTP_INS_BADLEN,      // len 0 or > RTP_REORDER_MAX_PKT — dropped
} rtp_ins_t;

typedef enum {
    RTP_POP_OK = 0,      // *out holds the next in-order packet; cursor advanced
    RTP_POP_WAIT,        // next seq missing but still within the hold window — caller
                         //   should wait / (re)request; cursor NOT advanced
    RTP_POP_CONCEAL,     // next seq missing AND hold exceeded — cursor advanced past
                         //   the hole; caller emits one concealment (silence) frame
    RTP_POP_EMPTY,       // nothing buffered / not started
} rtp_pop_t;

// Anchor the play cursor to the RECORD RTP-Info seq before the first insert.
// Optional: if never called, the first insert anchors to its own seq.
void      rtp_reorder_init(rtp_reorder_t *rb, rtp_reorder_slot_t *slots,
                           uint16_t window, uint16_t hold);
void      rtp_reorder_anchor(rtp_reorder_t *rb, uint16_t first_seq);

// Copy one encrypted audio payload into its seq slot. Returns a classification.
rtp_ins_t rtp_reorder_insert(rtp_reorder_t *rb, uint16_t seq,
                             const uint8_t *data, size_t len);

// Pop the next packet in seq order (see rtp_pop_t). On RTP_POP_OK, *out_len is set
// and up to out_cap bytes are copied to out.
rtp_pop_t rtp_reorder_pop(rtp_reorder_t *rb, uint8_t *out, size_t out_cap,
                          size_t *out_len, uint16_t *out_seq);

// If the play cursor is stalled on a gap (next missing but a later seq is present),
// report the resend range: *first = next, *count = consecutive missing from next up
// to the first buffered seq (clamped to `window`). Returns true if a resend is due.
bool      rtp_reorder_gap(const rtp_reorder_t *rb, uint16_t *first, uint16_t *count);

static inline size_t rtp_reorder_count(const rtp_reorder_t *rb) { return rb->count; }
