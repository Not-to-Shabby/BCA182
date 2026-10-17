// Pure-C extraction of the three RAOP-relevant SDP attributes from an ANNOUNCE
// body. No ESP-IDF/mbedTLS deps, so host-unit-testable. All fields are copied,
// bounded, and NUL-terminated (untrusted input, spec §9).
//   a=rsaaeskey:<base64>   RSA-encrypted AES session key
//   a=aesiv:<base64>       AES-CBC IV
//   a=fmtp:<params>        ALAC "magic cookie" params (kept for the Phase 3 decoder)
#pragma once
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    char rsaaeskey[512];  // base64 (RSA-2048 ciphertext -> 256 bytes -> ~344 b64 chars)
    char aesiv[64];       // base64 (16 bytes -> 24 chars)
    char fmtp[160];       // e.g. "96 352 0 16 40 10 14 2 255 0 0 44100"
    bool has_rsaaeskey, has_aesiv, has_fmtp;
} sdp_media_t;

// Parse the SDP `body`/`len`, filling the three attributes it finds. Returns true
// if at least one attribute was extracted. Missing attributes leave has_* false.
bool sdp_parse(const char *body, size_t len, sdp_media_t *out);
