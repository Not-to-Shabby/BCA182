// Pure-C RAOP mDNS TXT-record builder. No ESP-IDF deps, so host-unit-testable.
// The values are the AirPlay-1 capability advertisement from the design spec
// (§5a): ALAC, 44100/16/stereo, encryption none+RSA/AES, text metadata.
#pragma once

#include <stddef.h>

// Layout mirrors IDF's mdns_txt_item_t (key/value C strings) but stays const &
// ESP-IDF-free; mdns_service.c copies these into the IDF struct.
typedef struct {
    const char *key;
    const char *value;
} raop_txt_item_t;

// Number of TXT records produced (size caller-owned arrays with this).
#define RAOP_TXT_COUNT 10

// Fill `items` (caller-owned, >= RAOP_TXT_COUNT entries) with the RAOP TXT set.
// Returns the number written (== RAOP_TXT_COUNT when max_items is sufficient,
// else the number that fit).
size_t raop_txt_build(raop_txt_item_t *items, size_t max_items);
