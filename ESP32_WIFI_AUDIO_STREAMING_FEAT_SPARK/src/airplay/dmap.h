// Pure DMAP/DAAP TLV parser (spec §5b metadata). Walks an application/x-dmap-tagged
// SET_PARAMETER body — [4 ASCII code][BE32 length][value] tuples — extracting
// minm=title, asar=artist, asal=album as bounded UTF-8 strings. Recurses into the
// mlit listing-item container (senders may or may not wrap fields — the recursion
// handles both) up to a fixed depth cap (real senders nest <=1; the cap bounds the
// stack against a crafted nested-mlit chain, spec §9). Every length is bounded
// against the body (untrusted, spec §9):
// truncated/overrunning tags stop the walk, no read past the buffer, no allocation.
// No ESP-IDF deps -> host-unit-testable. Codes + layout cited to shairport rtsp.c.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define DMAP_STR_MAX 128    // bounded copy; sender strings are truncated to fit

typedef struct {
    char title [DMAP_STR_MAX];
    char artist[DMAP_STR_MAX];
    char album [DMAP_STR_MAX];
    bool has_title, has_artist, has_album;   // true when the tag was present
} dmap_meta_t;

// Parse `buf`/`len` into `out` (zeroed by the callee first). Returns the number of
// recognized text tags captured (0..3, counting repeats). Robust to flat OR
// mlit-wrapped bodies and to truncation/overflow (stops cleanly). vlen==0 yields an
// empty string with the has_* flag set (a deliberate field clear).
int dmap_parse(const uint8_t *buf, size_t len, dmap_meta_t *out);
