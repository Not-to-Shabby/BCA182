/**
 * @file ui_dashboard.c
 * @brief 240x240 ST7789 dashboard: product dashboard plus DIAG / COLLECT / BENCH
 *        service screens. Renders through the Lab 3-proven lcd_st7789 driver.
 */

#include "ui_dashboard.h"
#include "lcd_st7789.h"
#include "data_logger.h"
#include "cycle_model.h"

#include <stdio.h>
#include <string.h>

#define LCD_W 240
#define LCD_H 240

/* Palette (RGB565) */
#define COL_BG      0x0000U
#define COL_TITLE   0xFFFFU
#define COL_TEXT    0xC638U /* dim grey-blue */
#define COL_VALUE   0xFFFFU
#define COL_IDLE    0x03FFU /* blue */
#define COL_WASH    0x07FFU /* cyan */
#define COL_SPIN    0xFD20U /* orange */
#define COL_DONE    0x07E0U /* green */
#define COL_ALERT   0xF800U /* red */
#define COL_BAR     0x07E0U
#define COL_BAR_BG  0x1863U

static const uint16_t STATE_COLORS[4] = { COL_IDLE, COL_WASH, COL_SPIN, COL_DONE };

static char s_mode_char = 'R'; /* per-mode change detection for the static frame */

void ui_dashboard_init(void)
{
    lcd_st7789_init();
    lcd_backlight_on();
    lcd_clear(COL_BG);
    s_mode_char = 0;
}

static void draw_static_frame(char mode_char)
{
    lcd_clear(COL_BG);
    lcd_show_string(8, 6, "LAUNDRY SENTINEL", COL_TITLE, COL_BG);
    lcd_draw_line(0, 22, LCD_W, 22, COL_TEXT);
    lcd_draw_line(0, LCD_H - 18, LCD_W, LCD_H - 18, COL_TEXT);

    switch (mode_char) {
    case 'D':
        lcd_show_string(8, 226, "KEY0: next page  KEY1: exit", COL_TEXT, COL_BG);
        break;
    case 'C':
        lcd_show_string(8, 226, "KEY0: cycle label KEY1: exit", COL_TEXT, COL_BG);
        break;
    case 'B':
        lcd_show_string(8, 226, "Timing 50 live inferences", COL_TEXT, COL_BG);
        break;
    default:
        lcd_show_string(8, 226, "KEY1: service mode", COL_TEXT, COL_BG);
        break;
    }
    s_mode_char = mode_char;
}

static void render_run(const ui_snapshot_t *snap)
{
    static const uint16_t big_x = 40;
    static const uint16_t big_y = 40;

    /* Big state name */
    const char *name = cycle_state_name(snap->decision.state);
    lcd_show_string(big_x, big_y, "      ", STATE_COLORS[snap->decision.state], COL_BG);
    lcd_show_string(big_x, big_y, name, STATE_COLORS[snap->decision.state], COL_BG);

    if (snap->alert) {
        /* Twin-tub machine: name the tub that finished (blink comes from
         * snap->alert alternating across 4 Hz frames). */
        const char *banner = (snap->decision.alert_reason == CYCLE_REASON_WASH)
                                 ? "WASH DONE! Move"
                                 : (snap->decision.alert_reason == CYCLE_REASON_SPIN)
                                       ? "SPIN DONE! Hang"
                                       : "CYCLE DONE!";
        lcd_fill_rect(0, 26, LCD_W, 8, COL_ALERT);
        lcd_show_string(48, 120, banner, COL_ALERT, COL_BG);
    } else {
        lcd_fill_rect(0, 26, LCD_W, 8, COL_BG);
        lcd_show_string(48, 120, "                ", COL_TEXT, COL_BG);
    }

    /* Vibration gauge: rms 0..0.5 g across 200 px */
    char line[24];
    lcd_show_string(8, 150, "vib", COL_TEXT, COL_BG);
    lcd_draw_rect(40, 148, 200, 160, COL_TEXT);
    uint16_t fill = (uint16_t)(snap->rms_g * 320.0f);
    if (fill > 156U) {
        fill = 156U;
    }
    lcd_fill_rect(41, 149, (uint16_t)(41 + fill), 159, COL_BAR_BG);
    if (fill > 1U) {
        lcd_fill_rect(41, 149, (uint16_t)(41 + fill), 159, COL_BAR);
    }
    snprintf(line, sizeof(line), "%.3f g  %2u Hz   ", (double)snap->rms_g, (unsigned)snap->dom_hz);
    lcd_show_string(8, 166, line, COL_VALUE, COL_BG);

    /* Model vs baseline agreement marker */
    snprintf(line, sizeof(line), "ML:%s  RULE:%s %s",
             snap->decision.model_class == 0 ? "IDLE" :
             snap->decision.model_class == 1 ? "WASH" : "SPIN",
             snap->decision.baseline_class == 0 ? "IDLE" :
             snap->decision.baseline_class == 1 ? "WASH" : "SPIN",
             snap->decision.models_agree ? " ok" : " x");
    lcd_show_string(8, 182, line, COL_TEXT, COL_BG);

    /* Ambient context */
    snprintf(line, sizeof(line), "T:%4.1fC H:%3.0f%% L:%4u",
             (double)snap->temperature_c, (double)snap->humidity_pct,
             snap->lux == 0xFFFFU ? 0U : snap->lux);
    lcd_show_string(8, 198, line, COL_TEXT, COL_BG);
}

