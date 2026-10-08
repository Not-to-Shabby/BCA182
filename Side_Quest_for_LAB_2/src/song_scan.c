/**
 * @file song_scan.c
 * @brief Helpers for listing MIDI files found by scanning the SD card.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include "song_scan.h"
#include <ctype.h>
#include <string.h>

#define NAME_SEPARATOR      " - "
#define NAME_SEPARATOR_LEN  3U
#define MAX_CODE_DIGITS     9U

static bool extension_is(const char *ext, const char *lower)
{
    while (*lower != '\0') {
        if (tolower((unsigned char)*ext) != *lower) {
            return false;
        }
        ext++;
        lower++;
    }
    return *ext == '\0';
}

static void copy_cut(char *dst, size_t dst_size, const char *src, size_t len)
{
    if (dst_size == 0) {
        return;
    }
    size_t n = (len < dst_size - 1U) ? len : dst_size - 1U;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

bool song_name_is_midi(const char *name)
{
    if (name == NULL || name[0] == '.') {
        return false;
    }
    const char *dot = strrchr(name, '.');
    if (dot == NULL) {
        return false;
    }
    return extension_is(dot + 1, "mid") || extension_is(dot + 1, "midi");
}

void song_name_parse(const char *basename, uint32_t *code,
                     char *singer, size_t singer_size,
                     char *title, size_t title_size)
{
    size_t stem_len = strlen(basename);
    const char *dot = strrchr(basename, '.');
    if (dot != NULL && dot != basename) {
        stem_len = (size_t)(dot - basename);
    }

    *code = 0;
    if (singer_size > 0) {
        singer[0] = '\0';
    }

    size_t digits = 0;
    uint32_t value = 0;
    while (digits < stem_len && digits < MAX_CODE_DIGITS &&
           basename[digits] >= '0' && basename[digits] <= '9') {
        value = value * 10U + (uint32_t)(basename[digits] - '0');
        digits++;
    }

    bool bare_code = (digits > 0) && (digits == stem_len);
    bool code_then_text = (digits > 0) && (digits + NAME_SEPARATOR_LEN <= stem_len) &&
                          (memcmp(basename + digits, NAME_SEPARATOR, NAME_SEPARATOR_LEN) == 0);

    if (code_then_text) {
        *code = value;
        const char *rest = basename + digits + NAME_SEPARATOR_LEN;
        size_t rest_len = stem_len - digits - NAME_SEPARATOR_LEN;

        size_t split = 0;
        while (split + NAME_SEPARATOR_LEN <= rest_len &&
               memcmp(rest + split, NAME_SEPARATOR, NAME_SEPARATOR_LEN) != 0) {
            split++;
        }
        if (split + NAME_SEPARATOR_LEN <= rest_len) {
            copy_cut(singer, singer_size, rest, split);
            copy_cut(title, title_size, rest + split + NAME_SEPARATOR_LEN,
                     rest_len - split - NAME_SEPARATOR_LEN);
        } else {
            copy_cut(title, title_size, rest, rest_len);
        }
    } else {
        if (bare_code) {
            *code = value;
        }
        copy_cut(title, title_size, basename, stem_len);
    }

    if (title_size > 0 && title[0] == '\0') {
        copy_cut(title, title_size, basename, stem_len);
    }
}

void song_table_clear(song_table_t *table)
{
    table->count = 0;
    table->used = 0;
}

bool song_table_add(song_table_t *table, const char *path)
{
    size_t need = strlen(path) + 1U;

    if (table->count >= SONG_TABLE_MAX_SONGS ||
        need > (size_t)(SONG_TABLE_POOL_BYTES - table->used)) {
        return false;
    }

    memcpy(&table->pool[table->used], path, need);
    table->offset[table->count] = table->used;
    table->count++;
    table->used = (uint16_t)(table->used + need);
    return true;
}

const char *song_table_get(const song_table_t *table, uint16_t index)
{
    if (index >= table->count) {
        return NULL;
    }
    return &table->pool[table->offset[index]];
}
