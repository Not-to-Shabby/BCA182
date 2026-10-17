#include "rtsp_parser.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>

// Bounded, always-NUL-terminated copy of `srclen` bytes into a `dstsz` buffer.
static void copy_bounded(char *dst, size_t dstsz, const char *src, size_t srclen) {
    if (dstsz == 0) return;
    size_t n = (srclen < dstsz - 1) ? srclen : dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// First CRLF at or after `s`, strictly before `end`; NULL if none.
static const char *find_crlf(const char *s, const char *end) {
    for (const char *q = s; q + 1 < end; q++)
        if (q[0] == '\r' && q[1] == '\n') return q;
    return NULL;
}

bool rtsp_parse_request(const char *buf, size_t len, rtsp_request_t *out) {
    memset(out, 0, sizeof(*out));

    // Header terminator: the first blank line "\r\n\r\n". Absent -> not complete.
    const char *term = NULL;
    for (size_t i = 0; i + 4 <= len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            term = buf + i;
            break;
        }
    }
    if (!term) return false;

    // --- Request line: METHOD SP URI SP VERSION ---
    const char *rl_end = find_crlf(buf, term + 2);
    if (!rl_end) return false;  // defensive; term implies a CRLF exists
    {
        const char *s = buf;
        const char *sp1 = memchr(s, ' ', (size_t)(rl_end - s));
        if (sp1) {
            copy_bounded(out->method, sizeof(out->method), s, (size_t)(sp1 - s));
            const char *u = sp1 + 1;
            const char *sp2 = memchr(u, ' ', (size_t)(rl_end - u));
            if (sp2) {
                copy_bounded(out->uri, sizeof(out->uri), u, (size_t)(sp2 - u));
                const char *v = sp2 + 1;
                copy_bounded(out->version, sizeof(out->version), v, (size_t)(rl_end - v));
            } else {
                copy_bounded(out->uri, sizeof(out->uri), u, (size_t)(rl_end - u));
            }
        } else {
            copy_bounded(out->method, sizeof(out->method), s, (size_t)(rl_end - s));
        }
    }

    // --- Headers: from after the request-line CRLF up to the blank line ---
    const char *hp = rl_end + 2;
    while (hp < term) {
        const char *he = find_crlf(hp, term + 2);
        if (!he || he == hp) break;
        const char *colon = memchr(hp, ':', (size_t)(he - hp));
        if (colon && out->header_count < RTSP_MAX_HEADERS) {
            rtsp_header_t *h = &out->headers[out->header_count];
            copy_bounded(h->name, sizeof(h->name), hp, (size_t)(colon - hp));
            const char *vs = colon + 1;
            if (vs < he && *vs == ' ') vs++;  // trim one optional leading space
            copy_bounded(h->value, sizeof(h->value), vs, (size_t)(he - vs));
            out->header_count++;
        }
        hp = he + 2;
    }

    // --- Body: clamp to Content-Length; wait for more bytes if not all present ---
    const char *body = term + 4;
    size_t avail = len - (size_t)(body - buf);
    long cl = rtsp_content_length(out);  // headers already populated
    size_t body_len;
    if (cl < 0) {
        body_len = 0;  // RTSP request with no Content-Length carries no body
    } else {
        if ((size_t)cl > avail) return false;  // body not fully arrived yet
        body_len = (size_t)cl;
    }

    out->body = body_len ? body : NULL;
    out->body_len = body_len;
    out->total_len = (size_t)(body - buf) + body_len;
    out->valid = true;
    return true;
}

const char *rtsp_header_get(const rtsp_request_t *r, const char *name) {
    for (int i = 0; i < r->header_count; i++)
        if (strcasecmp(r->headers[i].name, name) == 0) return r->headers[i].value;
    return NULL;
}

int rtsp_cseq(const rtsp_request_t *r) {
    const char *v = rtsp_header_get(r, "CSeq");
    if (!v) return -1;
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v || n < 0) return -1;
    return (int)n;
}

long rtsp_content_length(const rtsp_request_t *r) {
    const char *v = rtsp_header_get(r, "Content-Length");
    if (!v) return -1;
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v || n < 0) return -1;
    return n;
}

int rtsp_transport_port(const char *transport, const char *key) {
    if (!transport || !key) return -1;
    size_t klen = strlen(key);
    const char *p = transport;
    while ((p = strstr(p, key)) != NULL) {
        bool at_start = (p == transport) || (p[-1] == ';') || (p[-1] == ' ');
        const char *after = p + klen;
        if (at_start && *after == '=') return atoi(after + 1);
        p = after;
    }
    return -1;
}

int rtsp_build_response(char *out, size_t out_cap, int status, const char *reason,
                        int cseq, const char *extra_headers,
                        const char *body, size_t body_len) {
    size_t off = 0;
    int n;

#define APPEND_FMT(...)                                                    \
    do {                                                                   \
        if (off >= out_cap) return -1;                                     \
        n = snprintf(out + off, out_cap - off, __VA_ARGS__);               \
        if (n < 0 || (size_t)n >= out_cap - off) return -1;                \
        off += (size_t)n;                                                  \
    } while (0)

    APPEND_FMT("RTSP/1.0 %d %s\r\n", status, reason ? reason : "");
    APPEND_FMT("CSeq: %d\r\n", cseq);
    APPEND_FMT("Server: Conduit/1.0\r\n");

    if (extra_headers) {
        size_t elen = strlen(extra_headers);
        if (off + elen >= out_cap) return -1;
        memcpy(out + off, extra_headers, elen);
        off += elen;
    }

    if (body) {
        APPEND_FMT("Content-Length: %zu\r\n", body_len);
    }

    APPEND_FMT("\r\n");  // end of headers

    if (body && body_len) {
        if (off + body_len > out_cap) return -1;
        memcpy(out + off, body, body_len);
        off += body_len;
    }

#undef APPEND_FMT
    return (int)off;
}
