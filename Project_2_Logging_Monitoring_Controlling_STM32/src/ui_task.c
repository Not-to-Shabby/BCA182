/**
 * @file ui_task.c
 * @brief Display thread: exclusive owner of the ST7789 LCD. Renders the live
 *        bench dashboard and manages the backlight (wakes on proximity).
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <stdio.h>

#include "app_config.h"
#include "app_context.h"
#include "lcd_st7789.h"
#include "telemetry.h"

#define RGB565(r, g, b)                                                        \
    (uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3))

#define COL_BG    RGB565(16, 18, 24)
#define COL_TEXT  RGB565(230, 232, 238)
#define COL_DIM   RGB565(120, 124, 134)
#define COL_OK    RGB565(80, 200, 120)
#define COL_WARN  RGB565(240, 180, 60)
#define COL_ALERT RGB565(235, 80, 70)

static char s_line[40];

static void draw_line(uint16_t x, uint16_t y, const char *txt, uint16_t color)
{
    lcd_show_string(x, y, txt, color, COL_BG);
}

void ui_task_run(void)
{
    printk("[UI] display thread started\n");

    lcd_st7789_init();
    lcd_backlight_on();
    lcd_clear(COL_BG);

    int64_t backlight_until = k_uptime_get() + 10000;
    int64_t next = 0;

    for (;;) {
        k_msleep(50);
        if (k_uptime_get() < next) {
            continue;
        }
        next = k_uptime_get() + APP_UI_PERIOD_MS;

        telemetry_snapshot_t snap;
        app_state_get(&snap);

        /* Backlight: someone at the bench (proximity) or fresh motion. */
        if (snap.proximity > APP_PROXIMITY_WAKE || snap.motion_event) {
            backlight_until = k_uptime_get() + 10000;
        }
        if (k_uptime_get() < backlight_until) {
            lcd_backlight_on();
        } else {
            lcd_backlight_off();
        }

        snprintf(s_line, sizeof(s_line), "BENCHWATCH  seq:%u", (unsigned)snap.seq);
        draw_line(8, 8, s_line, COL_DIM);

        snprintf(s_line, sizeof(s_line), "Temp: %.1f C   Hum: %.1f %%RH",
                 (double)snap.temperature_c, (double)snap.humidity_pct);
        draw_line(8, 30, s_line, COL_TEXT);

        snprintf(s_line, sizeof(s_line), "Lux:%5u   Prox:%4u", (unsigned)snap.light,
                 (unsigned)snap.proximity);
        draw_line(8, 52, s_line, COL_TEXT);

        snprintf(s_line, sizeof(s_line), "|a| %4.2f g  %s",
                 (double)snap.motion_mag, snap.motion_event ? "MOTION!" : "");
        draw_line(8, 74, s_line, snap.motion_event ? COL_ALERT : COL_TEXT);

        snprintf(s_line, sizeof(s_line), "Buzzer:%s  LED:%s",
                 snap.buzzer_on ? "ON " : "off", telemetry_led_mode_name(snap.led_mode));
        draw_line(8, 96, s_line, snap.buzzer_on ? COL_WARN : COL_TEXT);

        snprintf(s_line, sizeof(s_line), "WiFi:%s  MQTT:%s",
                 snap.wifi_ok ? "up " : "dn ", snap.mqtt_ok ? "up " : "dn ");
        draw_line(8, 118, s_line,
                  (snap.wifi_ok && snap.mqtt_ok) ? COL_OK : COL_ALERT);
    }
}

K_THREAD_DEFINE(ui_thread, 2048, ui_task_run, NULL, NULL, NULL, 12, 0, 0);
