/**
 * @file ui_dashboard.c
 * @brief ST7789 240x240 dashboard in the Lab 3 design language: navy header
 *        banner, boxed cards, a double-bordered hero box with the state word
 *        rendered in a 3x scaled font, differential field updates (fields are
 *        only redrawn when their content changes, so the display never flickers
 *        or leaves residue), and a blinking full-width alert strip.
 */

#include "ui_dashboard.h"
#include "lcd_st7789.h"
#include "data_logger.h"
#include "cycle_model.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

/* lcd_st7789.c owns the font table (lcd_font.h defines it non-extern); a
 * second include would duplicate the 1.5 KB array in Flash. */
extern const uint8_t asc2_1608[];

#define LCD_W 240
#define LCD_H 240

#define BG          LCD_COLOR_BLACK
#define CARD_BORDER LCD_COLOR_DARKGREY

/* Hero layout: the big state word is 4 chars at 3x scale (96x48 px) */
#define BIG_SCALE   3
#define BIG_W       (8 * BIG_SCALE)
#define BIG_H       (16 * BIG_SCALE)
#define BIG_TEXT_Y  74
#define HERO_LABEL_Y 54

static char s_mode_char;   /* current static frame ('R','D','C','B'), 0 = none */

/* Differential-update caches (RUN screen) */
static int8_t   s_last_state = -1;
static int8_t   s_last_reason = -1;
static bool     s_last_alert_phase;
static uint16_t s_last_bar_w = 0xFFFF;
static char     s_last_vib[20];
static char     s_last_ml[24];
static char     s_last_env[26];
static char     s_last_age[12];

/* DIAG event page cache: log grows monotonically, redraw when count changes */
static uint32_t s_last_log_total;

void ui_dashboard_init(void)
{
    lcd_st7789_init();
    lcd_backlight_on();
    s_mode_char = 0;
}

/* ------------------------------------------------------- scaled font ------- */
static void big_char(uint16_t x, uint16_t y, char c, uint16_t color)
{
    if (c < ' ' || c > '~') {
        c = ' ';
    }
    const uint8_t *glyph = &asc2_1608[(uint16_t)(c - ' ') * 16U];

    for (uint8_t row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (uint8_t col = 0; col < 8; col++) {
            if (bits & 0x80U) {
                lcd_fill_rect((uint16_t)(x + col * BIG_SCALE),
                              (uint16_t)(y + row * BIG_SCALE),
                              (uint16_t)(x + col * BIG_SCALE + BIG_SCALE - 1U),
                              (uint16_t)(y + row * BIG_SCALE + BIG_SCALE - 1U),
                              color);
            }
            bits <<= 1;
        }
    }
}

static void big_string_centered(uint16_t y, const char *str, uint16_t color)
{
    uint16_t x = (uint16_t)((LCD_W - (uint16_t)strlen(str) * BIG_W) / 2U);
    while (*str != '\0') {
        big_char(x, y, *str++, color);
        x = (uint16_t)(x + BIG_W);
    }
}

/* ------------------------------------------------------------ frames ------- */
static void header_footer(const char *title, const char *hint)
{
    lcd_clear(BG);
    lcd_fill_rect(0, 0, LCD_W - 1, 23, LCD_COLOR_NAVY);
    lcd_show_string((uint16_t)((LCD_W - strlen(title) * 8) / 2), 4, title,
                    LCD_COLOR_WHITE, LCD_COLOR_NAVY);
    lcd_draw_line(0, 24, LCD_W - 1, 24, CARD_BORDER);

    lcd_draw_line(0, 224, LCD_W - 1, 224, CARD_BORDER);
    lcd_show_string(8, 228, hint, LCD_COLOR_GRAY, BG);
    lcd_show_string(LCD_W - 56, 228, "BCA182", LCD_COLOR_DARKGREY, BG);
}

