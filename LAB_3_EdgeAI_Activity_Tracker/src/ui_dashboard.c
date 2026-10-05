/**
 * ==============================================================================
 * UI Dashboard Implementation for ST7789 240x240 IPS Color Display
 * ==============================================================================
 */

#include "ui_dashboard.h"
#include "lcd_st7789.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define BG_COLOR            LCD_COLOR_BLACK
#define HEADER_BG           LCD_COLOR_NAVY
#define CARD_BORDER_COLOR   0x39E7 /* Dark Slate Grey */

static activity_type_t s_last_act = ACTIVITY_UNKNOWN;
static uint8_t s_last_conf = 255;
static uint32_t s_last_steps = 0xFFFFFFFF;
static uint8_t s_last_cadence = 255;

void ui_dashboard_init(void)
{
    /* 1. Initialize LCD controller & backlight */
    lcd_st7789_init();

    /* 2. Clear entire screen */
    lcd_clear(BG_COLOR);

    /* 3. Top Header Banner */
    lcd_fill_rect(0, 0, 239, 25, HEADER_BG);
    lcd_show_string(36, 5, "SPARK EDGE AI TRACKER", LCD_COLOR_WHITE, HEADER_BG);

    /* 4. Subheader Info */
    lcd_show_string(36, 30, "BLE: 0xFEE7 | FPU: ON", LCD_COLOR_LIGHTGREY, BG_COLOR);

    /* 5. Hero Activity Box Outline */
    lcd_draw_rect(10, 48, 229, 110, LCD_COLOR_CYAN);
    lcd_draw_rect(11, 49, 228, 109, LCD_COLOR_CYAN);
    lcd_show_string(56, 54, "CURRENT ACTIVITY", LCD_COLOR_GRAY, BG_COLOR);
    lcd_show_string(48, 72, "[ INITIALIZING ]", LCD_COLOR_CYAN, BG_COLOR);

    /* Confidence Bar Outline */
    lcd_draw_rect(20, 94, 219, 102, CARD_BORDER_COLOR);

    /* 6. Metrics Card */
    lcd_draw_rect(10, 116, 229, 178, CARD_BORDER_COLOR);
    lcd_show_string(18, 122, "STEPS  :", LCD_COLOR_LIGHTGREY, BG_COLOR);
    lcd_show_string(82, 122, "0", LCD_COLOR_YELLOW, BG_COLOR);

    lcd_show_string(126, 122, "CADENCE:", LCD_COLOR_LIGHTGREY, BG_COLOR);
    lcd_show_string(190, 122, "0 SPM", LCD_COLOR_WHITE, BG_COLOR);

    lcd_show_string(18, 142, "ACC MAG:", LCD_COLOR_LIGHTGREY, BG_COLOR);
    lcd_show_string(82, 142, "1.00g", LCD_COLOR_CYAN, BG_COLOR);

    lcd_show_string(126, 142, "INFER  :", LCD_COLOR_LIGHTGREY, BG_COLOR);
    lcd_show_string(190, 142, "< 5 us", LCD_COLOR_GREEN, BG_COLOR);

    lcd_show_string(18, 160, "SENSOR :", LCD_COLOR_LIGHTGREY, BG_COLOR);
    lcd_show_string(82, 160, "ICM-20608 10Hz", LCD_COLOR_WHITE, BG_COLOR);

    /* 7. Live IMU Dynamic Monitor */
    lcd_draw_rect(10, 184, 229, 222, CARD_BORDER_COLOR);
    lcd_show_string(18, 188, "LIVE ACCEL DYNAMICS (g):", LCD_COLOR_GRAY, BG_COLOR);
    lcd_show_string(20, 204, "X:+0.00 Y:-0.00 Z:+1.00", LCD_COLOR_CYAN, BG_COLOR);

    /* 8. Footer */
    lcd_draw_line(10, 226, 229, 226, CARD_BORDER_COLOR);
    lcd_show_string(20, 228, "MSU-IIT BCA180 | Zephyr", LCD_COLOR_DARKGREY, BG_COLOR);

    s_last_act = ACTIVITY_UNKNOWN;
    s_last_conf = 255;
    s_last_steps = 0xFFFFFFFF;
    s_last_cadence = 255;
}

