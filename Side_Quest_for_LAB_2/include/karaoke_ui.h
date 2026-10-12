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

#include "karaoke_mix.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_VIEW_BROWSER = 0,
    UI_VIEW_PLAYING,
    UI_VIEW_NUMBER_SELECT,
    UI_VIEW_SETTINGS,
    UI_VIEW_MIXER,
    UI_VIEW_SENSME
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
void karaoke_ui_set_current_song_index(uint32_t index);

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

/**
 * @brief Render the audio settings mixer screen.
 *
 * @param cursor Active selected row: 0=Master Vol, 1=Instrument Gain, 2=Drums Gain, 3=Melody Gain, 4=Sound Card.
 * @param vol Current master volume (0-100%).
 * @param inst Current instrument gain (20-200%).
 * @param drum Current drum gain (20-200%).
 * @param melody Current melody gain (20-250%).
 * @param bank Current sound card bank (0=Yamaha OPL4, 1=Roland SC-55).
 */
void karaoke_ui_render_settings(uint8_t cursor, uint8_t vol, uint8_t inst, uint8_t drum, uint8_t melody, uint8_t bank);

/**
 * @brief Draw the mixer: a scrolling list of the rows in @p view with the selected row kept in
 *        view. Only rows whose content changed are redrawn.
 *
 * @param cursor Position of the selected row in @p view.
 * @param view The rows to show (see karaoke_mix_build_view()).
 * @param mix Values to show.
 * @param programs General MIDI program of each channel (names the instrument, or the drum kit).
 * @param levels Current loudness of each channel (0-255), shown as an activity dot.
 * @param drum_mask Bit n set = channel n is a drum part.
 * @param song_mix True if this song has a mix of its own, false if it plays the default.
 */
void karaoke_ui_render_mixer(uint8_t cursor, const mix_view_t *view, const karaoke_mix_t *mix,
                             const uint8_t programs[16], const uint8_t levels[16],
                             uint16_t drum_mask, bool song_mix);

/**
 * @brief Render the SensMe mood channels menu and 2D circular mood radar scope.
 *
 * @param cursor Active selected channel in list (0..12).
 * @param active_filter Currently active mood channel filter (0 = All).
 * @param current_song_index Catalog index of the currently playing song.
 */
void karaoke_ui_render_sensme(uint8_t cursor, uint8_t active_filter, uint32_t current_song_index);

#ifdef __cplusplus
}
#endif

#endif /* KARAOKE_UI_H_ */