static void frame_run(void)
{
    header_footer("LAUNDRY SENTINEL", "K0:ACK DONE  K1 HOLD:MENU");

    /* Alert strip (blinks in render_run) */
    lcd_show_string(48, 30, "TWIN-TUB WASHER MONITOR", LCD_COLOR_DARKGREY, BG);

    /* Hero box */
    lcd_draw_rect(10, 46, 229, 130, LCD_COLOR_CYAN);
    lcd_draw_rect(11, 47, 228, 129, LCD_COLOR_CYAN);
    lcd_show_string(88, HERO_LABEL_Y, "ACTIVE TUB", LCD_COLOR_GRAY, BG);

    /* Vibration card */
    lcd_draw_rect(10, 136, 229, 178, CARD_BORDER);
    lcd_show_string(18, 140, "VIBRATION", LCD_COLOR_LIGHTGREY, BG);
    lcd_draw_rect(18, 158, 221, 168, CARD_BORDER);

    /* Metrics card */
    lcd_draw_rect(10, 182, 229, 220, CARD_BORDER);

    /* Reset caches so the first render paints everything */
    s_last_state = -1;
    s_last_reason = -1;
    s_last_alert_phase = false;
    s_last_bar_w = 0xFFFF;
    s_last_vib[0] = s_last_ml[0] = s_last_env[0] = s_last_age[0] = '\0';
}

static void frame_diag(void)
{
    header_footer("SERVICE: DIAGNOSTICS", "K0:PAGE   K1:NEXT MODE");
    s_last_log_total = 0xFFFFFFFF;
}

static void frame_collect(void)
{
    header_footer("SERVICE: DATA CAPTURE", "K0:LABEL  K1 HOLD:BENCH");

    lcd_draw_rect(10, 36, 229, 130, LCD_COLOR_CYAN);
    lcd_draw_rect(11, 37, 228, 129, LCD_COLOR_CYAN);
    lcd_show_string(100, 44, "LABEL", LCD_COLOR_GRAY, BG);

    lcd_draw_rect(10, 136, 229, 214, CARD_BORDER);
    lcd_show_string(18, 142, "100 Hz CSV streams on the", LCD_COLOR_LIGHTGREY, BG);
    lcd_show_string(18, 158, "USB console. Run a real tub", LCD_COLOR_LIGHTGREY, BG);
    lcd_show_string(18, 174, "phase per label, then train", LCD_COLOR_LIGHTGREY, BG);
    lcd_show_string(18, 190, "with train_cycle_model.py", LCD_COLOR_LIGHTGREY, BG);
}

static void frame_bench(void)
{
    header_footer("SERVICE: INFERENCE BENCH", "K1:EXIT TO RUN");

    lcd_draw_rect(10, 36, 229, 96, LCD_COLOR_CYAN);
    lcd_draw_rect(11, 37, 228, 95, LCD_COLOR_CYAN);
    lcd_show_string(88, 44, "PROGRESS", LCD_COLOR_GRAY, BG);
    lcd_draw_rect(30, 76, 209, 86, CARD_BORDER);

    lcd_draw_rect(10, 102, 229, 158, CARD_BORDER);
    lcd_show_string(18, 110, "MEDIAN:", LCD_COLOR_LIGHTGREY, BG);
    lcd_show_string(18, 134, "P95   :", LCD_COLOR_LIGHTGREY, BG);

    lcd_draw_rect(10, 164, 229, 214, CARD_BORDER);
    lcd_show_string(18, 172, "Times feature extraction", LCD_COLOR_GRAY, BG);
    lcd_show_string(18, 190, "+ forest on live windows", LCD_COLOR_GRAY, BG);
}

/* ------------------------------------------------------------ renderers ---- */
static const char *const STATE_WORDS[4] = { "IDLE", "WASH", "SPIN", "DONE" };
static const uint16_t STATE_COLORS[4] = {
    LCD_COLOR_CYAN, LCD_COLOR_CYAN, LCD_COLOR_ORANGE, LCD_COLOR_GREEN
};

