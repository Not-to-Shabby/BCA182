#include "aes_frame.h"

void aes_frame_split(size_t payload_len, aes_frame_split_t *out) {
    out->cipher_len = payload_len & ~(size_t)0x0f;   // largest multiple of 16
    out->plain_off  = out->cipher_len;
    out->plain_len  = payload_len - out->cipher_len; // trailing 0..15 plaintext bytes
}
