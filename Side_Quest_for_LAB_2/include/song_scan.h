/**
 * @file song_scan.h
 * @brief Helpers for listing MIDI files found by scanning the SD card when
 *        songs.idx cannot be used: filename checks, filename parsing and a
 *        small fixed-size list of the files found.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#ifndef SONG_SCAN_H_
#define SONG_SCAN_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The board has about 25 KB of free RAM, so only the first few files found are
 * listed. A path takes about 20 bytes on a card made by prepare_karaoke_sd.py
 * and about 50 bytes with the original long file names. */
#define SONG_TABLE_MAX_SONGS    192
#define SONG_TABLE_POOL_BYTES   6144

typedef struct {
    uint16_t offset[SONG_TABLE_MAX_SONGS];
    char pool[SONG_TABLE_POOL_BYTES];
    uint16_t count;
    uint16_t used;
} song_table_t;

/**
 * @brief true for a regular MIDI file name (.mid or .midi, any case).
 *        Names starting with '.' (hidden files, "._" copies made by macOS) are rejected.
 */
bool song_name_is_midi(const char *name);

/**
 * @brief Split a file name into song code, artist and title.
 *
 * "027720 - Itchyworms - Beer.mid" gives code 27720, artist "Itchyworms",
 * title "Beer". A name that does not start with a code followed by " - " gives
 * code 0, no artist and the name without extension as the title. Results are
 * cut to fit the buffers.
 *
 * @param basename File name without any folder.
 */
void song_name_parse(const char *basename, uint32_t *code,
                     char *singer, size_t singer_size,
                     char *title, size_t title_size);

void song_table_clear(song_table_t *table);

/**
 * @brief Append a path, for example "midi/036/036527.mid".
 * @return false if the table or its string pool is full; nothing is added.
 */
bool song_table_add(song_table_t *table, const char *path);

/** @return the stored path, or NULL if @p index is out of range. */
const char *song_table_get(const song_table_t *table, uint16_t index);

#ifdef __cplusplus
}
#endif

#endif /* SONG_SCAN_H_ */
