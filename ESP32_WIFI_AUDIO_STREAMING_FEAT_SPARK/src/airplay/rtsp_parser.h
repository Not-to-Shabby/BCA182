// Pure-C RTSP/1.0 request parser + response builder. No ESP-IDF/mbedTLS deps,
// so host-unit-testable. Bounds every field against untrusted input (spec §9):
// over-long tokens are truncated but always NUL-terminated; the body is clamped
// to the bytes actually present. Header keys compare case-insensitively.
#pragma once
#include <stddef.h>
#include <stdbool.h>

#define RTSP_MAX_HEADERS 20
#define RTSP_MAX_NAME    32
#define RTSP_MAX_VALUE   384   // Apple-Challenge/Transport/RTP-Info fit; rsaaeskey rides in the SDP body

typedef struct { char name[RTSP_MAX_NAME]; char value[RTSP_MAX_VALUE]; } rtsp_header_t;

typedef struct {
    char           method[16];    // "OPTIONS", "ANNOUNCE", ...
    char           uri[128];      // request URI (truncated if longer)
    char           version[16];   // "RTSP/1.0"
    rtsp_header_t  headers[RTSP_MAX_HEADERS];
    int            header_count;
    const char    *body;          // points into `buf` after the blank line, or NULL
    size_t         body_len;      // min(Content-Length, bytes present after headers)
    size_t         total_len;     // bytes consumed by this request (headers + body_len)
    bool           valid;         // request line + headers well-formed
} rtsp_request_t;

// Parse a (possibly complete) RTSP request from `buf`/`len`. Returns true when
// the request line and header block are fully present (a blank CRLF line seen).
// On true, `body`/`body_len` describe the payload actually available.
bool rtsp_parse_request(const char *buf, size_t len, rtsp_request_t *out);

// Case-insensitive header lookup; NULL if absent.
const char *rtsp_header_get(const rtsp_request_t *r, const char *name);

// Convenience accessors. Return -1 if the header is absent or unparseable.
int  rtsp_cseq(const rtsp_request_t *r);
long rtsp_content_length(const rtsp_request_t *r);

// Extract an integer sub-field from a Transport header, e.g.
// rtsp_transport_port("RTP/AVP/UDP;unicast;control_port=6001;timing_port=6002",
//                     "control_port") -> 6001. Returns -1 if not found.
int rtsp_transport_port(const char *transport, const char *key);

// Build an RTSP response into `out`. Always emits the status line, the echoed
// CSeq, and Server; appends `extra_headers` verbatim (must be pre-formatted with
// trailing CRLF per line, or NULL); adds Content-Length + body when body != NULL.
// Returns total bytes written, or -1 on overflow.
int rtsp_build_response(char *out, size_t out_cap, int status, const char *reason,
                        int cseq, const char *extra_headers,
                        const char *body, size_t body_len);
