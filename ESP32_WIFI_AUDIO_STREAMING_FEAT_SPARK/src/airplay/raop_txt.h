#pragma once

#include <stddef.h>

typedef struct {
    const char *key;
    const char *value;
} raop_txt_item_t;

#define RAOP_TXT_COUNT 16

size_t raop_txt_build(raop_txt_item_t *items, size_t max_items);
