#include "base64.h"

static const char kEnc[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// -1 for non-alphabet chars; caller treats whitespace and '=' specially.
static int dec_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap) {
    size_t need = base64_encoded_size(in_len);
    if (out_cap < need) return -1;
    size_t o = 0;
    size_t i = 0;
    while (i + 3 <= in_len) {
        uint32_t n = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out[o++] = kEnc[(n >> 18) & 63];
        out[o++] = kEnc[(n >> 12) & 63];
        out[o++] = kEnc[(n >> 6) & 63];
        out[o++] = kEnc[n & 63];
        i += 3;
    }
    size_t rem = in_len - i;
    if (rem == 1) {
        uint32_t n = in[i] << 16;
        out[o++] = kEnc[(n >> 18) & 63];
        out[o++] = kEnc[(n >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        uint32_t n = (in[i] << 16) | (in[i + 1] << 8);
        out[o++] = kEnc[(n >> 18) & 63];
        out[o++] = kEnc[(n >> 12) & 63];
        out[o++] = kEnc[(n >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = '\0';
    return (int)o;
}

int base64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap) {
    uint32_t acc = 0;
    int nbits = 0;
    size_t o = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '=') break;                                   // padding: done
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;  // wrapped base64
        int v = dec_val(c);
        if (v < 0) return -1;                                  // invalid char
        acc = (acc << 6) | (uint32_t)v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            if (o >= out_cap) return -1;                       // bound the output (§9)
            out[o++] = (uint8_t)((acc >> nbits) & 0xFF);
        }
    }
    return (int)o;
}
