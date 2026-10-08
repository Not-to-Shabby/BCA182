/**
 * @file karaoke_ui.h
 * @brief Retro ST7789 240x240 IPS Karaoke User Interface for RT-Spark.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#ifndef KARAOKE_UI_H_
#define KARAOKE_UI_H_

#include <stdint.h>
#include <stdbool.h>
#include "midi_karaoke_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_VIEW_BROWSER = 0,
    UI_VIEW_PLAYING,
    UI_VIEW_NUMBER_SELECT
} ui_view_mode_t;

typedef struct {
    uint32_t song_code;
    char title[40];
    char singer[28];
    char language[8];
    char filename[16];
} song_entry_t;

/**
 * @brief Initialize ST7789 display and clear screen with splash screen.
 */
void karaoke_ui_init(void);

/**
 * @brief Switch UI view between song catalog browser and active karaoke lyric player.
 */
void karaoke_ui_set_view(ui_view_mode_t mode);

/**
 * @brief Get current UI view mode.
 */
ui_view_mode_t karaoke_ui_get_view(void);

/**
 * @brief Set current metadata for the active karaoke track.
 */
void karaoke_ui_set_current_song(const song_entry_t *song);

/**
 * @brief Refresh active karaoke view (lyrics, progress bar, VU meters, BPM).
 *
 * @param status Pointer to current MIDI sequencer status.
 * @param peak_l Left channel audio peak (0 to 32767).
 * @param peak_r Right channel audio peak (0 to 32767).
 * @param active_voices Number of active FM polyphony voices.
 * @param volume Master volume percent (0-100).
 */
void karaoke_ui_update_player(const midi_player_status_t *status,
                              int16_t peak_l, int16_t peak_r,
                              uint8_t active_voices, uint8_t volume);

/**
 * @brief Render the song catalog browser.
 *
 * @param songs Array of visible songs on current page (up to 5 items).
 * @param count Number of valid songs in the array.
 * @param selected_index Index of the cursor item (0 to count-1).
 * @param page_num Current page number (1-indexed).
 * @param total_pages Total page count.
 */
void karaoke_ui_render_browser(const song_entry_t *songs, uint8_t count,
                               uint8_t selected_index, uint32_t page_num, uint32_t total_pages);

/**
 * @brief Render the direct song number select screen.
 *
 * @param digits Array of 5 digits (0-9).
 * @param cursor Current active digit position (0-4).
 * @param preview_title Title of matched song (or NULL / "[No match]").
 * @param preview_artist Artist of matched song (or NULL).
 * @param found Whether current digits match a valid song.
 */
void karaoke_ui_render_number_select(const uint8_t *digits, uint8_t cursor,
                                     const char *preview_title, const char *preview_artist,
                                     bool found);

#ifdef __cplusplus
}
#endif

#endif /* KARAOKE_UI_H_ */
