// Pure-C assembly of the RAOP Apple-Challenge pre-signature buffer. No ESP-IDF/
// mbedTLS deps, so host-unit-testable. Layout (shairport-sync compatible, IPv4):
//   [0..15]  16-byte decoded Apple-Challenge
//   [16..19]  4-byte local IPv4 (network byte order)
//   [20..25]  6-byte MAC
//   [26..31]  zero padding to 32 bytes
// raop_crypto then RSA/PKCS1-v1.5-signs this 32-byte buffer raw (no hash).
#pragma once
#include <stddef.h>
#include <stdint.h>

#define RAOP_CHALLENGE_BUF_LEN 32

// Zero-fill `out` (exactly 32 bytes) then lay in challenge||ip4||mac. `clen` must
// be 16 (AirPlay's challenge size); returns the used length before padding (26)
// on success, or -1 if clen != 16 (would not fit the fixed layout).
int raop_challenge_assemble(const uint8_t *challenge, size_t clen,
                            const uint8_t ip4[4], const uint8_t mac[6],
                            uint8_t out[RAOP_CHALLENGE_BUF_LEN]);
