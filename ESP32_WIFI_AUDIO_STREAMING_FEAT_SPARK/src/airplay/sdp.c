#include "sdp.h"

#include <string.h>

// Bounded, always-NUL-terminated copy.
static void copy_bounded(char *dst, size_t dstsz, const char *src, size_t srclen) {
    if (dstsz == 0) return;
    size_t n = (srclen < dstsz - 1) ? srclen : dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// If `line`/`llen` begins with `prefix`, copy the remainder into `dst` and return true.
static bool take_attr(const char *line, size_t llen, const char *prefix,
                      char *dst, size_t dstsz) {
    size_t plen = strlen(prefix);
    if (llen < plen || memcmp(line, prefix, plen) != 0) return false;
    copy_bounded(dst, dstsz, line + plen, llen - plen);
    return true;
}

bool sdp_parse(const char *body, size_t len, sdp_media_t *out) {
    memset(out, 0, sizeof(*out));
    if (!body || len == 0) return false;

    size_t i = 0;
    while (i < len) {
        // Line = [i, eol); split on '\n', strip a trailing '\r'.
        size_t j = i;
        while (j < len && body[j] != '\n') j++;
        size_t llen = j - i;
        if (llen > 0 && body[i + llen - 1] == '\r') llen--;
        const char *line = body + i;

        if (take_attr(line, llen, "a=rsaaeskey:", out->rsaaeskey, sizeof(out->rsaaeskey)))
            out->has_rsaaeskey = true;
        else if (take_attr(line, llen, "a=aesiv:", out->aesiv, sizeof(out->aesiv)))
            out->has_aesiv = true;
        else if (take_attr(line, llen, "a=fmtp:", out->fmtp, sizeof(out->fmtp)))
            out->has_fmtp = true;

        i = j + 1;  // past the '\n'
    }

    return out->has_rsaaeskey || out->has_aesiv || out->has_fmtp;
}
