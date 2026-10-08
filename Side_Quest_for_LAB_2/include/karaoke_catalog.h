/**
 * @file karaoke_catalog.h
 * @brief High-performance song database catalog supporting both MicroSD
 *        binary index (songs.idx) and onboard ROM fallback tracks.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#ifndef KARAOKE_CATALOG_H_
#define KARAOKE_CATALOG_H_

#include <stdint.h>
#include <stdbool.h>
#include "karaoke_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Where the song list comes from.
 */
typedef enum {
    KARAOKE_SOURCE_ROM = 0,     /**< built-in songs (no usable SD card, or no MIDI files on it) */
    KARAOKE_SOURCE_INDEX,       /**< songs.idx on the SD card */
    KARAOKE_SOURCE_SCAN         /**< MIDI files found by scanning the SD card */
} karaoke_catalog_source_t;

/**
 * @brief Initialize the song catalog.
 *
 * Uses songs.idx when it is present and valid (a truncated index keeps the
 * records that are present). Otherwise, with a mounted card, scans SD:/midi
 * (one folder level below it) and the card root for .mid files; only the first
 * few files found are listed. With no mounted card, or no MIDI files, the
 * built-in songs are used. Call after sd_card_reader_init().
 */
void karaoke_catalog_init(void);

/**
 * @brief Which of the three song sources is in use.
 */
karaoke_catalog_source_t karaoke_catalog_get_source(void);

/**
 * @brief Check if the catalog is reading songs from the SD card (index or scan).
 */
bool karaoke_catalog_is_sd_active(void);

/**
 * @brief Get total number of songs in the catalog.
 */
uint32_t karaoke_catalog_get_total_songs(void);

/**
 * @brief Find a song by its numeric song code.
 *
 * Uses logarithmic binary search on songs.idx (<= 16 seeks across 48,000 songs),
 * or linear scan in ROM/scan fallback modes.
 *
 * @param target_code Numeric song code (e.g. 27720 for Beer).
 * @param out_index Pointer to output 0-based catalog index.
 * @return true if song code exists in catalog.
 */
bool karaoke_catalog_find_by_code(uint32_t target_code, uint32_t *out_index);

/**
 * @brief Retrieve metadata for a song by catalog index.
 *
 * @param index 0-based index.
 * @param out_song Pointer to output struct.
 * @return true if successful.
 */
bool karaoke_catalog_get_song(uint32_t index, song_entry_t *out_song);

/**
 * @brief Load MIDI data for the specified song index.
 *
 * @param index 0-based index.
 * @param out_data Pointer to output data pointer (ROM or allocated buffer).
 * @param out_length Pointer to output byte length.
 * @return true if data was successfully loaded.
 */
bool karaoke_catalog_load_midi_data(uint32_t index, const uint8_t **out_data, uint32_t *out_length);

#ifdef __cplusplus
}
#endif

#endif /* KARAOKE_CATALOG_H_ */
