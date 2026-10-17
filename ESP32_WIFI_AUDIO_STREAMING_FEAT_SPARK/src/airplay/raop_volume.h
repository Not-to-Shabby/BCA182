// Pure parsers for RAOP SET_PARAMETER "text/parameters" bodies. No ESP-IDF deps.
// Bodies are untrusted (spec §9): bounded scan, no allocation, tolerant of a
// missing trailing \r\n, and never read past body+len (the RTSP body pointer is
// NOT NUL-terminated — it points into the recv buffer).
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// Scan a text/parameters body for a "volume: <float>\r\n" line. On match writes
// the dB value and returns true. Matches the exact prefix "volume: " (8 bytes,
// note the space) — shairport uses strncmp(cp, "volume: ", 8).
bool raop_parse_volume(const char *body, size_t len, float *out_db);

// Scan for "progress: <u32>/<u32>/<u32>". On match writes the three RTP timestamps
// (44100 Hz) and returns true. Optional (logged only); a free-run receiver ignores
// the values. shairport uses strncmp(cp, "progress: ", 10).
bool raop_parse_progress(const char *body, size_t len,
                         uint32_t *start, uint32_t *cur, uint32_t *end);
