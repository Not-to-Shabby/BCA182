/**
 * @file led_matrix.c
 * @brief 19-LED SK6805 driver + appliance status renderer.
 *        Timing core adapted from the verified Lab 2 side-quest driver
 *        (DWT cycle-counted WS2812/SK6805 800 kHz protocol).
 */

#include "led_matrix.h"
#include <string.h>

#if defined(__arm__)
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <stm32f4xx.h>
#endif

/* Cap per-LED brightness to keep the 19-LED matrix within the board's power budget */
#define LED_MAX_BRIGHTNESS 64U

static led_color_t s_pixels[LED_MATRIX_NUM_LEDS];

void led_matrix_init(void)
{
#if defined(__arm__)
    RCC->AHB1ENR |= (RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOFEN);

    /* PF2: push-pull output, high speed, driven LOW = power ON */
    GPIOF->MODER = (GPIOF->MODER & ~(3U << 4)) | (1U << 4);
    GPIOF->OTYPER &= ~(1U << 2);
    GPIOF->OSPEEDR |= (3U << 4);
    GPIOF->PUPDR &= ~(3U << 4);
    GPIOF->BSRR = (1U << (2 + 16));

    /* PA7: push-pull output, idle LOW */
    GPIOA->MODER = (GPIOA->MODER & ~(3U << 14)) | (1U << 14);
    GPIOA->OTYPER &= ~(1U << 7);
    GPIOA->OSPEEDR |= (3U << 14);
    GPIOA->PUPDR &= ~(3U << 14);
    GPIOA->BSRR = (1U << (7 + 16));

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
#endif

    led_matrix_clear();
}

void led_matrix_set_pixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index < LED_MATRIX_NUM_LEDS) {
        s_pixels[index].r = r;
        s_pixels[index].g = g;
        s_pixels[index].b = b;
    }
}

void led_matrix_show(void)
{
#if defined(__arm__)
    uint32_t clk = SystemCoreClock;
    /* SK6805 800 kHz: 1250 ns period, ~350 ns zero-high, ~800 ns one-high */
    uint32_t t_total = (uint32_t)(((uint64_t)clk * 1250ULL) / 1000000000ULL);
    uint32_t t0h = (uint32_t)(((uint64_t)clk * 350ULL) / 1000000000ULL);
    uint32_t t1h = (uint32_t)(((uint64_t)clk * 800ULL) / 1000000000ULL);

    unsigned int key = irq_lock();

    for (uint32_t i = 0; i < LED_MATRIX_NUM_LEDS; i++) {
        uint8_t grb[3];
        grb[0] = s_pixels[i].g;
        grb[1] = s_pixels[i].r;
        grb[2] = s_pixels[i].b;

        for (int byte_idx = 0; byte_idx < 3; byte_idx++) {
            uint8_t b = grb[byte_idx];
            for (int bit = 7; bit >= 0; bit--) {
                uint32_t th = (b & (1U << bit)) ? t1h : t0h;
                uint32_t t0 = DWT->CYCCNT;

                GPIOA->BSRR = (1U << 7);
                while ((uint32_t)(DWT->CYCCNT - t0) < th) {
                }
                GPIOA->BSRR = (1U << (7 + 16));
                while ((uint32_t)(DWT->CYCCNT - t0) < t_total) {
                }
            }
        }
    }

    irq_unlock(key);
#endif
}

void led_matrix_clear(void)
{
    memset(s_pixels, 0, sizeof(s_pixels));
    led_matrix_show();
}

static uint8_t clamp_level(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return (uint8_t)(v * (float)LED_MAX_BRIGHTNESS);
}

void led_matrix_update_appliance(int state, bool alert_active, int alert_reason,
                                 float rms_g, bool hot_ambient, bool dark_room)
{
    uint8_t dim = dark_room ? 3U : 1U;

    /* LEDs 0..7: vibration intensity bar, 0.05 g maps to full */
    for (uint8_t i = 0; i < 8; i++) {
        float level = rms_g * 20.0f - (float)i * 0.125f;
        if (level > 0.0f) {
            uint8_t b = clamp_level(level);
            led_matrix_set_pixel(i, b, b / 2U, 0U); /* green->orange ramp */
        } else {
            led_matrix_set_pixel(i, 0, 0, 0);
        }
    }

    /* LED 8: product state color */
    switch (state) {
    case 1:  led_matrix_set_pixel(8, 0, (uint8_t)(LED_MAX_BRIGHTNESS / dim), (uint8_t)(LED_MAX_BRIGHTNESS / dim)); break; /* WASH cyan */
    case 2:  led_matrix_set_pixel(8, (uint8_t)(LED_MAX_BRIGHTNESS / dim), (uint8_t)(LED_MAX_BRIGHTNESS / 2U / dim), 0); break; /* SPIN orange */
    case 3:  led_matrix_set_pixel(8, 0, LED_MAX_BRIGHTNESS, 0); break; /* DONE green */
    default: led_matrix_set_pixel(8, 0, 0, (uint8_t)(8U / dim)); break; /* IDLE dim blue */
    }

    /* LED 16: alert flash tinted by which tub finished (twin-tub machine) */
    if (alert_active) {
        if (alert_reason == 1) {          /* wash tub: cyan flash */
            led_matrix_set_pixel(16, 0, LED_MAX_BRIGHTNESS, LED_MAX_BRIGHTNESS);
        } else if (alert_reason == 2) {   /* spin tub: orange flash */
            led_matrix_set_pixel(16, LED_MAX_BRIGHTNESS, LED_MAX_BRIGHTNESS / 2U, 0);
        } else {
            led_matrix_set_pixel(16, LED_MAX_BRIGHTNESS, LED_MAX_BRIGHTNESS, LED_MAX_BRIGHTNESS);
        }
    } else {
        led_matrix_set_pixel(16, 0, 0, 0);
    }

    /* LEDs 17/18: ambient context */
    led_matrix_set_pixel(17, hot_ambient ? 24U : 0U, 0U, 0U);
    led_matrix_set_pixel(18, 0U, 0U, dark_room ? 24U : 0U);

    led_matrix_show();
}
