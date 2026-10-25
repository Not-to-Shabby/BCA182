/**
 * @file service_mode.c
 * @brief Debounced KEY0/KEY1 handling plus the RUN/DIAG/COLLECT/BENCH mode
 *        machine. The transition rules in service_mode_step() are pure C and
 *        covered by host tests; only the pin polling is board-specific.
 */

#include "service_mode.h"
#include "cycle_logic.h"

#include <string.h>

#if defined(__arm__) || defined(CONFIG_SOC_SERIES_STM32F4X) || defined(ZEPHYR_VERSION_CODE)
#include <stm32f4xx.h>
#include <zephyr/sys/printk.h>
#else
#include <stdio.h>
#define printk printf
#endif

#define LONG_PRESS_POLLS 30U /* 30 x 50 ms = 1.5 s */
#define DEBOUNCE_POLLS   2U

static service_mode_t s_mode = SVC_RUN;
static int s_collect_label;
static uint32_t s_collect_samples;
static uint8_t s_diag_page;

static uint32_t s_bench_done;
static uint32_t s_bench_median_us;
static uint32_t s_bench_p95_us;

/* Debounce bookkeeping */
static uint32_t s_key0_down;
static uint32_t s_key1_down;
static bool s_key1_long_fired;

void service_mode_init(void)
{
#if defined(__arm__)
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
    /* PC0 (KEY0) and PC1 (KEY1): inputs with pull-up, active LOW */
    GPIOC->MODER &= ~((3U << 0) | (3U << 2));
    GPIOC->PUPDR = (GPIOC->PUPDR & ~((3U << 0) | (3U << 2))) | ((1U << 0) | (1U << 2));
#endif
    s_mode = SVC_RUN;
    s_collect_label = 0;
    s_collect_samples = 0;
    s_diag_page = 0;
    s_bench_done = 0;
}

service_mode_t service_mode_get(void) { return s_mode; }
int service_mode_collect_label(void) { return s_collect_label; }
bool service_mode_is_collecting(void) { return s_mode == SVC_COLLECT; }
uint32_t service_mode_collect_samples(void) { return s_collect_samples; }
uint32_t service_mode_bench_done(void) { return s_bench_done; }
uint32_t service_mode_bench_median_us(void) { return s_bench_median_us; }
uint32_t service_mode_bench_p95_us(void) { return s_bench_p95_us; }
uint8_t service_mode_diag_page(void) { return s_diag_page; }

void service_mode_collect_note_sample(void)
{
    s_collect_samples++;
}

void service_mode_store_bench(uint32_t done, uint32_t median_us, uint32_t p95_us)
{
    s_bench_done = done;
    s_bench_median_us = median_us;
    s_bench_p95_us = p95_us;
}

/**
 * Pure mode-machine transition. Exposed via a shared header for tests through
 * the declarations below (used by service_mode_poll on target, by tests on host).
 */
void service_mode_step(bool key0_short, bool key1_short, bool key1_long)
{
    switch (s_mode) {
    case SVC_RUN:
        if (key0_short) {
            if (cycle_logic_acknowledge()) {
                printk("[SVC] DONE alert acknowledged\n");
            }
        }
        if (key1_long) {
            s_mode = SVC_DIAG;
            printk("[SVC] Diagnostics mode\n");
        }
        break;

    case SVC_DIAG:
        if (key0_short) {
            s_diag_page = (uint8_t)(s_diag_page ^ 1U);
        }
        if (key1_short || key1_long) {
            s_mode = SVC_COLLECT;
            s_collect_samples = 0;
            printk("# raw ICM-20608 counts @100 Hz: accel LSB(4096/g), gyro LSB(32.8 per deg/s); cols ax,ay,az,gx,gy,gz,label\n");
            printk("[SVC] COLLECT mode: KEY0 cycles label, stream CSV to PC\n");
        }
        break;

    case SVC_COLLECT:
        if (key0_short) {
            s_collect_label = (s_collect_label + 1) % 3;
            printk("[SVC] Label now %s\n",
                   s_collect_label == 0 ? "IDLE" : s_collect_label == 1 ? "WASH" : "SPIN");
        }
        if (key1_long) {
            s_mode = SVC_BENCH;
            printk("[SVC] BENCH mode: timing 50 live inferences\n");
        }
        break;

    case SVC_BENCH:
        if (key1_short || key1_long) {
            s_mode = SVC_RUN;
            printk("[SVC] Back to RUN (median %u us, p95 %u us)\n",
                   (unsigned)s_bench_median_us, (unsigned)s_bench_p95_us);
        }
        break;

    default:
        s_mode = SVC_RUN;
        break;
    }
}

void service_mode_poll(void)
{
#if defined(__arm__)
    const bool key0_pressed = (GPIOC->IDR & (1U << 0)) == 0U;
    const bool key1_pressed = (GPIOC->IDR & (1U << 1)) == 0U;

    const uint32_t prev1 = s_key1_down;
    s_key0_down = key0_pressed ? s_key0_down + 1U : 0U;
    s_key1_down = key1_pressed ? prev1 + 1U : 0U;

    /* KEY0 fires once on the debounced press edge */
    const bool key0_short = (s_key0_down == DEBOUNCE_POLLS);

    /* KEY1 fires on release: short tap, or one long-press event at the threshold */
    const bool key1_long = (key1_pressed && prev1 + 1U == LONG_PRESS_POLLS);
    const bool key1_short = (!key1_pressed && prev1 >= DEBOUNCE_POLLS && !s_key1_long_fired);

    if (key1_long) {
        s_key1_long_fired = true;
    }
    if (!key1_pressed) {
        s_key1_long_fired = false;
    }

    if (key0_short || key1_short || key1_long) {
        service_mode_step(key0_short, key1_short, key1_long);
    }
#endif
}
