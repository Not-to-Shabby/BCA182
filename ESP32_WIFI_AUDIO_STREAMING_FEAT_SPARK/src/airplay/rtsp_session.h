// Per-connection RAOP session state + the three receiver-side UDP sockets. The
// RTP receive loop that reads these sockets is Phase 3; Phase 2 binds them and
// reports their local ports in the SETUP Transport response.
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    RAOP_IDLE = 0,   // no ANNOUNCE yet
    RAOP_ANNOUNCED,  // AES key/iv + fmtp stored
    RAOP_SETUP,      // UDP sockets bound, ports exchanged
    RAOP_RECORDING,  // RECORD received; streaming would be live (Phase 3)
} raop_state_t;

typedef struct {
    raop_state_t state;

    uint8_t  aeskey[16];      // decrypted AES session key
    uint8_t  aesiv[16];       // AES-CBC IV
    bool     have_key;
    char     fmtp[160];       // ALAC magic-cookie params, kept for Phase 3

    // Sender-advertised ports (from the SETUP Transport header).
    int      client_control_port;
    int      client_timing_port;

    // Sender IPv4 (network byte order), captured via getpeername() on the RTSP
    // socket at RECORD. Lets timing/resend start immediately, without waiting for
    // the first audio packet to learn the peer (iOS drops us at ~2 s otherwise).
    uint32_t client_ip;

    // Receiver-side sockets we bind + their local ports (reported back in Transport).
    int      audio_fd,  control_fd,  timing_fd;
    uint16_t audio_port, control_port, timing_port;
} raop_session_t;

// Reset to IDLE, closing any open sockets and clearing key material.
void raop_session_reset(raop_session_t *s);

// Bind three UDP sockets to ephemeral ports; fills *_fd and *_port. Returns 0 on success.
int  raop_session_bind_udp(raop_session_t *s);

// Close the three fds (if open), set them to -1, zero the reported ports.
void raop_session_close_udp(raop_session_t *s);
