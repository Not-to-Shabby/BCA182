#include "alac_config.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>

// Skip an optional leading "a=fmtp:" (SDP attribute prefix). shairport stores the
// value without it, but tolerate both forms.
static const char *skip_fmtp_prefix(const char *s) {
    static const char kPrefix[] = "a=fmtp:";
    if (strncmp(s, kPrefix, sizeof(kPrefix) - 1) == 0) {
        return s + (sizeof(kPrefix) - 1);
    }
    return s;
}

int alac_cfg_from_fmtp(const char *fmtp, alac_cfg_t *out) {
    if (fmtp == NULL || out == NULL) return -1;

    const char *p = skip_fmtp_prefix(fmtp);

    // Parse exactly 12 base-10 integers. strtoll advances `end` past each token and
    // skips leading whitespace, so a single loop handles arbitrary spacing.
    // NB: `long long` (64-bit on both host AND the 32-bit target), NOT `long`. On
    // the ESP32 `long` is 32-bit, so `(long)UINT32_MAX` overflows to -1 and every
    // uint32 upper-bound check below wrongly rejected valid fmtp — a target-only
    // bug the 64-bit host tests never saw.
    long long v[12];
    for (int i = 0; i < 12; i++) {
        char *end = NULL;
        long long n = strtoll(p, &end, 10);
        if (end == p) return -1;   // no digits where an integer was expected
        v[i] = n;
        p = end;
    }
    // Reject trailing junk that is not just whitespace (e.g. "1 2 ... hello").
    // Extra integers are also rejected to keep the mapping unambiguous.
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '\0') return -1;

    // Range-check each codec field into its destination width before assigning.
    // v[0] (payload type) is deliberately not consumed.
    if (v[1]  < 0 || v[1]  > (long long)UINT32_MAX) return -1;  // frame_length
    if (v[2]  < 0 || v[2]  > 0xFF)             return -1;  // compat_version
    if (v[3]  < 0 || v[3]  > 0xFF)             return -1;  // bit_depth
    if (v[4]  < 0 || v[4]  > 0xFF)             return -1;  // pb
    if (v[5]  < 0 || v[5]  > 0xFF)             return -1;  // mb
    if (v[6]  < 0 || v[6]  > 0xFF)             return -1;  // kb
    if (v[7]  < 0 || v[7]  > 0xFF)             return -1;  // num_channels
    if (v[8]  < 0 || v[8]  > 0xFFFF)           return -1;  // max_run
    if (v[9]  < 0 || v[9]  > (long long)UINT32_MAX) return -1;  // max_frame_bytes
    if (v[10] < 0 || v[10] > (long long)UINT32_MAX) return -1;  // avg_bitrate
    if (v[11] < 0 || v[11] > (long long)UINT32_MAX) return -1;  // sample_rate

    out->frame_length    = (uint32_t)v[1];
    out->compat_version  = (uint8_t) v[2];
    out->bit_depth       = (uint8_t) v[3];
    out->pb              = (uint8_t) v[4];
    out->mb              = (uint8_t) v[5];
    out->kb              = (uint8_t) v[6];
    out->num_channels    = (uint8_t) v[7];
    out->max_run         = (uint16_t)v[8];
    out->max_frame_bytes = (uint32_t)v[9];
    out->avg_bitrate     = (uint32_t)v[10];
    out->sample_rate     = (uint32_t)v[11];
    return 0;
}