static void render_run(const ui_snapshot_t *snap)
{
    char buf[32];

    /* Alert strip: steady subtitle normally, red/maroon blink while alerting */
    if (snap->alert || s_last_state >= 3) {
        bool on = snap->alert;
        const char *msg = (snap->decision.alert_reason == CYCLE_REASON_WASH)
                              ? "WASH DONE! MOVE CLOTHES"
                              : "SPIN DONE! HANG DRY";
        uint16_t fill = on ? LCD_COLOR_RED : LCD_COLOR_MAROON;
        uint16_t fg = on ? LCD_COLOR_WHITE : LCD_COLOR_LIGHTGREY;
        lcd_fill_rect(0, 26, LCD_W - 1, 42, fill);
        lcd_show_string((uint16_t)((LCD_W - strlen(msg) * 8) / 2), 28, msg, fg, fill);
        s_last_alert_phase = on;
    }

    /* Hero: big state word + border recolor, only on change */
    if (snap->decision.state != s_last_state) {
        int st = (int)snap->decision.state;
        uint16_t border = STATE_COLORS[st];

        lcd_fill_rect(14, 50, 225, 126, BG); /* clear old word (borders stay) */
        lcd_draw_rect(10, 46, 229, 130, border);
        lcd_draw_rect(11, 47, 228, 129, border);
        big_string_centered(BIG_TEXT_Y, STATE_WORDS[st],
                            (st == 0) ? LCD_COLOR_LIGHTGREY : border);
        s_last_state = (int8_t)st;
    }

    /* Vibration bar (differential fill/erase) */
    uint16_t fill_w = (uint16_t)(snap->rms_g * 400.0f);
    if (fill_w > 200U) {
        fill_w = 200U;
    }
    if (fill_w != s_last_bar_w) {
        if (fill_w > 0U) {
            lcd_fill_rect(20, 160, (uint16_t)(19 + fill_w), 166,
                          (fill_w > 120U) ? LCD_COLOR_ORANGE : LCD_COLOR_GREEN);
        }
        if (fill_w < 200U) {
            lcd_fill_rect((uint16_t)(20 + fill_w), 160, 219, 166, BG);
        }
        s_last_bar_w = fill_w;
    }

    snprintf(buf, sizeof(buf), "%5.3fg  %2uHz", (double)snap->rms_g,
             (unsigned)snap->dom_hz);
    if (strcmp(buf, s_last_vib) != 0) {
        lcd_show_string(110, 140, buf, LCD_COLOR_CYAN, BG);
        strcpy(s_last_vib, buf);
    }

    snprintf(buf, sizeof(buf), "ML:%-4s RULE:%-4s %s",
             snap->decision.model_class == 1 ? "WASH" :
             snap->decision.model_class == 2 ? "SPIN" : "IDLE",
             snap->decision.baseline_class == 1 ? "WASH" :
             snap->decision.baseline_class == 2 ? "SPIN" : "IDLE",
             snap->decision.models_agree ? "OK" : "!!");
    if (strcmp(buf, s_last_ml) != 0) {
        lcd_show_string(18, 188, buf,
                        snap->decision.models_agree ? LCD_COLOR_LIGHTGREY : LCD_COLOR_YELLOW,
                        BG);
        strcpy(s_last_ml, buf);
    }

    uint32_t age_s = snap->decision.state_age_ms / 1000U;
    if (age_s > 5999U) age_s = 5999U;
    if (isnan(snap->temperature_c) || isnan(snap->humidity_pct)) {
        snprintf(buf, sizeof(buf), "UP %02u:%02u  T/RH: sensor absent",
                 (unsigned)(age_s / 60U), (unsigned)(age_s % 60U));
    } else {
        snprintf(buf, sizeof(buf), "UP %02u:%02u  T:%2.0fC H:%2.0f%%",
                 (unsigned)(age_s / 60U), (unsigned)(age_s % 60U),
                 (double)snap->temperature_c, (double)snap->humidity_pct);
    }
    if (strcmp(buf, s_last_env) != 0) {
        lcd_show_string(18, 204, buf, LCD_COLOR_LIGHTGREY, BG);
        strcpy(s_last_env, buf);
    }
}

