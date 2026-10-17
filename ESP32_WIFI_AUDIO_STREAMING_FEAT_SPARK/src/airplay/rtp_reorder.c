#include "rtp_reorder.h"
#include <string.h>

static inline bool     seq_lt(uint16_t a, uint16_t b){ return (int16_t)(a - b) < 0; }
static inline uint16_t seq_diff(uint16_t a, uint16_t b){ return (uint16_t)(a - b); }

void rtp_reorder_init(rtp_reorder_t *rb, rtp_reorder_slot_t *slots,
                      uint16_t window, uint16_t hold){
    rb->slots = slots; rb->window = window;
    rb->hold = (hold && hold < window) ? hold : (uint16_t)(window - 1);
    rb->next = rb->head = rb->count = 0; rb->started = false;
    for (uint16_t i = 0; i < window; i++) slots[i].valid = false;
}
void rtp_reorder_anchor(rtp_reorder_t *rb, uint16_t first_seq){
    rb->next = rb->head = first_seq; rb->started = true;
}

rtp_ins_t rtp_reorder_insert(rtp_reorder_t *rb, uint16_t seq,
                             const uint8_t *data, size_t len){
    if (len == 0 || len > RTP_REORDER_MAX_PKT) return RTP_INS_BADLEN;
    if (!rb->started){ rb->next = rb->head = seq; rb->started = true; }
    if (seq_lt(seq, rb->next)) return RTP_INS_TOO_OLD;      // already past it
    if (seq_diff(seq, rb->next) >= rb->window) return RTP_INS_TOO_NEW;
    rtp_reorder_slot_t *sl = &rb->slots[seq % rb->window];
    if (sl->valid && sl->seq == seq) return RTP_INS_DUP;    // retransmit we already hold
    if (!sl->valid) rb->count++;
    sl->valid = true; sl->seq = seq; sl->len = (uint16_t)len;
    memcpy(sl->data, data, len);
    if (seq_lt(rb->head, (uint16_t)(seq + 1))) rb->head = (uint16_t)(seq + 1);
    return RTP_INS_STORED;
}

rtp_pop_t rtp_reorder_pop(rtp_reorder_t *rb, uint8_t *out, size_t out_cap,
                          size_t *out_len, uint16_t *out_seq){
    if (!rb->started || rb->count == 0) return RTP_POP_EMPTY;
    rtp_reorder_slot_t *sl = &rb->slots[rb->next % rb->window];
    if (sl->valid && sl->seq == rb->next){
        size_t n = sl->len; if (n > out_cap) n = out_cap;
        memcpy(out, sl->data, n);
        if (out_len) *out_len = n;
        if (out_seq) *out_seq = rb->next;
        sl->valid = false; rb->count--; rb->next++;
        return RTP_POP_OK;
    }
    // Gap at `next`. Hold unless the write head has run past the hold window.
    if (seq_diff(rb->head, rb->next) > rb->hold){
        rb->next++;                      // give up on the missing packet
        return RTP_POP_CONCEAL;
    }
    return RTP_POP_WAIT;
}

bool rtp_reorder_gap(const rtp_reorder_t *rb, uint16_t *first, uint16_t *count){
    if (!rb->started || rb->count == 0) return false;
    const rtp_reorder_slot_t *sl = &rb->slots[rb->next % rb->window];
    if (sl->valid && sl->seq == rb->next) return false;     // front is present
    uint16_t span = seq_diff(rb->head, rb->next);           // 1.. up to window
    uint16_t miss = 0;
    for (uint16_t i = 0; i < span && i < rb->window; i++){
        uint16_t s = (uint16_t)(rb->next + i);
        const rtp_reorder_slot_t *p = &rb->slots[s % rb->window];
        if (p->valid && p->seq == s) break;                 // first present seq ends the run
        miss++;
    }
    if (miss == 0) return false;
    if (first) *first = rb->next;
    if (count) *count = miss;
    return true;
}
