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
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

K_MUTEX_DEFINE(s_ui_lcd_mutex);

static ui_view_mode_t s_view_mode = UI_VIEW_PLAYING;
static song_entry_t s_current_song;
static uint32_t s_song_id;

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
    lcd_clear(LCD_COLOR_BLACK);
    memset(s_last_cur_lyric, 0, sizeof(s_last_cur_lyric));
    memset(s_last_prev_lyric, 0, sizeof(s_last_prev_lyric));
    memset(s_last_up_lyric, 0, sizeof(s_last_up_lyric));
    s_frame_initialized = false;
    k_mutex_unlock(&s_ui_lcd_mutex);
}

ui_view_mode_t karaoke_ui_get_view(void)
{
    return s_view_mode;
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
    s_frame_initialized = false;
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
    static uint32_t s_last_song_id = 0xFFFFFFFFU;
    if (s_last_song_id != s_song_id) {
        s_last_song_id = s_song_id;

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

        snprintf(line_buf, sizeof(line_buf), "Time: %02u:%02u  BPM:%u", (unsigned)min, (unsigned)s, status->bpm);
        char padded_time[28];
        pad_string(padded_time, line_buf, 26);
        lcd_show_string(8, 148, padded_time, LCD_COLOR_LIGHTGREY, LCD_COLOR_BLACK);

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

    /* 6. Active Voice Polyphony HUD: Differential update */
    if (active_voices != s_last_voices) {
        s_last_voices = active_voices;
        snprintf(line_buf, sizeof(line_buf), "Voices: %2u/30  FM:44k ", active_voices);
        lcd_show_string(8, 218, line_buf, LCD_COLOR_GRAY, LCD_COLOR_BLACK);
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
        y += 38;
    }

    /* Footer Navigation Guide (x1=0, y1=220, x2=239, y2=239) */
    lcd_fill_rect(0, 220, 239, 239, LCD_COLOR_DARKGREY);
    lcd_show_string(8, 222, "UP/DN:Sel  OK:Play", LCD_COLOR_WHITE, LCD_COLOR_DARKGREY);

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
        lcd_show_string(12, 166, "[Ready - OK to Play]", LCD_COLOR_YELLOW, LCD_COLOR_BLACK);
    } else {
        lcd_show_string(40, 138, "No Song Matching", LCD_COLOR_RED, LCD_COLOR_BLACK);
        lcd_show_string(24, 158, "Enter 5-digit number", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
    }

    /* 5. Footer (y: 194 to 239) */
    lcd_fill_rect(0, 194, 239, 239, LCD_COLOR_DARKGREY);
    lcd_show_string(8, 200, "HOLD UP/DN or OK: Play", LCD_COLOR_WHITE, LCD_COLOR_DARKGREY);
    lcd_show_string(8, 218, "AUX: Cancel / Return", LCD_COLOR_YELLOW, LCD_COLOR_DARKGREY);

    k_mutex_unlock(&s_ui_lcd_mutex);
}
