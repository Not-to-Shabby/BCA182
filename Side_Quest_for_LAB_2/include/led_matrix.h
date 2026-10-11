/**
 * @file led_matrix.h
 * @brief Onboard 19-LED SK6805 RGB LED Matrix driver for RT-Thread RT-Spark.
 *        Connected to PA7 (serial data) and PF2 (active-low matrix power enable).
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#ifndef LED_MATRIX_H_
#define LED_MATRIX_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LED_MATRIX_NUM_LEDS 19U

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} led_color_t;

/**
 * @brief Initialize GPIOs for onboard LED matrix: PF2 (Power Enable, Active LOW), PA7 (Data Out).
 */
void led_matrix_init(void);

/**
 * @brief Set color of a specific LED (0 to 18).
 */
void led_matrix_set_pixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Transmit the 19 RGB pixels to the SK6805 strip.
 */
void led_matrix_show(void);

/**
 * @brief Clear all 19 LEDs to off/dark.
 */
void led_matrix_clear(void);

/**
 * @brief Update the 19 LEDs dynamically corresponding to active MIDI instruments:
 *        - LEDs 0..15: 16 MIDI Channels (Color = Instrument Family, Brightness = Channel Amplitude)
 *        - LED 16: Singing Lead Melody Channel indicator (Bright Magenta/White pulse)
 *        - LED 17: Left Audio Peak VU meter
 *        - LED 18: Right Audio Peak VU meter
 */
void led_matrix_update_instruments(int8_t melody_channel, int16_t peak_l, int16_t peak_r);

#ifdef __cplusplus
}
#endif

#endif /* LED_MATRIX_H_ */
