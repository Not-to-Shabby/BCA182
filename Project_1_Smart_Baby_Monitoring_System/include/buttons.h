/**
 * @file buttons.h
 * @brief Debounced KEY0 (PC0) / KEY1 (PC1) user-input interface.
 *
 * Presentation layer input handling for the Smart Baby Monitoring System.
 * Both keys are active-LOW with pull-ups on GPIOC (RT-Spark board keys),
 * raw-register modeled on the Laboratory 4 service_mode.c approach.
 */

#ifndef BUTTONS_H_
#define BUTTONS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure PC0 (KEY0) and PC1 (KEY1) as inputs with pull-ups.
 */
void buttons_init(void);

/**
 * @brief Poll both keys; call once per UI cycle (500 ms).
 *
 * Implements a 50 ms time-stamped debounce plus falling-edge (press)
 * detection internally. Raw level changes are timestamped with now_ms and a
 * level held stable for DEBOUNCE_MS is accepted; a debounce-confirmed press
 * fires its action exactly once per press.
 *
 *   KEY0 press -> acknowledge the current alert: baby_logic_acknowledge(now)
 *                 and mute_until_ms = now + ALERT_MUTE_MS (state lock held),
 *                 console "[UI] Alert acknowledged (mute 60 s)".
 *   KEY1 press -> toggle monitor_state.test_mode (UI-level demo alert; see
 *                 buttons.c), console "[UI] Test alert ON/OFF".
 *
 * Note: with a 500 ms polling cycle the worst-case press latency is one
 * cycle; buttons shorter than one poll interval cannot be seen.
 */
void buttons_poll(uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* BUTTONS_H_ */
