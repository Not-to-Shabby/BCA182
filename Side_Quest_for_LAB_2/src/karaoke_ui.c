/**
 * @file karaoke_ui.c
 * @brief Flicker-Free Retro ST7789 240x240 IPS Karaoke User Interface
 *        with differential updates and correct FSMC rectangular bounds.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include "karaoke_ui.h"
#include "lcd_st7789.h"
#include "yamaha_fm_synth.h"
#include "sensme.h"
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

K_MUTEX_DEFINE(s_ui_lcd_mutex);

static ui_view_mode_t s_view_mode = UI_VIEW_PLAYING;
static song_entry_t s_current_song;
static uint32_t s_song_id;
static uint32_t s_current_catalog_index = 0;

/* Differential rendering trackers to eliminate flicker */
static char s_last_cur_lyric[KARAOKE_MAX_LINE_CHARS + 1] = "";
static char s_last_prev_lyric[KARAOKE_MAX_LINE_CHARS + 1] = "";
static char s_last_up_lyric[KARAOKE_MAX_LINE_CHARS + 1] = "";
static uint32_t s_last_sec = 0xFFFFFFFFU;
static uint8_t s_last_vol = 0xFFU;
static uint8_t s_last_voices = 0xFFU;
static int s_last_vu_l = -1;
static int s_last_vu_r = -1;
static bool s_frame_initialized = false;
static uint32_t s_title_song_id = 0xFFFFFFFFU;     /* song whose artist and title are on the LCD */
static bool s_mixer_redraw = true;      /* the whole mixer screen must be drawn again */

static void pad_string(char *dst, const char *src, size_t max_len)
{
    size_t slen = strlen(src);
    if (slen >= max_len) {
        memcpy(dst, src, max_len - 1);
        dst[max_len - 1] = '\0';
    } else {
        memcpy(dst, src, slen);
        memset(dst + slen, ' ', max_len - slen - 1);
        dst[max_len - 1] = '\0';
    }
}