static void render_diag(const ui_snapshot_t *snap)
{
    int y = 32;

    if (snap->diag_page == 0) {
        char line[32];
        lcd_show_string(8, y, "ICM20608 IMU  primary", COL_VALUE, COL_BG); y += 18;
        lcd_show_string(8, y, "vib ring live sampling", COL_TEXT, COL_BG); y += 22;
        lcd_show_string(8, y, "AHT10 / AP3216C:", COL_VALUE, COL_BG); y += 18;
        snprintf(line, sizeof(line), "T %.1fC  H %.0f%%  L %u",
                 (double)snap->temperature_c, (double)snap->humidity_pct,
                 snap->lux == 0xFFFFU ? 0U : snap->lux);
        lcd_show_string(8, y, line, COL_TEXT, COL_BG); y += 22;
        lcd_show_string(8, y, "Model:", COL_VALUE, COL_BG); y += 18;
        lcd_show_string(8, y, cycle_model_training_set(), COL_TEXT, COL_BG); y += 18;
        lcd_show_string(8, y, "(1/2) KEY0: event log", COL_TEXT, COL_BG);
    } else {
        char evbuf[40];
        lcd_show_string(8, y, "Recent events:", COL_VALUE, COL_BG); y += 18;
        for (uint32_t i = 0; i < 7; i++) {
            if (data_logger_event_text(i, evbuf, sizeof(evbuf)) != NULL) {
                lcd_show_string(8, y, evbuf, COL_TEXT, COL_BG);
            } else {
                lcd_show_string(8, y, "                     ", COL_TEXT, COL_BG);
            }
            y += 18;
        }
        lcd_show_string(8, y + 4, "(2/2) KEY0: sensors", COL_TEXT, COL_BG);
    }
}

static void render_collect(const ui_snapshot_t *snap)
{
    char line[32];
    static const char *LABELS[3] = { "IDLE", "WASH", "SPIN" };
    const uint16_t label_color = STATE_COLORS[snap->collect_label];

    lcd_show_string(8, 34, "Training capture", COL_TITLE, COL_BG);
    lcd_show_string(8, 70, "label:", COL_TEXT, COL_BG);
    lcd_show_string(64, 62, "     ", COL_BG, COL_BG);
    lcd_show_string(64, 62, LABELS[snap->collect_label], label_color, COL_BG);
    snprintf(line, sizeof(line), "samples: %u      ", (unsigned)snap->collect_samples);
    lcd_show_string(8, 96, line, COL_VALUE, COL_BG);
    lcd_show_string(8, 120, "100 Hz CSV on console", COL_TEXT, COL_BG);
    lcd_show_string(8, 138, "run real appliance phases", COL_TEXT, COL_BG);
    lcd_show_string(8, 156, "then retrain with", COL_TEXT, COL_BG);
    lcd_show_string(8, 174, "ml_model/train_cycle_model.py", COL_TEXT, COL_BG);
}

static void render_bench(const ui_snapshot_t *snap)
{
    char line[32];
    lcd_show_string(8, 34, "Inference bench", COL_TITLE, COL_BG);
    snprintf(line, sizeof(line), "runs done: %u / 50", (unsigned)snap->bench_done);
    lcd_show_string(8, 70, line, COL_VALUE, COL_BG);
    snprintf(line, sizeof(line), "median: %u us", (unsigned)snap->bench_median_us);
    lcd_show_string(8, 96, line, COL_VALUE, COL_BG);
    snprintf(line, sizeof(line), "p95:    %u us", (unsigned)snap->bench_p95_us);
    lcd_show_string(8, 118, line, COL_VALUE, COL_BG);
    lcd_show_string(8, 150, "measures feature extract", COL_TEXT, COL_BG);
    lcd_show_string(8, 168, "+ forest on live windows", COL_TEXT, COL_BG);
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
        draw_static_frame(mode_char);
    }

    switch (snap->mode) {
    case UI_MODE_DIAG:    render_diag(snap); break;
    case UI_MODE_COLLECT: render_collect(snap); break;
    case UI_MODE_BENCH:   render_bench(snap); break;
    default:              render_run(snap); break;
    }
}
