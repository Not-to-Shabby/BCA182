// Pure-C base64 (standard alphabet, RFC 4648). No ESP-IDF/mbedTLS deps, so
// host-unit-testable. RAOP needs both directions: decode Apple-Challenge /
// rsaaeskey / aesiv from RTSP+SDP, encode the signed challenge response.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Bytes required to encode n input bytes, including the NUL terminator.
static inline size_t base64_encoded_size(size_t n) { return ((n + 2) / 3) * 4 + 1; }

// Decode `in_len` base64 chars into `out` (<= out_cap bytes). Skips ASCII
// whitespace; accepts optional trailing '=' padding. Returns the number of
// decoded bytes, or -1 on an invalid character or if the result exceeds out_cap.
int base64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap);

// Encode `in_len` bytes to a NUL-terminated base64 string in `out`. Returns the
// string length (excluding NUL), or -1 if out_cap < base64_encoded_size(in_len).
int base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap);
