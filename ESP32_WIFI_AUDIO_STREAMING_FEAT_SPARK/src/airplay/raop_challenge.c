#include "raop_challenge.h"
#include <string.h>

int raop_challenge_assemble(const uint8_t *challenge, size_t clen,
                            const uint8_t ip4[4], const uint8_t mac[6],
                            uint8_t out[RAOP_CHALLENGE_BUF_LEN]) {
    if (clen != 16) return -1;               // AirPlay challenge is always 16 bytes
    memset(out, 0, RAOP_CHALLENGE_BUF_LEN);  // zero-pad tail
    memcpy(out + 0,  challenge, 16);
    memcpy(out + 16, ip4, 4);
    memcpy(out + 20, mac, 6);
    return 26;                               // 16 + 4 + 6 used; [26..31] stay zero
}
