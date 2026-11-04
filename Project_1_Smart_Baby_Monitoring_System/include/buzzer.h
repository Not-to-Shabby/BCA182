/**
 * @file buzzer.h
 * @brief TIM3 PWM alarm buzzer interface (RT-Spark onboard buzzer, PB0).
 *
 * Presentation/alert-output layer of the Smart Baby Monitoring System.
 * Raw-register port of the Laboratory 2 audio engine (hw_pwm_init /
 * hw_set_tone / hw_stop_tone) reduced to a single fixed 2 kHz alarm tone:
 * the monitor only ever needs "sound the alarm" / "silence", not melodies.
 */

#ifndef BUZZER_H_
#define BUZZER_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure TIM3 CH3 on PB0 (AF2) for PWM and leave the output silent.
 *
 * Counter clock = 1 MHz (PSC = 83 @ 84 MHz APB1 timer clock), period = 2 kHz
 * (ARR = 499). The channel is enabled but the compare value is 0, so the
 * output idles OFF (no clicks, no DC bias on the transducer).
 */
void buzzer_init(void);

/**
 * @brief Sound the fixed 2 kHz alarm tone while on, silence otherwise.
 *
 * Cheap and idempotent: repeated calls with the same value simply rewrite the
 * CCR3 compare register (a single store), so the UI/alert thread can call it
 * every cycle without bookkeeping.
 */
void buzzer_set(bool on);

#ifdef __cplusplus
}
#endif

#endif /* BUZZER_H_ */
