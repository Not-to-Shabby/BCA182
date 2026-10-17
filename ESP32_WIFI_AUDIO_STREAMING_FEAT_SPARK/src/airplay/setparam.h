// Pure decision: given a SET_PARAMETER Content-Type header value, decide how the
// body is handled. Keeps the dispatch policy out of the socket glue and unit-tests
// it directly. Matching is case-insensitive on the media type; parameters after a
// ';' (e.g. "; charset=...") are ignored. Mirrors shairport rtsp.c's dispatch on
// text/parameters vs application/x-dmap-tagged. No ESP-IDF deps.
#pragma once

typedef enum {
    SETPARAM_VOLUME = 0,   // text/parameters           -> volume:/progress: line parse
    SETPARAM_METADATA,     // application/x-dmap-tagged  -> DMAP TLV parse
    SETPARAM_OTHER,        // image/jpeg, image/png, unknown, or absent -> ignore
} setparam_kind_t;

// content_type may be NULL (absent header) -> SETPARAM_OTHER.
setparam_kind_t setparam_classify(const char *content_type);
