#include "raop_volume.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Find a line beginning with `prefix` within [body, body+len); return a pointer
// just past the prefix, or NULL. Lines are \r\n- or \n-separated; the body is NOT
// guaranteed NUL-terminated, so we never call str* past `len`.
static const char *find_line(const char *body, size_t len, const char *prefix) {
    size_t plen = strlen(prefix);
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && body[j] != '\n' && body[j] != '\r') j++;
        size_t line_len = j - i;
        if (line_len >= plen && memcmp(body + i, prefix, plen) == 0) {
            return body + i + plen;
        }
        while (j < len && (body[j] == '\n' || body[j] == '\r')) j++;
        i = j;
    }
    return NULL;
}

bool raop_parse_volume(const char *body, size_t len, float *out_db) {
    if (!body || !out_db) return false;
    const char *p = find_line(body, len, "volume: ");
    if (!p) return false;
    // Copy the remainder of the (bounded) value into a small NUL-terminated scratch
    // so strtof has a terminator; the value is short (e.g. "-14.500000").
    char tmp[32]; size_t n = 0;
    for (const char *q = p; q < body + len && *q != '\r' && *q != '\n' && n < sizeof(tmp) - 1; q++)
        tmp[n++] = *q;
    tmp[n] = '\0';
    char *endp = NULL;
    float v = strtof(tmp, &endp);
    if (endp == tmp) return false;     // no number parsed
    *out_db = v;
    return true;
}

bool raop_parse_progress(const char *body, size_t len,
                         uint32_t *start, uint32_t *cur, uint32_t *end) {
    if (!body) return false;
    const char *p = find_line(body, len, "progress: ");
    if (!p) return false;
    char tmp[48]; size_t n = 0;
    for (const char *q = p; q < body + len && *q != '\r' && *q != '\n' && n < sizeof(tmp) - 1; q++)
        tmp[n++] = *q;
    tmp[n] = '\0';
    unsigned long a = 0, b = 0, c = 0;
    if (sscanf(tmp, "%lu/%lu/%lu", &a, &b, &c) != 3) return false;
    if (start) *start = (uint32_t)a;
    if (cur)   *cur   = (uint32_t)b;
    if (end)   *end   = (uint32_t)c;
    return true;
}