void ui_dashboard_update(
    activity_type_t act,
    uint8_t confidence,
    uint8_t cadence_spm,
    uint32_t step_count,
    float mag_g,
    float ax,
    float ay,
    float az)
{
    char buf[32];

    /* ---------------------------------------------------------------------- */
    /* 1. Hero Activity Box Update (Differential)                             */
    /* ---------------------------------------------------------------------- */
    if (act != s_last_act) {
        uint16_t border_color;
        const char *act_text;
        uint16_t text_color;
        uint16_t text_x;

        if (act == ACTIVITY_WALKING) {
            border_color = LCD_COLOR_GREEN;
            act_text = "[  WALKING   ]";
            text_color = LCD_COLOR_GREEN;
            text_x = 60;
        } else if (act == ACTIVITY_RUNNING) {
            border_color = LCD_COLOR_ORANGE;
            act_text = ">>> RUNNING <<<";
            text_color = LCD_COLOR_ORANGE;
            text_x = 60;
        } else if (act == ACTIVITY_STATIONARY) {
            border_color = LCD_COLOR_CYAN;
            act_text = "[ STATIONARY ]";
            text_color = LCD_COLOR_CYAN;
            text_x = 60;
        } else {
            border_color = LCD_COLOR_GRAY;
            act_text = "[  SAMPLING  ]";
            text_color = LCD_COLOR_GRAY;
            text_x = 60;
        }

        /* Update box border */
        lcd_draw_rect(10, 48, 229, 110, border_color);
        lcd_draw_rect(11, 49, 228, 109, border_color);

        /* Clear and rewrite activity title */
        lcd_fill_rect(20, 70, 219, 88, BG_COLOR);
        lcd_show_string(text_x, 72, act_text, text_color, BG_COLOR);

        s_last_act = act;
    }

    /* ---------------------------------------------------------------------- */
    /* 2. Confidence Progress Bar Update                                      */
    /* ---------------------------------------------------------------------- */
    if (confidence != s_last_conf) {
        if (confidence > 100) confidence = 100;
        uint16_t max_w = 196; /* 218 - 22 */
        uint16_t fill_w = (uint16_t)((max_w * confidence) / 100);

        uint16_t bar_color = (act == ACTIVITY_RUNNING) ? LCD_COLOR_ORANGE :
                             ((act == ACTIVITY_WALKING) ? LCD_COLOR_GREEN : LCD_COLOR_CYAN);
        if (fill_w > 0) {
            lcd_fill_rect(22, 96, 22 + fill_w, 100, bar_color);
        }
        if (fill_w < max_w) {
            lcd_fill_rect(22 + fill_w, 96, 218, 100, BG_COLOR);
        }
        s_last_conf = confidence;
    }

    /* ---------------------------------------------------------------------- */
    /* 3. Steps Counter Update                                                */
    /* ---------------------------------------------------------------------- */
    if (step_count != s_last_steps) {
        snprintf(buf, sizeof(buf), "%-5lu", (unsigned long)step_count);
        lcd_show_string(82, 122, buf, LCD_COLOR_YELLOW, BG_COLOR);
        s_last_steps = step_count;
    }

    /* ---------------------------------------------------------------------- */
    /* 4. Cadence Update                                                      */
    /* ---------------------------------------------------------------------- */
    if (cadence_spm != s_last_cadence) {
        snprintf(buf, sizeof(buf), "%3u SPM", (unsigned int)cadence_spm);
        lcd_show_string(190, 122, buf, LCD_COLOR_WHITE, BG_COLOR);
        s_last_cadence = cadence_spm;
    }

    /* ---------------------------------------------------------------------- */
    /* 5. Acceleration Magnitude Update                                       */
    /* ---------------------------------------------------------------------- */
    snprintf(buf, sizeof(buf), "%4.2fg", (double)mag_g);
    lcd_show_string(82, 142, buf, LCD_COLOR_CYAN, BG_COLOR);

    /* ---------------------------------------------------------------------- */
    /* 6. Live 3-Axis Stream Readout (Differential)                           */
    /* ---------------------------------------------------------------------- */
    snprintf(buf, sizeof(buf), "X:%+4.2f Y:%+4.2f Z:%+4.2f",
             (double)ax, (double)ay, (double)az);
    lcd_show_string(20, 204, buf, LCD_COLOR_CYAN, BG_COLOR);
}