void karaoke_ui_init(void)
{
    k_mutex_lock(&s_ui_lcd_mutex, K_FOREVER);
    lcd_st7789_init();
    lcd_clear(LCD_COLOR_BLACK);

    /* Splash Screen Banner: Coordinates (x1, y1, x2, y2) */
    lcd_fill_rect(0, 0, 239, 40, LCD_COLOR_NAVY);
    lcd_show_string(24, 12, "RT-SPARK KARAOKE", LCD_COLOR_YELLOW, LCD_COLOR_NAVY);
    lcd_show_string(32, 70, "Yamaha FM Synth", LCD_COLOR_CYAN, LCD_COLOR_BLACK);
    lcd_show_string(40, 95, "ES8388 44.1kHz", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    lcd_show_string(20, 130, "48k SongHub Library", LCD_COLOR_GREEN, LCD_COLOR_BLACK);
    lcd_show_string(36, 175, "Loading Songs...", LCD_COLOR_LIGHTGREY, LCD_COLOR_BLACK);
    lcd_draw_rect(10, 50, 229, 210, LCD_COLOR_DARKCYAN);
    k_mutex_unlock(&s_ui_lcd_mutex);
}

static void render_static_player_frame(void)
{
    /* 0. Rows 0..144 are repainted below; the rest of the screen may still hold the previous
     * view (the browser and number-entry footers sit down there), so wipe it first. */
    lcd_fill_rect(0, 145, 239, 239, LCD_COLOR_BLACK);

    /* 1. Header Bar background */
    lcd_fill_rect(0, 0, 239, 22, LCD_COLOR_NAVY);

    /* 2. Song area background */
    lcd_fill_rect(0, 23, 239, 68, LCD_COLOR_BLACK);
    lcd_draw_line(0, 69, 239, 69, LCD_COLOR_DARKGREY);

    /* 3. Lyrics Area clear */
    lcd_fill_rect(0, 70, 239, 144, LCD_COLOR_BLACK);

    /* 4. Progress bar box outline */
    lcd_draw_rect(8, 168, 231, 178, LCD_COLOR_DARKGREY);

    /* 5. Static VU labels */
    lcd_show_string(8, 192, "L:", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    lcd_fill_rect(26, 194, 91, 202, LCD_COLOR_DARKGREY);

    lcd_show_string(120, 192, "R:", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    lcd_fill_rect(138, 194, 203, 202, LCD_COLOR_DARKGREY);

    s_frame_initialized = true;
    s_title_song_id = 0xFFFFFFFFU;      /* the wipe above removed the artist and title: draw them again */
    s_last_vol = 0xFFU;
    s_last_sec = 0xFFFFFFFFU;
    s_last_voices = 0xFFU;
    s_last_vu_l = -1;
    s_last_vu_r = -1;
    memset(s_last_cur_lyric, 0, sizeof(s_last_cur_lyric));
    memset(s_last_prev_lyric, 0, sizeof(s_last_prev_lyric));
    memset(s_last_up_lyric, 0, sizeof(s_last_up_lyric));
}

void karaoke_ui_set_view(ui_view_mode_t mode)
{
    k_mutex_lock(&s_ui_lcd_mutex, K_FOREVER);
    s_view_mode = mode;
    if (mode == UI_VIEW_PLAYING) {
        render_static_player_frame();
    } else {
        lcd_clear(LCD_COLOR_BLACK);
    }
    memset(s_last_cur_lyric, 0, sizeof(s_last_cur_lyric));
    memset(s_last_prev_lyric, 0, sizeof(s_last_prev_lyric));
    memset(s_last_up_lyric, 0, sizeof(s_last_up_lyric));
    s_mixer_redraw = true;
    k_mutex_unlock(&s_ui_lcd_mutex);
}

ui_view_mode_t karaoke_ui_get_view(void)
{
    return s_view_mode;
}

void karaoke_ui_set_current_song_index(uint32_t index)
{
    s_current_catalog_index = index;
}

void karaoke_ui_set_current_song(const song_entry_t *song)
{
    k_mutex_lock(&s_ui_lcd_mutex, K_FOREVER);
    if (song) {
        s_current_song = *song;
    } else {
        memset(&s_current_song, 0, sizeof(s_current_song));
    }
    s_song_id++;
    memset(s_last_cur_lyric, 0, sizeof(s_last_cur_lyric));
    memset(s_last_prev_lyric, 0, sizeof(s_last_prev_lyric));
    memset(s_last_up_lyric, 0, sizeof(s_last_up_lyric));

    /* Immediately paint the player screen with song title, artist, and [Loading...] to eliminate black screen delay */
    if (s_view_mode == UI_VIEW_PLAYING && song != NULL) {
        render_static_player_frame();

        /* Header bar */
        char line_buf[32];
        lcd_fill_rect(0, 0, 239, 22, LCD_COLOR_NAVY);
        snprintf(line_buf, sizeof(line_buf), "[>] #%05u %s", (unsigned)song->song_code, song->language);
        lcd_show_string(4, 3, line_buf, LCD_COLOR_WHITE, LCD_COLOR_NAVY);
        snprintf(line_buf, sizeof(line_buf), "V:%u%%", s_last_vol != 0xFFU ? s_last_vol : 80U);
        lcd_show_string(185, 3, line_buf, LCD_COLOR_YELLOW, LCD_COLOR_NAVY);

        /* Artist & Title */
        char artist_buf[28];
        pad_string(artist_buf, song->singer, 28);
        lcd_show_string(8, 26, artist_buf, LCD_COLOR_CYAN, LCD_COLOR_BLACK);

        char title_buf[28];
        pad_string(title_buf, song->title, 28);
        lcd_show_string(8, 46, title_buf, LCD_COLOR_YELLOW, LCD_COLOR_BLACK);
        lcd_draw_line(0, 69, 239, 69, LCD_COLOR_DARKGREY);

        /* Loading indicator in the lyric highlight box */
        lcd_fill_rect(4, 96, 235, 120, LCD_COLOR_DARKCYAN);
        lcd_draw_rect(4, 96, 235, 120, LCD_COLOR_YELLOW);
        lcd_show_string(10, 100, "    [Loading Song...]    ", LCD_COLOR_YELLOW, LCD_COLOR_DARKCYAN);
        s_frame_initialized = true;
        s_title_song_id = s_song_id;
    } else {
        s_frame_initialized = false;
    }
    k_mutex_unlock(&s_ui_lcd_mutex);
}

void karaoke_ui_update_player(const midi_player_status_t *status,
                              int16_t peak_l, int16_t peak_r,
                              uint8_t active_voices, uint8_t volume)
{
    if (s_view_mode != UI_VIEW_PLAYING || !status) return;

    if (k_mutex_lock(&s_ui_lcd_mutex, K_MSEC(10)) != 0) {
        return;
    }

    /* Initialize static labels once */
    if (!s_frame_initialized) {
        render_static_player_frame();
    }

    char line_buf[32];

    /* 1. Header Bar: Only draw when volume changes */
    if (s_last_vol != volume) {
        s_last_vol = volume;
        lcd_fill_rect(0, 0, 239, 22, LCD_COLOR_NAVY);

        snprintf(line_buf, sizeof(line_buf), "[>] #%05u %s", (unsigned)s_current_song.song_code, s_current_song.language);
        lcd_show_string(4, 3, line_buf, LCD_COLOR_WHITE, LCD_COLOR_NAVY);

        snprintf(line_buf, sizeof(line_buf), "V:%u%%", volume);
        lcd_show_string(185, 3, line_buf, LCD_COLOR_YELLOW, LCD_COLOR_NAVY);
    }

    /* 2. Artist & Song Title (redrawn per song: scanned files can all report code 0) */
    if (s_title_song_id != s_song_id) {
        s_title_song_id = s_song_id;

        lcd_fill_rect(0, 24, 239, 68, LCD_COLOR_BLACK);

        char artist_buf[28];
        pad_string(artist_buf, s_current_song.singer, 28);
        lcd_show_string(8, 26, artist_buf, LCD_COLOR_CYAN, LCD_COLOR_BLACK);

        char title_buf[28];
        pad_string(title_buf, s_current_song.title, 28);
        lcd_show_string(8, 46, title_buf, LCD_COLOR_YELLOW, LCD_COLOR_BLACK);

        lcd_draw_line(0, 69, 239, 69, LCD_COLOR_DARKGREY);
    }

    /* 3. Synchronized Lyrics: Three-line scrolling display */
    if (strcmp(s_last_prev_lyric, status->previous_lyric_line) != 0) {
        snprintf(s_last_prev_lyric, sizeof(s_last_prev_lyric), "%s", status->previous_lyric_line);
        char padded[30];
        pad_string(padded, status->previous_lyric_line, 28);
        lcd_show_string(8, 74, padded, LCD_COLOR_GRAY, LCD_COLOR_BLACK);
    }

    if (strcmp(s_last_cur_lyric, status->current_lyric_line) != 0) {
        snprintf(s_last_cur_lyric, sizeof(s_last_cur_lyric), "%s", status->current_lyric_line);

        /* Highlight box for current active lyric line */
        lcd_fill_rect(4, 96, 235, 120, LCD_COLOR_DARKCYAN);
        lcd_draw_rect(4, 96, 235, 120, LCD_COLOR_YELLOW);

        char padded[30];
        pad_string(padded, status->current_lyric_line, 26);
        lcd_show_string(10, 100, padded, LCD_COLOR_WHITE, LCD_COLOR_DARKCYAN);
    }

    if (strcmp(s_last_up_lyric, status->upcoming_lyric_line) != 0) {
        snprintf(s_last_up_lyric, sizeof(s_last_up_lyric), "%s", status->upcoming_lyric_line);
        char padded[30];
        char line_formatted[48];
        if (status->upcoming_lyric_line[0] != '\0' && status->upcoming_lyric_line[0] != ' ') {
            snprintf(line_formatted, sizeof(line_formatted), ">> %s", status->upcoming_lyric_line);
        } else {
            snprintf(line_formatted, sizeof(line_formatted), "%s", status->upcoming_lyric_line);
        }
        pad_string(padded, line_formatted, 28);
        lcd_show_string(8, 126, padded, LCD_COLOR_CYAN, LCD_COLOR_BLACK);
    }

    /* 4. Playback Time & Progress Bar: Update once per second */
    uint32_t sec = status->elapsed_ms / 1000;
    if (sec != s_last_sec) {
        s_last_sec = sec;
        uint32_t min = sec / 60;
        uint32_t s = sec % 60;

        snprintf(line_buf, sizeof(line_buf), "Time:%02u:%02u BPM:%u", (unsigned)min, (unsigned)s, status->bpm);
        char padded_time[22];
        pad_string(padded_time, line_buf, 18);
        lcd_show_string(8, 148, padded_time, LCD_COLOR_LIGHTGREY, LCD_COLOR_BLACK);

        sensme_data_t sd;
        if (sensme_get_song_data(s_current_catalog_index, &sd)) {
            char m_buf[14];
            snprintf(m_buf, sizeof(m_buf), "[%s]", sensme_channel_name(sd.channel));
            char padded_mood[12];
            pad_string(padded_mood, m_buf, 11);
            lcd_show_string(152, 148, padded_mood, sensme_channel_color(sd.channel), LCD_COLOR_BLACK);
        } else {
            lcd_show_string(152, 148, "           ", LCD_COLOR_BLACK, LCD_COLOR_BLACK);
        }

        /* Progress Bar: width = 220 pixels (x=9 to x=229) */
        uint16_t fill_w = 0;
        if (status->duration_ms > 0) {
            fill_w = (uint16_t)(((uint64_t)status->elapsed_ms * 220) / status->duration_ms);
            if (fill_w > 220) fill_w = 220;
        } else {
            fill_w = (sec * 3) % 220;
        }

        if (fill_w > 0) {
            lcd_fill_rect(9, 170, 9 + fill_w, 176, LCD_COLOR_GREEN);
        }
        if (fill_w < 220) {
            lcd_fill_rect(9 + fill_w + 1, 170, 229, 176, LCD_COLOR_BLACK);
        }
    }

    /* 5. Stereo Audio VU Meters: Differential update (x1, y1, x2, y2) */
    int vu_l = (peak_l * 65) / 32767;
    if (vu_l > 65) vu_l = 65;
    if (vu_l < 0)  vu_l = 0;

    if (vu_l != s_last_vu_l) {
        s_last_vu_l = vu_l;
        uint16_t color = (vu_l > 55) ? LCD_COLOR_RED : ((vu_l > 40) ? LCD_COLOR_YELLOW : LCD_COLOR_GREEN);
        if (vu_l > 0) {
            lcd_fill_rect(26, 194, 26 + vu_l, 202, color);
        }
        if (vu_l < 65) {
            lcd_fill_rect(26 + vu_l + 1, 194, 91, 202, LCD_COLOR_DARKGREY);
        }
    }

    int vu_r = (peak_r * 65) / 32767;
    if (vu_r > 65) vu_r = 65;
    if (vu_r < 0)  vu_r = 0;

    if (vu_r != s_last_vu_r) {
        s_last_vu_r = vu_r;
        uint16_t color = (vu_r > 55) ? LCD_COLOR_RED : ((vu_r > 40) ? LCD_COLOR_YELLOW : LCD_COLOR_GREEN);
        if (vu_r > 0) {
            lcd_fill_rect(138, 194, 138 + vu_r, 202, color);
        }
        if (vu_r < 65) {
            lcd_fill_rect(138 + vu_r + 1, 194, 203, 202, LCD_COLOR_DARKGREY);
        }
    }

    /* 6. Active Voice Polyphony & Active Sound Card HUD: Differential update */
    if (active_voices != s_last_voices) {
        s_last_voices = active_voices;
        synth_sound_bank_t b = yamaha_fm_get_sound_bank();
        const char *b_tag = (b == SYNTH_BANK_PURE_FM) ? "FM  " :
                            ((b == SYNTH_BANK_ROLAND_SC55) ? "SC55" : "OPL4");
        sensme_data_t sd_hud;
        if (sensme_get_song_data(s_current_catalog_index, &sd_hud)) {
            snprintf(line_buf, sizeof(line_buf), "Vox:%2u/70 [%s]", active_voices, b_tag);
            lcd_show_string(8, 218, line_buf, LCD_COLOR_GRAY, LCD_COLOR_BLACK);
            char sm_coords[14];
            snprintf(sm_coords, sizeof(sm_coords), "V:%3u E:%3u", sd_hud.valence, sd_hud.energy);
            lcd_show_string(144, 218, sm_coords, LCD_COLOR_LIGHTGREY, LCD_COLOR_BLACK);
        } else {
            snprintf(line_buf, sizeof(line_buf), "Vox:%2u/70  [%s] ", active_voices, b_tag);
            lcd_show_string(8, 218, line_buf, LCD_COLOR_GRAY, LCD_COLOR_BLACK);
        }
    }

    k_mutex_unlock(&s_ui_lcd_mutex);
}

void karaoke_ui_render_browser(const song_entry_t *songs, uint8_t count,
                               uint8_t selected_index, uint32_t page_num, uint32_t total_pages)
{
    if (s_view_mode != UI_VIEW_BROWSER) return;

    if (k_mutex_lock(&s_ui_lcd_mutex, K_MSEC(50)) != 0) {
        return;
    }

    /* Header (x1=0, y1=0, x2=239, y2=24) */
    lcd_fill_rect(0, 0, 239, 24, LCD_COLOR_NAVY);
    char buf[32];
    snprintf(buf, sizeof(buf), "SELECT SONG (%u/%u)", (unsigned)page_num, (unsigned)total_pages);
    lcd_show_string(16, 4, buf, LCD_COLOR_YELLOW, LCD_COLOR_NAVY);

    /* Render song items (up to 5) */
    uint16_t y = 30;
    for (uint8_t i = 0; i < 5; i++) {
        uint16_t bg = (i == selected_index) ? LCD_COLOR_DARKGREEN : LCD_COLOR_BLACK;
        uint16_t fg_title = (i == selected_index) ? LCD_COLOR_WHITE : LCD_COLOR_LIGHTGREY;
        uint16_t fg_artist = (i == selected_index) ? LCD_COLOR_YELLOW : LCD_COLOR_CYAN;

        lcd_fill_rect(0, y, 239, y + 36, bg);

        if (i < count) {
            snprintf(buf, sizeof(buf), "%u. %s", (unsigned)songs[i].song_code, songs[i].title);
            buf[26] = '\0';
            lcd_show_string(6, y + 2, buf, fg_title, bg);

            snprintf(buf, sizeof(buf), "   %s (%s)", songs[i].singer, songs[i].language);
            buf[26] = '\0';
            lcd_show_string(6, y + 18, buf, fg_artist, bg);
        } else {
            lcd_show_string(6, y + 8, "--", LCD_COLOR_DARKGREY, bg);
        }

        lcd_draw_line(0, y + 36, 239, y + 36, LCD_COLOR_DARKGREY);
        y += 36;
    }

    /* Footer Navigation Guide (x1=0, y1=220, x2=239, y2=239) */
    lcd_fill_rect(0, 220, 239, 239, LCD_COLOR_DARKGREY);
    lcd_show_string(8, 222, "UP/DN:Sel OK:Play HOLD:Back", LCD_COLOR_WHITE, LCD_COLOR_DARKGREY);

    k_mutex_unlock(&s_ui_lcd_mutex);
}

void karaoke_ui_render_number_select(const uint8_t *digits, uint8_t cursor,
                                     const char *preview_title, const char *preview_artist,
                                     bool found)
{
    if (s_view_mode != UI_VIEW_NUMBER_SELECT || !digits) return;

    if (k_mutex_lock(&s_ui_lcd_mutex, K_MSEC(50)) != 0) {
        return;
    }

    /* 1. Header (x1=0, y1=0, x2=239, y2=24) */
    lcd_fill_rect(0, 0, 239, 24, LCD_COLOR_NAVY);
    lcd_show_string(24, 4, "=== NUMBER SELECT ===", LCD_COLOR_YELLOW, LCD_COLOR_NAVY);

    /* 2. Instructions (y: 28 to 60) */
    lcd_fill_rect(0, 26, 239, 64, LCD_COLOR_BLACK);
    lcd_show_string(14, 28, "LEFT/RIGHT: Select digit", LCD_COLOR_LIGHTGREY, LCD_COLOR_BLACK);
    lcd_show_string(14, 46, "UP/DOWN:    Change 0-9", LCD_COLOR_LIGHTGREY, LCD_COLOR_BLACK);

    /* 3. Five Digit Boxes (y: 68 to 116) */
    /* Digit box width: 36, gap: 8. Total = 5 * 36 + 4 * 8 = 180 + 32 = 212. Left margin = 14 */
    for (uint8_t i = 0; i < 5; i++) {
        uint16_t bx1 = 14 + (uint16_t)i * 44;
        uint16_t bx2 = bx1 + 36;
        uint16_t by1 = 68;
        uint16_t by2 = 114;

        bool active = (i == cursor);
        uint16_t box_bg = active ? LCD_COLOR_DARKCYAN : LCD_COLOR_DARKGREY;
        uint16_t border = active ? LCD_COLOR_YELLOW : LCD_COLOR_GRAY;
        uint16_t fg     = active ? LCD_COLOR_YELLOW : LCD_COLOR_WHITE;

        lcd_fill_rect(bx1, by1, bx2, by2, box_bg);
        lcd_draw_rect(bx1, by1, bx2, by2, border);

        /* Draw character in center of box */
        lcd_show_char(bx1 + 14, by1 + 14, '0' + (digits[i] % 10), fg, box_bg);

        /* Active underline cursor */
        if (active) {
            lcd_fill_rect(bx1 + 4, by2 - 4, bx2 - 4, by2 - 2, LCD_COLOR_YELLOW);
        }
    }

    /* 4. Live Song Match Preview Box (y: 122 to 184) */
    lcd_fill_rect(8, 122, 231, 184, LCD_COLOR_BLACK);
    lcd_draw_rect(8, 122, 231, 184, found ? LCD_COLOR_GREEN : LCD_COLOR_DARKGREY);

    if (found && preview_title) {
        char buf[30];
        pad_string(buf, preview_title, 26);
        lcd_show_string(12, 128, buf, LCD_COLOR_GREEN, LCD_COLOR_BLACK);

        if (preview_artist) {
            pad_string(buf, preview_artist, 26);
            lcd_show_string(12, 146, buf, LCD_COLOR_CYAN, LCD_COLOR_BLACK);
        }
        lcd_show_string(12, 166, "[Ready - hold UP/DN]", LCD_COLOR_YELLOW, LCD_COLOR_BLACK);
    } else {
        lcd_show_string(40, 138, "No Song Matching", LCD_COLOR_RED, LCD_COLOR_BLACK);
        lcd_show_string(24, 158, "Enter 5-digit number", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
    }

    /* 5. Footer (y: 194 to 239) */
    lcd_fill_rect(0, 194, 239, 239, LCD_COLOR_DARKGREY);
    lcd_show_string(8, 200, "HOLD UP or DOWN: Play", LCD_COLOR_WHITE, LCD_COLOR_DARKGREY);
    lcd_show_string(8, 218, "UP+DN together: Cancel", LCD_COLOR_YELLOW, LCD_COLOR_DARKGREY);

    k_mutex_unlock(&s_ui_lcd_mutex);
}

void karaoke_ui_render_settings(uint8_t cursor, uint8_t vol, uint8_t inst, uint8_t drum, uint8_t melody, uint8_t bank)
{
    if (s_view_mode != UI_VIEW_SETTINGS) return;

    if (k_mutex_lock(&s_ui_lcd_mutex, K_MSEC(50)) != 0) {
        return;
    }

    /* 1. Header */
    lcd_fill_rect(0, 0, 239, 22, LCD_COLOR_NAVY);
    lcd_show_string(32, 3, "=== AUDIO SETTINGS ===", LCD_COLOR_YELLOW, LCD_COLOR_NAVY);

    /* 2. Instructions */
    lcd_fill_rect(0, 23, 239, 34, LCD_COLOR_BLACK);
    lcd_show_string(14, 23, "UP/DN: Select  L/R: Adjust", LCD_COLOR_LIGHTGREY, LCD_COLOR_BLACK);

    /* 3. Setting rows (y: 35 to 202), 21 px apart */
    static const char *labels[] = {
        "Master Volume",
        "Instrument Vol",
        "Drums / Rhythm",
        "Melody / Lead"
    };
    uint8_t vals[] = { vol, inst, drum, melody };
    static const uint8_t maxs[] = { 100, 200, 200, 250 };

    uint16_t y = 35;
    for (uint8_t i = 0; i < 4; i++) {
        bool sel = (i == cursor);
        uint16_t bg = sel ? LCD_COLOR_DARKCYAN : LCD_COLOR_DARKGREY;
        uint16_t fg = sel ? LCD_COLOR_YELLOW : LCD_COLOR_WHITE;

        lcd_fill_rect(8, y, 231, y + 19, bg);
        lcd_draw_rect(8, y, 231, y + 19, sel ? LCD_COLOR_YELLOW : LCD_COLOR_GRAY);

        char buf[32];
        snprintf(buf, sizeof(buf), "%s: %u%%", labels[i], vals[i]);
        lcd_show_string(14, y, buf, fg, bg);

        /* Gauge bar */
        uint16_t gw = (uint16_t)(((uint32_t)vals[i] * 180U) / maxs[i]);
        if (gw > 180) gw = 180;
        lcd_draw_rect(14, y + 15, 196, y + 18, LCD_COLOR_BLACK);
        if (gw > 0) {
            lcd_fill_rect(15, y + 16, 15 + gw, y + 17, sel ? LCD_COLOR_GREEN : LCD_COLOR_CYAN);
        }
        if (gw < 180) {
            lcd_fill_rect(15 + gw + 1, y + 16, 195, y + 17, LCD_COLOR_BLACK);
        }

        y += 21;
    }

    /* 4. Rows 4..7: Sound Bank, SensMe, Mixer, Done */
    for (uint8_t r = 4; r < 8; r++) {
        bool sel = (cursor == r);
        uint16_t bg = sel ? LCD_COLOR_DARKCYAN : LCD_COLOR_DARKGREY;
        uint16_t fg = sel ? LCD_COLOR_YELLOW : LCD_COLOR_WHITE;
        char buf[36];

        lcd_fill_rect(8, y, 231, y + 19, bg);
        lcd_draw_rect(8, y, 231, y + 19, sel ? LCD_COLOR_YELLOW : LCD_COLOR_GRAY);
        if (r == 4) {
            const char *bname = (bank == 2) ? "Pure FM OPL3" :
                                ((bank == 1) ? "Roland SC-55" : "Yamaha OPL4");
            snprintf(buf, sizeof(buf), "Sound Card: <%s>", bname);
        } else if (r == 5) {
            uint8_t ach = sensme_get_active_channel();
            snprintf(buf, sizeof(buf), "SensMe Mood: <%s> >", sensme_channel_name(ach));
            if (!sel) fg = sensme_channel_color(ach);
        } else if (r == 6) {
            snprintf(buf, sizeof(buf), "Mixer: sections/channels >");
        } else {
            snprintf(buf, sizeof(buf), "Done  (save & return) >");
            fg = sel ? LCD_COLOR_YELLOW : LCD_COLOR_GREEN;
        }
        lcd_show_string(14, y + 2, buf, fg, bg);
        y += 21;
    }

    /* 5. Footer */
    const char *hint = "UP+DN together: Save & Exit";
    if (cursor == 5) hint = "RIGHT: SensMe Mood Radar";
    if (cursor == 6) hint = "RIGHT: Open the mixer";
    if (cursor == 7) hint = "RIGHT: Save & return";
    lcd_fill_rect(0, 206, 239, 239, LCD_COLOR_DARKGREY);
    lcd_show_string(8, 214, hint, LCD_COLOR_WHITE, LCD_COLOR_DARKGREY);

    k_mutex_unlock(&s_ui_lcd_mutex);
}

/* -------------------------------------------------------------------------- */
/* Mixer screen                                                               */
/* -------------------------------------------------------------------------- */
#define MIX_VISIBLE_ROWS    8U
#define MIX_ROW_H           21U
#define MIX_LIST_Y          40U
#define MIX_EMPTY_HASH      1U

static uint32_t s_mix_row_hash[MIX_VISIBLE_ROWS];
static uint8_t s_mix_last_top = 0xFFU;
static uint8_t s_mix_last_foot = 0xFFU;
static bool s_mix_last_song = false;

static uint32_t mix_hash(uint32_t h, uint32_t v)
{
    return (h ^ v) * 16777619U;
}

void karaoke_ui_render_mixer(uint8_t cursor, const mix_view_t *view, const karaoke_mix_t *mix,
                             const uint8_t programs[16], const uint8_t levels[16],
                             uint16_t drum_mask, bool song_mix)
{
    if (s_view_mode != UI_VIEW_MIXER || view->count == 0U) return;
    if (cursor >= view->count) cursor = (uint8_t)(view->count - 1U);

    if (k_mutex_lock(&s_ui_lcd_mutex, K_MSEC(50)) != 0) {
        return;
    }

    uint8_t top = 0;
    if (cursor >= 4U) top = (uint8_t)(cursor - 3U);
    if (view->count <= MIX_VISIBLE_ROWS) {
        top = 0;
    } else if (top > view->count - MIX_VISIBLE_ROWS) {
        top = (uint8_t)(view->count - MIX_VISIBLE_ROWS);
    }

    if (s_mixer_redraw) {
        s_mix_last_top = 0xFFU;
        s_mix_last_foot = 0xFFU;
        memset(s_mix_row_hash, 0, sizeof(s_mix_row_hash));
        lcd_fill_rect(0, 0, 239, 239, LCD_COLOR_BLACK);
        lcd_fill_rect(0, 26, 239, 38, LCD_COLOR_BLACK);
        lcd_show_string(14, 24, "UP/DN: Row  L/R: Adjust", LCD_COLOR_LIGHTGREY, LCD_COLOR_BLACK);
        s_mix_last_song = !song_mix;
        s_mixer_redraw = false;
    }
    if (s_mix_last_song != song_mix) {
        lcd_fill_rect(0, 0, 239, 22, LCD_COLOR_NAVY);
        lcd_show_string(8, 3, song_mix ? "=== MIXER (this song) ===" : "=== MIXER (default) ===",
                        LCD_COLOR_YELLOW, LCD_COLOR_NAVY);
        s_mix_last_song = song_mix;
    }
    if (s_mix_last_top != top) {
        memset(s_mix_row_hash, 0, sizeof(s_mix_row_hash));      /* scrolled: every row shows another item */
        s_mix_last_top = top;
    }

    for (uint8_t slot = 0; slot < MIX_VISIBLE_ROWS; slot++) {
        uint8_t pos = (uint8_t)(top + slot);

        if (pos >= view->count) {                       /* a short list: clear what the longer one left */
            if (s_mix_row_hash[slot] != MIX_EMPTY_HASH) {
                uint16_t ey = (uint16_t)(MIX_LIST_Y + slot * MIX_ROW_H);

                lcd_fill_rect(0, ey, 239, (uint16_t)(ey + MIX_ROW_H - 1U), LCD_COLOR_BLACK);
                s_mix_row_hash[slot] = MIX_EMPTY_HASH;
            }
            continue;
        }
        uint8_t item = view->item[pos];
        mix_item_kind_t kind = karaoke_mix_item_kind(item);
        bool sel = (pos == cursor);
        int value = karaoke_mix_item_value(mix, item);
        int state = 0;
        uint8_t ch = 0;
        uint8_t lvl = 0;
        bool drum = false;

        if (kind == MIX_KIND_CHANNEL) {
            ch = (uint8_t)(item - MIX_ITEM_CHANNEL0);
            state = karaoke_mix_channel_state(mix, ch);
            lvl = levels[ch] > 24U ? 1U : 0U;
            drum = (drum_mask & (1U << ch)) != 0U;
        }
        bool changed = karaoke_mix_item_changed(mix, item);
        uint32_t h = 2166136261U;
        h = mix_hash(h, item);
        h = mix_hash(h, (uint32_t)(value + 1000));
        h = mix_hash(h, (uint32_t)state + (sel ? 16U : 0U) + (changed ? 64U : 0U) + (uint32_t)lvl * 256U);
        h = mix_hash(h, drum ? 1U : 0U);
        h = mix_hash(h, kind == MIX_KIND_CHANNEL ? programs[ch] : 0U);
        if (h == s_mix_row_hash[slot] && h != 0U) {
            continue;
        }
        s_mix_row_hash[slot] = h;

        uint16_t y = (uint16_t)(MIX_LIST_Y + slot * MIX_ROW_H);
        uint16_t bg = sel ? LCD_COLOR_DARKCYAN : LCD_COLOR_BLACK;
        uint16_t fg = sel ? LCD_COLOR_YELLOW : (changed ? LCD_COLOR_CYAN : LCD_COLOR_WHITE);
        char name[32];
        char val[16];

        if (kind == MIX_KIND_CHANNEL) {
            char scratch[12];

            const char *chname = karaoke_mix_item_name(item, scratch, sizeof(scratch));

            if (drum) {
                snprintf(name, sizeof(name), "%s %s kit", chname, karaoke_mix_kit_name(programs[ch]));
            } else {
                snprintf(name, sizeof(name), "%s %s", chname, karaoke_mix_program_name(programs[ch]));
            }
            if (state == 1) {
                snprintf(val, sizeof(val), "MUTE");
            } else if (state == 2) {
                snprintf(val, sizeof(val), "SOLO");
            } else if (value <= -40) {
                snprintf(val, sizeof(val), "-inf");
            } else {
                snprintf(val, sizeof(val), "%+ddB", value);
            }
        } else if (kind == MIX_KIND_TONE) {
            snprintf(name, sizeof(name), "%s", karaoke_mix_item_name(item, NULL, 0));
            snprintf(val, sizeof(val), "%+ddB", value);
        } else if (kind == MIX_KIND_ACTION) {
            snprintf(name, sizeof(name), "%s", karaoke_mix_item_name(item, NULL, 0));
            val[0] = '\0';
        } else {
            snprintf(name, sizeof(name), "%s", karaoke_mix_item_name(item, NULL, 0));
            if (value <= -40) {
                snprintf(val, sizeof(val), "-inf");
            } else {
                snprintf(val, sizeof(val), "%+ddB", value);
            }
        }
        if (state == 1) fg = sel ? LCD_COLOR_YELLOW : LCD_COLOR_RED;
        if (state == 2) fg = sel ? LCD_COLOR_YELLOW : LCD_COLOR_GREEN;

        lcd_fill_rect(0, y, 239, (uint16_t)(y + MIX_ROW_H - 1U), bg);
        if (kind == MIX_KIND_ACTION) {
            lcd_show_string(10, y + 2, name, sel ? LCD_COLOR_YELLOW : LCD_COLOR_GREEN, bg);
            if (item == MIX_ITEM_DONE) {
                lcd_show_string(214, y + 2, ">", sel ? LCD_COLOR_YELLOW : LCD_COLOR_GREEN, bg);
            }
            continue;
        }
        lcd_show_string(10, y, name, fg, bg);
        lcd_show_string((uint16_t)(238U - 8U * (uint16_t)strlen(val) - ((kind == MIX_KIND_CHANNEL) ? 12U : 0U)), y, val, fg, bg);
        if (kind == MIX_KIND_CHANNEL) {
            lcd_fill_rect(228, (uint16_t)(y + 4), 235, (uint16_t)(y + 11), lvl ? LCD_COLOR_GREEN : LCD_COLOR_DARKGREY);
        }

        /* bar under the text: tone rows from the middle (-6 to +6 dB); fader rows from -40 dB to +10 dB */
        lcd_fill_rect(10, (uint16_t)(y + 17), 227, (uint16_t)(y + 19), LCD_COLOR_DARKGREY);
        uint16_t bar_color = sel ? LCD_COLOR_GREEN : (changed ? LCD_COLOR_CYAN : LCD_COLOR_GRAY);
        if (kind == MIX_KIND_TONE) {
            int mid = 10 + 109;
            int w = (value * 109) / MIX_TONE_MAX_DB;
            if (w > 0) {
                lcd_fill_rect((uint16_t)mid, (uint16_t)(y + 17), (uint16_t)(mid + w), (uint16_t)(y + 19), bar_color);
            } else if (w < 0) {
                lcd_fill_rect((uint16_t)(mid + w), (uint16_t)(y + 17), (uint16_t)mid, (uint16_t)(y + 19), bar_color);
            }
        } else if (state == 0 && value > -40) {
            /* -40 dB to +10 dB total span (50 dB). Unity 0 dB tick is at x = 10 + 174 = 184 (80% of width) */
            int w = ((value + 40) * 217) / 50;
            if (w > 0) {
                int base_w = (w > 174) ? 174 : w;
                lcd_fill_rect(10, (uint16_t)(y + 17), (uint16_t)(10 + base_w), (uint16_t)(y + 19), bar_color);
                if (w > 174) {
                    /* Boost zone (> 0 dB up to +10 dB): highlighted in warm yellow/gold */
                    lcd_fill_rect(184, (uint16_t)(y + 17), (uint16_t)(10 + w), (uint16_t)(y + 19), LCD_COLOR_YELLOW);
                }
            }
            /* Unity 0 dB tick marker */
            lcd_draw_line(184, (uint16_t)(y + 16), 184, (uint16_t)(y + 20), LCD_COLOR_WHITE);
        }
    }

    /* footer: what OK does on the selected row */
    uint8_t cur_item = view->item[cursor];
    uint8_t foot = (uint8_t)(karaoke_mix_item_kind(cur_item));
    if (cur_item == MIX_ITEM_RESET) foot = 10;
    if (cur_item == MIX_ITEM_SAVE_ALL) foot = 11;
    if (cur_item == MIX_ITEM_DONE) foot = 12;
    if (s_mix_last_foot != foot) {
        const char *line1 = "L+R together: Reset row";
        if (foot == MIX_KIND_CHANNEL) line1 = "L+R together: Mute / Solo";
        if (foot == 10) line1 = "L+R together: Default mix";
        if (foot == 11) line1 = "L+R together: Use for all";
        if (foot == 12) line1 = "RIGHT: Save & close";
        lcd_fill_rect(0, 210, 239, 239, LCD_COLOR_DARKGREY);
        lcd_show_string(8, 210, line1, LCD_COLOR_WHITE, LCD_COLOR_DARKGREY);
        lcd_show_string(8, 225, "UP+DN together: Save & Exit", LCD_COLOR_YELLOW, LCD_COLOR_DARKGREY);
        s_mix_last_foot = foot;
    }

    k_mutex_unlock(&s_ui_lcd_mutex);
}

/* -------------------------------------------------------------------------- */
/* SensMe Mood Channels & 2D Circular Radar Scope                             */
/* -------------------------------------------------------------------------- */
void karaoke_ui_render_sensme(uint8_t cursor, uint8_t active_filter, uint32_t current_song_index)
{
    if (s_view_mode != UI_VIEW_SENSME) return;

    if (k_mutex_lock(&s_ui_lcd_mutex, K_MSEC(50)) != 0) {
        return;
    }

    /* 1. Header (y: 0 to 22) */
    lcd_fill_rect(0, 0, 239, 22, LCD_COLOR_NAVY);
    lcd_show_string(14, 3, "=== SENSME MOOD CHANNELS ===", LCD_COLOR_YELLOW, LCD_COLOR_NAVY);

    /* 2. Sub-header (y: 24 to 34): Active filter */
    lcd_fill_rect(0, 24, 239, 34, LCD_COLOR_BLACK);
    char buf[36];
    if (active_filter == SENSME_CH_ALL) {
        snprintf(buf, sizeof(buf), "Filter: <All Songs> (%u)", (unsigned)sensme_get_song_count());
    } else {
        snprintf(buf, sizeof(buf), "Filter: <%s> (%u)", sensme_channel_name(active_filter),
                 (unsigned)sensme_get_channel_song_count(active_filter));
    }
    lcd_show_string(8, 24, buf, LCD_COLOR_GREEN, LCD_COLOR_BLACK);

    /* 3. Left side: Channel List (x: 4 to 134, y: 38 to 204) - 7 visible slots */
    uint8_t top = 0;
    if (cursor >= 4U) top = (uint8_t)(cursor - 3U);
    if (top > SENSME_CH_COUNT - 7U) top = (uint8_t)(SENSME_CH_COUNT - 7U);

    uint16_t row_y = 38;
    for (uint8_t slot = 0; slot < 7; slot++) {
        uint8_t ch_id = (uint8_t)(top + slot);
        bool sel = (ch_id == cursor);
        bool is_active = (ch_id == active_filter);

        uint16_t bg = sel ? LCD_COLOR_DARKCYAN : LCD_COLOR_BLACK;
        uint16_t fg = sel ? LCD_COLOR_YELLOW : (is_active ? LCD_COLOR_GREEN : LCD_COLOR_WHITE);

        lcd_fill_rect(4, row_y, 134, row_y + 21, bg);
        lcd_draw_rect(4, row_y, 134, row_y + 21, sel ? LCD_COLOR_YELLOW : LCD_COLOR_DARKGREY);

        char ch_buf[24];
        if (ch_id == SENSME_CH_ALL) {
            snprintf(ch_buf, sizeof(ch_buf), "0. %s", sensme_channel_name(ch_id));
        } else {
            snprintf(ch_buf, sizeof(ch_buf), "%u. %s", (unsigned)ch_id, sensme_channel_name(ch_id));
        }
        lcd_show_string(8, row_y + 3, ch_buf, fg, bg);
        row_y += 24;
    }

    /* 4. Right side: Circular 2D Mood Radar Scope (center cx=188, cy=105, R=44) */
    uint16_t cx = 188;
    uint16_t cy = 105;
    uint16_t R = 44;

    /* Radar background and concentric rings */
    lcd_fill_rect(138, 38, 238, 204, LCD_COLOR_BLACK);
    lcd_draw_rect(138, 38, 238, 204, LCD_COLOR_DARKGREY);
    lcd_draw_circle(cx, cy, R, LCD_COLOR_DARKGREY);
    lcd_draw_circle(cx, cy, R / 2, 0x2104); /* Faint inner ring */

    /* Crosshairs */
    lcd_draw_line(cx - R + 2, cy, cx + R - 2, cy, LCD_COLOR_GRAY);
    lcd_draw_line(cx, cy - R + 2, cx, cy + R - 2, LCD_COLOR_GRAY);

    /* Quadrant labels */
    lcd_show_string(cx - R + 3, cy - R + 3, "EXT", LCD_COLOR_RED, LCD_COLOR_BLACK);
    lcd_show_string(cx + R - 26, cy - R + 3, "DNC", LCD_COLOR_MAGENTA, LCD_COLOR_BLACK);
    lcd_show_string(cx - R + 3, cy + R - 14, "EMO", LCD_COLOR_PURPLE, LCD_COLOR_BLACK);
    lcd_show_string(cx + R - 26, cy + R - 14, "RLX", LCD_COLOR_GREEN, LCD_COLOR_BLACK);

    /* Plot Current Song Mood Point */
    sensme_data_t cur_data;
    if (sensme_get_song_data(current_song_index, &cur_data)) {
        int px = (int)cx + ((int)cur_data.valence - 128) * (int)(R - 6) / 128;
        int py = (int)cy - ((int)cur_data.energy - 128) * (int)(R - 6) / 128;
        uint16_t pcolor = sensme_channel_color(cur_data.channel);

        /* Glowing pulsating point */
        lcd_fill_circle((uint16_t)px, (uint16_t)py, 3, pcolor);
        lcd_draw_circle((uint16_t)px, (uint16_t)py, 5, LCD_COLOR_WHITE);

        /* Mood info under radar */
        char mood_buf[20];
        snprintf(mood_buf, sizeof(mood_buf), "V:%3u E:%3u", cur_data.valence, cur_data.energy);
        lcd_show_string(142, 160, mood_buf, LCD_COLOR_LIGHTGREY, LCD_COLOR_BLACK);
        snprintf(mood_buf, sizeof(mood_buf), "[%s]", sensme_channel_name(cur_data.channel));
        lcd_show_string(142, 178, mood_buf, pcolor, LCD_COLOR_BLACK);
    }

    /* 5. Footer (y: 206 to 239) */
    lcd_fill_rect(0, 206, 239, 239, LCD_COLOR_DARKGREY);
    lcd_show_string(8, 208, "RIGHT: Apply Mood Filter", LCD_COLOR_WHITE, LCD_COLOR_DARKGREY);
    lcd_show_string(8, 224, "UP+DN: Save & Exit to Song", LCD_COLOR_YELLOW, LCD_COLOR_DARKGREY);

    k_mutex_unlock(&s_ui_lcd_mutex);
}
