/**
 * @file led_matrix.h
 * @brief Onboard 19-LED SK6805 RGB LED Matrix driver for the RT-Spark.
 *        PA7 = serial data (800 kHz GRB), PF2 = matrix power enable (active LOW).
 *        Adapted from the verified Lab 2 side-quest driver for the appliance
 *        cycle monitor's state/alert rendering.
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

/** Configure PF2 (power, active LOW) and PA7 (data), then blank the matrix. */
void led_matrix_init(void);

/** Set one LED color (0..18). */
void led_matrix_set_pixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

/** Transmit the 19 GRB pixels to the strip (interrupts off for ~0.6 ms). */
void led_matrix_show(void);

/** Blank all LEDs and transmit. */
void led_matrix_clear(void);

/**
 * @brief Appliance status rendering (called by the UI thread at 4 Hz):
 *        LEDs 0..7  - vibration intensity bar (rms-gated, green->red)
 *        LED 8      - state color (IDLE dim blue / WASH cyan / SPIN orange / DONE green)
 *        LED 16     - DONE alert flash (bright white blink while alert_active)
 *        LEDs 17,18 - ambient context (temperature over 30 C -> warm tint, dark room -> dim)
 */
void led_matrix_update_appliance(int state, bool alert_active, float rms_g,
                                 bool hot_ambient, bool dark_room);

#ifdef __cplusplus
}
#endif

#endif /* LED_MATRIX_H_ */