static void render_diag(const ui_snapshot_t *snap)
{
    if (snap->diag_page == 0) {
        char buf[32];
        lcd_draw_rect(10, 32, 229, 218, CARD_BORDER);
        lcd_show_string(18, 38, "SENSORS & MODEL", LCD_COLOR_WHITE, BG);

        lcd_show_string(18, 60, "IMU  : ICM-20608 100Hz", LCD_COLOR_LIGHTGREY, BG);
        snprintf(buf, sizeof(buf), "AHT10: %s",
                 isnan(snap->temperature_c) ? "absent" : "fitted");
        lcd_show_string(18, 78, buf, LCD_COLOR_LIGHTGREY, BG);
        if (!isnan(snap->temperature_c)) {
            snprintf(buf, sizeof(buf), "       %4.1fC  %2.0f%%RH",
                     (double)snap->temperature_c, (double)snap->humidity_pct);
            lcd_show_string(18, 96, buf, LCD_COLOR_CYAN, BG);
        }
        snprintf(buf, sizeof(buf), "LUX  : %u", snap->lux == 0xFFFFU ? 0U : snap->lux);
        lcd_show_string(18, 114, buf, LCD_COLOR_CYAN, BG);

        lcd_show_string(18, 140, "MODEL: RF 10 trees x 14 f", LCD_COLOR_WHITE, BG);
        lcd_show_string(18, 158, "SET  :", LCD_COLOR_LIGHTGREY, BG);
        lcd_show_string(66, 158, cycle_model_training_set(), LCD_COLOR_GRAY, BG);
        lcd_show_string(18, 182, "Hysteresis: 2 windows", LCD_COLOR_GRAY, BG);
        snprintf(buf, sizeof(buf), "State changes: %u", (unsigned)snap->decision.state_changes);
        lcd_show_string(18, 200, buf, LCD_COLOR_GRAY, BG);
    } else {
        lcd_draw_rect(10, 32, 229, 218, CARD_BORDER);
        lcd_show_string(18, 38, "EVENT LOG (newest first)", LCD_COLOR_WHITE, BG);

        if (data_logger_count() != s_last_log_total) {
            char ev[40];
            for (int i = 0; i < 9; i++) {
                int y = 60 + i * 16;
                if (data_logger_event_text((uint32_t)i, ev, sizeof(ev)) != NULL) {
                    lcd_show_string(18, (uint16_t)y, ev, LCD_COLOR_LIGHTGREY, BG);
                } else {
                    lcd_show_string(18, (uint16_t)y, "                        ",
                                    LCD_COLOR_LIGHTGREY, BG);
                }
            }
            s_last_log_total = data_logger_count();
        }
    }
}

static void render_collect(const ui_snapshot_t *snap)
{
    static const char *const LABELS[3] = { "IDLE", "WASH", "SPIN" };
    static int s_last_label = -1;
    char buf[32];

    if (snap->collect_label != s_last_label) {
        lcd_fill_rect(14, 40, 225, 126, BG);
        big_string_centered(BIG_TEXT_Y - 14, LABELS[snap->collect_label],
                            STATE_COLORS[snap->collect_label]);
        s_last_label = snap->collect_label;
    }

    snprintf(buf, sizeof(buf), "SAMPLES: %-8u", (unsigned)snap->collect_samples);
    lcd_show_string(64, 180, buf, LCD_COLOR_YELLOW, BG);
    lcd_show_string(64, 196, "STREAMING 100 Hz CSV", LCD_COLOR_GREEN, BG);
}

static void render_bench(const ui_snapshot_t *snap)
{
    static uint32_t s_last_done = 0xFFFFFFFF;
    char buf[32];

    if (snap->bench_done != s_last_done) {
        uint16_t fill_w = (uint16_t)(snap->bench_done * 178U / 50U);
        if (fill_w > 0U) {
            lcd_fill_rect(31, 78, (uint16_t)(30 + fill_w), 84, LCD_COLOR_GREEN);
        }
        if (fill_w < 178U) {
            lcd_fill_rect((uint16_t)(31 + fill_w), 78, 208, 84, BG);
        }
        snprintf(buf, sizeof(buf), "%2u / 50", (unsigned)snap->bench_done);
        lcd_show_string(100, 56, buf, LCD_COLOR_WHITE, BG);
        s_last_done = snap->bench_done;
    }

    snprintf(buf, sizeof(buf), "%6u us", (unsigned)snap->bench_median_us);
    lcd_show_string(90, 110, buf, LCD_COLOR_CYAN, BG);
    snprintf(buf, sizeof(buf), "%6u us", (unsigned)snap->bench_p95_us);
    lcd_show_string(90, 134, buf, LCD_COLOR_CYAN, BG);
}

void ui_dashboard_render(const ui_snapshot_t *snap)
{
    if (snap == NULL) {
        return;
    }

    const char mode_char =
        snap->mode == UI_MODE_DIAG ? 'D' :
        snap->mode == UI_MODE_COLLECT ? 'C' :
        snap->mode == UI_MODE_BENCH ? 'B' : 'R';

    if (mode_char != s_mode_char) {
        s_mode_char = mode_char;
        switch (mode_char) {
        case 'D': frame_diag(); break;
        case 'C': frame_collect(); break;
        case 'B': frame_bench(); break;
        default:  frame_run(); break;
        }
    }

    switch (snap->mode) {
    case UI_MODE_DIAG:    render_diag(snap); break;
    case UI_MODE_COLLECT: render_collect(snap); break;
    case UI_MODE_BENCH:   render_bench(snap); break;
    default:              render_run(snap); break;
    }
}
