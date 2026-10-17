// RAOP crypto glue over mbedTLS 4.0 PSA Crypto (target-only; not host-testable —
// pure framing/layout is covered by base64/raop_challenge tests). Loads the
// well-known RAOP RSA key once, then signs Apple-Challenges and RSA/OAEP-decrypts
// the AES session key.
//
// Algorithm source of truth: shairport-sync 3.3.9 (rtsp.c apple_challenge /
// handle_announce + common.c rsa_apply). The RAOP operations map onto PSA:
//   Apple-Challenge sign : rsa_apply(RSA_MODE_AUTH) = PKCS#1 v1.5 raw private
//                          encrypt (MD_NONE) == PSA_ALG_RSA_PKCS1V15_SIGN_RAW
//   AES-key decrypt      : rsa_apply(RSA_MODE_KEY)  = RSAES-OAEP/SHA-1 private
//                          decrypt (PKCS_V21/MD_SHA1) == PSA_ALG_RSA_OAEP(SHA_1)
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

extern const char raop_key_pem[];          // defined in raop_key.c
extern const unsigned int raop_key_pem_len;

// Init PSA, parse the RAOP PEM, import two PSA keys (sign + decrypt), and run a
// self-test round-trip so a corrupted embedded key fails loud. Call once at boot.
esp_err_t raop_crypto_init(void);

// RSA/PKCS1-v1.5 raw-sign the 32-byte pre-signature buffer (PSA_ALG_RSA_PKCS1V15_
// SIGN_RAW). Writes the 256-byte signature to `sig`; returns 0 on success.
int raop_crypto_sign_challenge(const uint8_t buf32[32],
                               uint8_t *sig, size_t sig_cap, size_t *sig_len);

// RSA/OAEP-SHA1 decrypt the RSA-encrypted AES session key (PSA_ALG_RSA_OAEP(SHA_1)).
// Requires the decrypted length to be exactly 16; writes it to out16. Returns 0 on success.
int raop_crypto_decrypt_aeskey(const uint8_t *in, size_t in_len, uint8_t out16[16]);
