/**
 * @file main.c
 * @brief Project 1 - Smart Baby Monitoring System: P0 firmware bring-up.
 *
 * Verifies the RT-Spark (STM32F407ZGT6) base peripherals under Zephyr RTOS:
 *   - ST7789 240x240 LCD (FSMC 8080 8-bit parallel bus)
 *   - Onboard 19-LED SK6805 RGB matrix (PA7 data, PF2 power enable)
 *   - Onboard red RGB LED (PF12, active LOW) 500 ms heartbeat blink
 *   - 1 Hz status thread: console uptime heartbeat every 5 s plus a
 *     seconds counter on the LCD
 *
 * Thread architecture (Zephyr RTOS, statically defined):
 *   blink  (prio 8)  500 ms: toggles onboard red LED PF12
 *   status (prio 10) 1000 ms: console heartbeat (every 5th cycle) + LCD uptime
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <stm32f4xx.h>

#include "lcd_st7789.h"
#include "led_matrix.h"
#include "app_version.h"

#include <stdio.h>

#define BLINK_PERIOD_MS      500U
#define STATUS_PERIOD_MS     1000U
#define HEARTBEAT_DIVIDER    5U    /* status cycles per console heartbeat (5 s) */

/* LCD layout (8x16 font) */
#define TITLE_Y              8U
#define SUBTITLE_Y           32U
#define UPTIME_Y             120U

/* ---------------------------------------------------------------- shared state */
/* K_THREAD_DEFINE threads start before main() returns; they park here until the
 * hardware bring-up in main() has finished (guards FSMC/GPIO access). */
static volatile bool s_hw_ready;

static void wait_hw_ready(void)
{
    while (!s_hw_ready) {
        k_sleep(K_MSEC(10));
    }
}

/* Deadline-resync sleep (Laboratory 4 pattern): keeps a fixed cadence instead
 * of bursting after a missed deadline. */
static void sleep_until(uint32_t *next_wake_ms, uint32_t period_ms)
{
    *next_wake_ms += period_ms;
    uint32_t now = k_uptime_get_32();
    int32_t remaining = (int32_t)(*next_wake_ms - now);
    if (remaining < 1) {
        /* Deadline missed: resync instead of bursting */
        *next_wake_ms = now + period_ms;
        remaining = period_ms;
    }
    k_sleep(K_MSEC(remaining));
}

/* --------------------------------------------------------------------- bring-up */
static void init_onboard_rgb(void)
{
    /* PF12: onboard red RGB LED, push-pull output (active LOW on RT-Spark) */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOFEN;
    (void)RCC->AHB1ENR; /* settle after clock enable */
    GPIOF->MODER = (GPIOF->MODER & ~(3U << 24)) | (1U << 24);
    GPIOF->OTYPER &= ~(1U << 12);
    GPIOF->OSPEEDR |= (3U << 24);
    GPIOF->PUPDR &= ~(3U << 24);
    GPIOF->BSRR = (1U << 12); /* LED OFF initially */
}

/* ---------------------------------------------------------------------- threads */
static void blink_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    bool on = false;

    for (;;) {
        wait_hw_ready();
        on = !on;
        if (on) {
            GPIOF->BSRR = (1U << (12 + 16)); /* active LOW: red ON */
        } else {
            GPIOF->BSRR = (1U << 12);        /* red OFF */
        }
        k_sleep(K_MSEC(BLINK_PERIOD_MS));
    }
}

static void status_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    char buf[24];
    uint32_t next_wake = k_uptime_get_32();
    uint32_t cycle = 0;

    for (;;) {
        wait_hw_ready();
        cycle++;

        uint32_t uptime_s = k_uptime_get_32() / 1000U;

        /* Console heartbeat once every 5 status cycles */
        if ((cycle % HEARTBEAT_DIVIDER) == 0U) {
            printk("[APP] heartbeat: uptime %u s (cycle %u)\n",
                   (unsigned)uptime_s, (unsigned)cycle);
        }

        /* LCD seconds counter; fixed-width field overwrites cleanly */
        snprintf(buf, sizeof(buf), "UPTIME %4u s", (unsigned)uptime_s);
        lcd_show_string(16, UPTIME_Y, buf, LCD_COLOR_YELLOW, LCD_COLOR_BLACK);

        sleep_until(&next_wake, STATUS_PERIOD_MS);
    }
}

/* Static thread definitions (house style: K_THREAD_DEFINE, no dynamic allocation) */
K_THREAD_DEFINE(blink_thread, 1024, blink_thread_fn, NULL, NULL, NULL, 8, 0, 0);
K_THREAD_DEFINE(status_thread, 2048, status_thread_fn, NULL, NULL, NULL, 10, 0, 0);

/* ------------------------------------------------------------------------ main */
int main(void)
{
    printk("\n=== Project 1: Smart Baby Monitoring System ===\n");
    printk("Zephyr RTOS | %s | %s v%s\n", CONFIG_BOARD, PROJECT_NAME, PROJECT_VERSION);

    init_onboard_rgb();

    led_matrix_init();
    printk("[APP] LED matrix initialized (19x SK6805, PA7/PF2)\n");

    /* lcd_st7789_init() returns void (no failure code); a dead FSMC bus simply
     * leaves a blank display, so log progress and continue regardless. */
    lcd_st7789_init();
    printk("[APP] LCD initialized (ST7789 240x240, FSMC 8-bit)\n");

    /* Splash screen */
    lcd_clear(LCD_COLOR_BLACK);
    lcd_fill_rect(0, 0, LCD_WIDTH - 1, 23, LCD_COLOR_NAVY);
    lcd_draw_rect(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, LCD_COLOR_BLUE);
    lcd_show_string(16, TITLE_Y, "Baby Monitor", LCD_COLOR_WHITE, LCD_COLOR_NAVY);
    lcd_show_string(16, SUBTITLE_Y, "P0 bring-up", LCD_COLOR_LIGHTGREY, LCD_COLOR_BLACK);
    lcd_show_string(16, UPTIME_Y, "UPTIME    0 s", LCD_COLOR_YELLOW, LCD_COLOR_BLACK);

    /* LED matrix liveness: dim green pixel on LED 0 */
    led_matrix_set_pixel(0, 0, 16, 0);
    led_matrix_show();

    s_hw_ready = true;
    printk("[APP] Bring-up complete: blink(8) + status(10) threads released\n");
    return 0;
}
