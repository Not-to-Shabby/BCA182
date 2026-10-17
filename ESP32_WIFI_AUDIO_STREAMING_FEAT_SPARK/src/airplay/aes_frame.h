// Pure-C framing decision for the RAOP AES-128-CBC audio payload. Decides ONLY
// which bytes are ciphertext vs the plaintext remainder — no crypto here, so the
// boundary logic is host-unit-testable with zero ESP-IDF/PSA includes.
//
// RAOP encrypts only the largest whole number of 16-byte blocks of the RTP audio
// payload; any trailing (payload_len % 16) bytes are transmitted as PLAINTEXT and
// copied verbatim. (shairport-sync rtp.c: `aeslen = plen & ~0xf;` then
// `memcpy(out+aeslen, in+aeslen, plen-aeslen);`.) The IV is reset to the fixed
// session IV for every packet — CBC chains within a packet, never across packets.
#pragma once
#include <stddef.h>

typedef struct {
    size_t cipher_len;   // payload_len & ~0xf  — feed these to AES-128-CBC decrypt
    size_t plain_off;    // == cipher_len        — offset where the plaintext tail starts
    size_t plain_len;    // payload_len - cipher_len — copied verbatim (plaintext)
} aes_frame_split_t;

// Compute the ciphertext/plaintext split for a payload of `payload_len` bytes.
void aes_frame_split(size_t payload_len, aes_frame_split_t *out);
