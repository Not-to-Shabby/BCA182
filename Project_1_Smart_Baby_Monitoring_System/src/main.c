/**
 * @file main.c
 * @brief Project 1 - Smart Baby Monitoring System: presentation + alert layer.
 *
 * Builds on the P0 bring-up (RT-Spark STM32F407ZGT6, Zephyr RTOS) and wires
 * the full application together:
 *   - SENSOR layer  : sensor_task_start() (AHT10 / AP3216C / ICM20608 poll,
 *                     decision logic refresh) - see sensor_task.h
 *   - PRESENTATION  : ui_dashboard on the ST7789 240x240 LCD (differential
 *                     redraw), state tint on the onboard RGB LED matrix
 *   - ALERT OUTPUT  : TIM3 PWM buzzer (PB0, fixed 2 kHz alarm tone)
 *   - USER INPUT    : KEY0 acknowledge / KEY1 test-alert toggle (PC0/PC1,
 *                     debounced in buttons.c)
 *   - SHARED STATE  : monitor_state_t snapshot guarded by one mutex
 *
 * Thread architecture (Zephyr RTOS, statically defined):
 *   blink  (prio 8)  500 ms: toggles onboard red LED PF12 (liveness)
 *   ui     (prio 9)  500 ms: snapshot copy -> dashboard redraw, button poll,
 *                            buzzer drive, LED matrix state tint
 *   status (prio 10) 1000 ms: console heartbeat only (every 5th cycle = 5 s)
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <stm32f4xx.h>

#include "lcd_st7789.h"
#include "led_matrix.h"
#include "app_version.h"
#include "monitor_state.h"
#include "baby_logic.h"
#include "buzzer.h"
#include "buttons.h"
#include "ui_dashboard.h"
#include "sensor_task.h"
#include "audio_capture.h"
#include "cry_task.h"

#include <stdio.h>

#define BLINK_PERIOD_MS      500U
#define UI_PERIOD_MS         500U
#define STATUS_PERIOD_MS     1000U
#define HEARTBEAT_DIVIDER    5U    /* status cycles per console heartbeat (5 s) */

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

/* UI/alert cycle: differential dashboard redraw, debounced keys, buzzer and
 * LED matrix state tint. Everything runs on a local snapshot copy taken under
 * the state mutex, so no lock is held across LCD/register IO. */
static void ui_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    monitor_state_t snap;
    uint32_t next_wake = k_uptime_get_32();
    uint32_t cycle = 0;

    for (;;) {
        wait_hw_ready();
        cycle++;
        const uint32_t now = k_uptime_get_32();

        monitor_state_lock();
        snap = *monitor_state_get();
        monitor_state_unlock();

        /* Dashboard: only changed regions are repainted */
        ui_dashboard_update(&snap);

        /* KEY0 = acknowledge alert (60 s mute), KEY1 = test alert toggle */
        buttons_poll(now);

        /* Buzzer: logic decision OR the UI-level demo alert. The demo alert
         * is suppressed in FAULT so a device fault can never be mistaken for
         * a working alarm demonstration. */
        const bool effective_buzzer =
            snap.buzzer_on ||
            (snap.test_mode && snap.state != BABY_STATE_FAULT);
        buzzer_set(effective_buzzer);

        /* LED matrix pixel 0 tint by system state */
        uint8_t r = 0, g = 0, b = 0;
        switch (snap.state) {
        case BABY_STATE_ALERT:
            /* Bright red blink, toggling with the 500 ms UI cycle parity */
            if ((cycle & 1U) != 0U) {
                r = 255U;
            } else {
                r = 30U;
            }
            g = 0;
            b = 0;
            break;
        case BABY_STATE_FAULT:
            r = 200U; g = 80U; b = 0U; /* amber, steady */
            break;
        case BABY_STATE_NORMAL:
        default:
            r = 0U; g = 16U; b = 0U;   /* dim green, steady */
            break;
        }
        led_matrix_set_pixel(0, r, g, b);
        led_matrix_show();

        sleep_until(&next_wake, UI_PERIOD_MS);
    }
}

/* Console heartbeat only: the LCD uptime row moved to the UI dashboard. */
static void status_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    uint32_t next_wake = k_uptime_get_32();
    uint32_t cycle = 0;

    for (;;) {
        wait_hw_ready();
        cycle++;

        if ((cycle % HEARTBEAT_DIVIDER) == 0U) {
            printk("[APP] heartbeat: uptime %u s (cycle %u)\n",
                   (unsigned)(k_uptime_get_32() / 1000U), (unsigned)cycle);
        }

        sleep_until(&next_wake, STATUS_PERIOD_MS);
    }
}

/* Static thread definitions (house style: K_THREAD_DEFINE, no dynamic allocation) */
K_THREAD_DEFINE(blink_thread, 1024, blink_thread_fn, NULL, NULL, NULL, 8, 0, 0);
K_THREAD_DEFINE(ui_thread, 2048, ui_thread_fn, NULL, NULL, NULL, 9, 0, 0);
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

    /* Application layers: shared state, outputs, inputs, presentation, sensors */
    monitor_state_init();
    printk("[APP] Monitor state initialized (mutex-guarded snapshot)\n");

    buzzer_init();
    printk("[APP] Buzzer initialized (TIM3 CH3 PB0, 2 kHz, idle off)\n");

    buttons_init();
    printk("[APP] Buttons initialized (KEY0=PC0 ack, KEY1=PC1 test)\n");

    ui_dashboard_init();
    printk("[APP] Dashboard static frame painted (header/cards/disclaimer)\n");

    sensor_task_start();
    printk("[APP] Sensor task started (AHT10/AP3216C/ICM20608, 2 s)\n");

    /* AUDIO CAPTURE PATH (P2): ES8388 codec + I2S3 TX engine, then the
     * I2S3ext RX capture path (PB4, DMA1 S2 Ch2 circular). */
    audio_capture_init();
    printk("[APP] Audio codec + capture path initialized (ES8388, 16 kHz)\n");

    audio_capture_start();
    printk("[APP] Audio capture started (mic -> I2S3ext RX -> mono ring)\n");

    /* ON-DEVICE CRY CLASSIFIER: drains the capture ring after the boot demo
     * releases it (self-gating thread; 2-of-3 windows -> cry -> ALERT). */
    cry_task_start();
    printk("[APP] Cry task started (on-device classifier, 2-of-3 windows)\n");

    /* LED matrix liveness until the UI thread takes over pixel 0 */
    led_matrix_set_pixel(0, 0, 16, 0);
    led_matrix_show();

    s_hw_ready = true;
    printk("[APP] Bring-up complete: blink(8) + ui(9) + status(10) threads released\n");
    return 0;
}
