/**
 * @file led_matrix.c
 * @brief Onboard 19-LED SK6805 RGB LED Matrix driver for RT-Thread RT-Spark.
 *        - PF2: Matrix power enable (Active LOW, pull-down to turn on power)
 *        - PA7: SK6805 serial data line (800 kHz GRB888 protocol)
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include "led_matrix.h"
#include "yamaha_fm_synth.h"
#include <string.h>

#if defined(__arm__)
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <stm32f4xx.h>
#endif

/* Maximum per-channel LED brightness (64 out of 255 keeps colors vibrant without power droop) */
#define LED_MAX_BRIGHTNESS  64U

static led_color_t s_pixels[LED_MATRIX_NUM_LEDS];
static uint8_t s_smooth_levels[16];

void led_matrix_init(void)
{
#if defined(__arm__)
    /* 1. Enable GPIOA and GPIOF peripheral clocks */
    RCC->AHB1ENR |= (RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOFEN);

    /* 2. Configure PF2 as GPIO Output, Push-Pull, High-Speed */
    GPIOF->MODER = (GPIOF->MODER & ~(3U << 4)) | (1U << 4); /* Output mode */
    GPIOF->OTYPER &= ~(1U << 2);                           /* Push-Pull */
    GPIOF->OSPEEDR |= (3U << 4);                           /* High speed */
    GPIOF->PUPDR &= ~(3U << 4);                            /* No pull */

    /* Enable power to the matrix (PF2 active LOW) */
    GPIOF->BSRR = (1U << (2 + 16));

    /* 3. Configure PA7 as GPIO Output, Push-Pull, High-Speed (Low at idle) */
    GPIOA->MODER = (GPIOA->MODER & ~(3U << 14)) | (1U << 14); /* Output mode */
    GPIOA->OTYPER &= ~(1U << 7);                              /* Push-Pull */
    GPIOA->OSPEEDR |= (3U << 14);                             /* Very High speed */
    GPIOA->PUPDR &= ~(3U << 14);
    GPIOA->BSRR = (1U << (7 + 16));                           /* Start LOW */

    /* 4. Ensure DWT cycle counter is running */
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
    /* SK6805 / WS2812B 800 kHz timing (1.25 us bit period) */
    uint32_t t_total = (uint32_t)(((uint64_t)clk * 1250ULL) / 1000000ULL);
    uint32_t t0h     = (uint32_t)(((uint64_t)clk * 350ULL)  / 1000000ULL);
    uint32_t t1h     = (uint32_t)(((uint64_t)clk * 800ULL)  / 1000000ULL);

    unsigned int key = irq_lock();

    for (uint32_t i = 0; i < LED_MATRIX_NUM_LEDS; i++) {
        /* SK6805 expects GRB order */
        uint8_t grb[3];
        grb[0] = s_pixels[i].g;
        grb[1] = s_pixels[i].r;
        grb[2] = s_pixels[i].b;

        for (int byte_idx = 0; byte_idx < 3; byte_idx++) {
            uint8_t b = grb[byte_idx];
            for (int bit = 7; bit >= 0; bit--) {
                uint32_t th = (b & (1U << bit)) ? t1h : t0h;
                uint32_t t0 = DWT->CYCCNT;

                GPIOA->BSRR = (1U << 7); /* PA7 HIGH */
                while ((uint32_t)(DWT->CYCCNT - t0) < th) {
                }
                GPIOA->BSRR = (1U << (7 + 16)); /* PA7 LOW */
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

/* Returns the signature RGB hue for each General MIDI program family */
static led_color_t get_instrument_color(uint8_t prog, uint8_t ch, uint8_t drum_note)
{
    led_color_t c = {0, 0, 0};

    if (ch == 9) {
        /* MIDI Channel 10: Dynamic Drum Color based on hit type */
        if (drum_note == 35 || drum_note == 36) {
            /* Bass Drum / Kick: Solid Red */
            c.r = 255; c.g = 10; c.b = 10;
        } else if (drum_note == 38 || drum_note == 40 || drum_note == 39) {
            /* Snare / Clap: Brilliant Warm White */
            c.r = 255; c.g = 240; c.b = 180;
        } else if (drum_note == 42 || drum_note == 44 || drum_note == 46) {
            /* Hi-Hats: Electric Cyan */
            c.r = 20; c.g = 240; c.b = 255;
        } else if (drum_note == 49 || drum_note == 51 || drum_note == 52 ||
                   drum_note == 53 || drum_note == 55 || drum_note == 57 || drum_note == 59) {
            /* Cymbals (Crash / Ride): Gold */
            c.r = 255; c.g = 200; c.b = 20;
        } else {
            /* Toms / Percussion: Magenta */
            c.r = 240; c.g = 20; c.b = 255;
        }
        return c;
    }

    /* Melodic Programs (0..127) */
    if (prog <= 7) {
        /* Pianos: Warm Gold */
        c.r = 255; c.g = 175; c.b = 15;
    } else if (prog <= 15) {
        /* Chromatic Percussion (Vibes/Marimba/Celesta): Ice Cyan */
        c.r = 20; c.g = 220; c.b = 255;
    } else if (prog <= 23) {
        /* Organs: Warm Amber / Orange */
        c.r = 255; c.g = 85; c.b = 0;
    } else if (prog <= 28) {
        /* Acoustic & Clean Guitars: Emerald Green */
        c.r = 20; c.g = 255; c.b = 60;
    } else if (prog <= 31) {
        /* Overdriven & Distortion Guitars (DOOM!): Crimson Fire */
        c.r = 255; c.g = 15; c.b = 5;
    } else if (prog <= 39) {
        /* Basses: Royal Electric Blue */
        c.r = 20; c.g = 60; c.b = 255;
    } else if (prog <= 55) {
        /* Strings & Choir: Deep Violet / Purple */
        c.r = 180; c.g = 20; c.b = 255;
    } else if (prog <= 63) {
        /* Brass: Brass Yellow-Gold */
        c.r = 255; c.g = 220; c.b = 0;
    } else if (prog <= 79) {
        /* Woodwinds / Sax / Flute: Fresh Mint / Aqua */
        c.r = 10; c.g = 255; c.b = 150;
    } else if (prog <= 95) {
        /* Synth Leads & Pads: Hot Magenta / Pink */
        c.r = 255; c.g = 20; c.b = 180;
    } else {
        /* SFX / Ethnic: Lime Green */
        c.r = 130; c.g = 255; c.b = 10;
    }

    return c;
}

void led_matrix_update_instruments(int8_t melody_channel, int16_t peak_l, int16_t peak_r)
{
    uint8_t progs[16];
    uint8_t raw_levels[16];
    uint8_t drum_note = 36;

    yamaha_fm_get_channel_activity(progs, raw_levels, &drum_note);

    /* 1. LEDs 0..15: 16 MIDI Channels */
    for (uint8_t ch = 0; ch < 16; ch++) {
        uint8_t lvl = raw_levels[ch];
        /* Fast attack, smooth decay for visual persistence */
        if (lvl > s_smooth_levels[ch]) {
            s_smooth_levels[ch] = lvl;
        } else {
            s_smooth_levels[ch] = (uint8_t)(((uint32_t)s_smooth_levels[ch] * 78U) / 100U);
        }

        uint32_t bri = ((uint32_t)s_smooth_levels[ch] * LED_MAX_BRIGHTNESS) / 255U;
        if (bri > LED_MAX_BRIGHTNESS) bri = LED_MAX_BRIGHTNESS;

        if (bri == 0) {
            led_matrix_set_pixel(ch, 0, 0, 0);
        } else {
            led_color_t base = get_instrument_color(progs[ch], ch, drum_note);
            uint8_t r = (uint8_t)(((uint32_t)base.r * bri) / 255U);
            uint8_t g = (uint8_t)(((uint32_t)base.g * bri) / 255U);
            uint8_t b = (uint8_t)(((uint32_t)base.b * bri) / 255U);
            led_matrix_set_pixel(ch, r, g, b);
        }
    }

    /* 2. LED 16: Active Lead Melody / Singing Indicator */
    if (melody_channel >= 0 && melody_channel < 16) {
        uint32_t mel_bri = ((uint32_t)s_smooth_levels[melody_channel] * LED_MAX_BRIGHTNESS) / 255U;
        if (mel_bri > 0) {
            /* Hot White-Pink singing glow */
            led_matrix_set_pixel(16, (uint8_t)mel_bri, (uint8_t)(mel_bri / 2U), (uint8_t)mel_bri);
        } else {
            /* Dim pilot light */
            led_matrix_set_pixel(16, 2, 0, 2);
        }
    } else {
        led_matrix_set_pixel(16, 0, 0, 0);
    }

    /* 3. LED 17 & 18: Stereo L / R VU Peak Level Indicators (Green -> Yellow -> Red) */
    int vu_l = (int)(((int32_t)peak_l * 255) / 32767);
    int vu_r = (int)(((int32_t)peak_r * 255) / 32767);
    if (vu_l < 0) {
        vu_l = 0;
    } else if (vu_l > 255) {
        vu_l = 255;
    }
    if (vu_r < 0) {
        vu_r = 0;
    } else if (vu_r > 255) {
        vu_r = 255;
    }

    const uint8_t vus[2] = { (uint8_t)vu_l, (uint8_t)vu_r };
    for (int k = 0; k < 2; k++) {
        uint8_t v = vus[k];
        uint8_t r = 0, g = 0, b = 0;
        if (v > 200) {
            /* Peak: Bright Red */
            r = LED_MAX_BRIGHTNESS; g = 0; b = 0;
        } else if (v > 120) {
            /* High: Yellow */
            r = (uint8_t)(LED_MAX_BRIGHTNESS * 3U / 4U);
            g = (uint8_t)(LED_MAX_BRIGHTNESS * 3U / 4U);
        } else if (v > 30) {
            /* Normal: Green */
            g = (uint8_t)(LED_MAX_BRIGHTNESS * (uint32_t)v / 180U);
        }
        led_matrix_set_pixel((uint8_t)(17 + k), r, g, b);
    }

    /* Transmit the updated frame to the SK6805 strip */
    led_matrix_show();
}
