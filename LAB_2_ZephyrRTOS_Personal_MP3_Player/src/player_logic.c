/**
 * @file player_logic.c
 * @brief Implementation of pure decision logic for the Personal MP3 Player.
 *
 * Course: BCA182 Embedded Systems Programming
 * Laboratory Activity 2: Personal MP3 Player
 */

#include "player_logic.h"

/* Catalog of the 8 playable classical repertoire songs from song_def.h */
static const song_info_t SONG_CATALOG[8] = {
    {0, "Fur Elise -",        "Beethoven",      0.18f, 72},
    {1, "Canon In D - ",      "Pachelbel",      0.20f, 88},
    {2, "Minuet in G",        "major - Bach",   0.25f, 90},
    {3, "Turkish March - ",   "Mozart",         0.15f, 176},
    {4, "Nocturne in E ",     "flat - Chopin",  0.22f, 116},
    {5, "Waltz No. 2 - ",     "Shostakovich",   0.22f, 135},
    {6, "Nocturne in C ",     "sharp - Chopin", 0.25f, 64},
    {7, "Symphony No. 40 ",   "- Mozart",       0.15f, 168}
};

uint8_t decode_binary_song_index(bool b2, bool b3, bool b4)
{
    uint8_t index = 0;
    if (b2) {
        index |= (1U << 0); /* LSB */
    }
    if (b3) {
        index |= (1U << 1);
    }
    if (b4) {
        index |= (1U << 2); /* MSB */
    }
    return (index & 0x07U);
}

bool check_confirmation_timeout(uint32_t start_time_ms, uint32_t current_time_ms, uint32_t timeout_duration_ms)
{
    /* Handle potential 32-bit tick wrap-around gracefully */
    uint32_t elapsed = current_time_ms - start_time_ms;
    return (elapsed >= timeout_duration_ms);
}

player_state_t toggle_play_pause(player_state_t current_state)
{
    switch (current_state) {
        case PLAYER_STATE_PLAYING:
            return PLAYER_STATE_PAUSED;
        case PLAYER_STATE_PAUSED:
        case PLAYER_STATE_STOPPED:
            return PLAYER_STATE_PLAYING;
        case PLAYER_STATE_CONFIRMING:
        default:
            return current_state;
    }
}

uint8_t normalize_adc_volume(uint16_t raw_adc, uint16_t min_raw, uint16_t max_raw)
{
    if (raw_adc <= min_raw) {
        return 0;
    }
    if (raw_adc >= max_raw) {
        return 100;
    }
    uint32_t range = (uint32_t)(max_raw - min_raw);
    uint32_t offset = (uint32_t)(raw_adc - min_raw);
    uint32_t scaled = (offset * 100U + (range / 2U)) / range;
    if (scaled > 100U) {
        scaled = 100U;
    }
    return (uint8_t)scaled;
}

#include "sd_card_reader.h"

static song_info_t s_dynamic_song;

const song_info_t* get_song_info(uint8_t song_index)
{
    uint8_t sd_tracks = sd_card_get_track_count();
    if (song_index < sd_tracks) {
        const sd_track_t *t = sd_card_get_track(song_index);
        s_dynamic_song.id = song_index;
        s_dynamic_song.name1 = t->filename;
        s_dynamic_song.name2 = "[SD Card .WAV]";
        s_dynamic_song.tempo = 0.0f;
        s_dynamic_song.length = 0;
        return &s_dynamic_song;
    }
    song_index -= sd_tracks;
    if (song_index >= 8U) {
        song_index = 0U;
    }
    return &SONG_CATALOG[song_index];
}

const char* get_player_state_str(player_state_t state)
{
    switch (state) {
        case PLAYER_STATE_PLAYING:
            return "PLAYING";
        case PLAYER_STATE_PAUSED:
            return "PAUSED";
        case PLAYER_STATE_CONFIRMING:
            return "CONFIRMING";
        case PLAYER_STATE_STOPPED:
        default:
            return "STOPPED";
    }
}
