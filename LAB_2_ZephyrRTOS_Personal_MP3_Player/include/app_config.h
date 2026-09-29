/**
 * @file app_config.h
 * @brief Hardware configuration and peripheral pinouts for RT-Thread Spark Board
 *        (STM32F407ZGT6) running Zephyr RTOS.
 *
 * Course: BCA182 Embedded Systems Programming
 * Laboratory Activity 2: Personal MP3 Player
 */

#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* UART / Telemetry Configuration                                             */
/* -------------------------------------------------------------------------- */
#define APP_UART_BAUDRATE               115200
#define APP_BANNER_LINE                 "=====================================================\n"

/* -------------------------------------------------------------------------- */
/* Directional D-Pad Navigation Buttons (RT-Thread Spark Board Schematic)     */
/* -------------------------------------------------------------------------- */
/* UP Button: Scroll Track Forward (SW2 / GPIO_BTN_UP on PC5, Active LOW)     */
#define BTN_UP_PORT                     "GPIOC"
#define BTN_UP_PIN                      5

/* DOWN Button: Scroll Track Backward (SW4 / GPIO_BTN_DOWN on PC1, Active LOW) */
#define BTN_DOWN_PORT                   "GPIOC"
#define BTN_DOWN_PIN                    1

/* LEFT Button: Decrease Volume -5% (SW3 / GPIO_BTN_LEFT on PC0, Active LOW)  */
#define BTN_LEFT_PORT                   "GPIOC"
#define BTN_LEFT_PIN                    0

/* RIGHT Button: Increase Volume +5% (SW5 / GPIO_BTN_RIGHT on PC4, Active LOW)*/
#define BTN_RIGHT_PORT                  "GPIOC"
#define BTN_RIGHT_PIN                   4

/* PRESS / PLAY-PAUSE Button: Onboard USER / Wakeup Key (PA0, Active HIGH)   */
#define BTN_PRESS_PORT                  "GPIOA"
#define BTN_PRESS_PIN                   0

/* Optional Auxiliary 5th Button on PA1 (Active LOW with pull-up)             */
#define BTN_AUX_PORT                    "GPIOA"
#define BTN_AUX_PIN                     1

/* -------------------------------------------------------------------------- */
/* Volume Adjustment Settings                                                 */
/* -------------------------------------------------------------------------- */
#define VOLUME_STEP_PERCENT             5       /* 5% step per click          */
#define VOLUME_DEFAULT_PERCENT          70      /* Initial power-on volume    */

/* -------------------------------------------------------------------------- */
/* RGB LED Indicators                                                         */
/* -------------------------------------------------------------------------- */
/* Red LED: ON when audio is Paused or Stopped (Active LOW on RT-Spark)       */
#define LED_RED_PORT                    "GPIOF"
#define LED_RED_PIN                     12

/* Blue LED: ON when Song is actively Playing (Active LOW on RT-Spark)        */
#define LED_BLUE_PORT                   "GPIOF"
#define LED_BLUE_PIN                    11

/* Green LED: Optional indicator (PE3)                                        */
#define LED_GREEN_PORT                  "GPIOE"
#define LED_GREEN_PIN                   3

/* -------------------------------------------------------------------------- */
/* Timing Constraints                                                         */
/* -------------------------------------------------------------------------- */
#define BUTTON_DEBOUNCE_TIME_MS         20      /* 20 ms debounce filter      */
#define LCD_LED_THREAD_PERIOD_MS        100     /* 10 Hz refresh rate         */
#define BUTTON_POLL_PERIOD_MS           20      /* 50 Hz button poll rate     */
#define VOLUME_THREAD_PERIOD_MS         100     /* 10 Hz volume poll rate     */

/* -------------------------------------------------------------------------- */
/* Audio Player Specifications                                                */
/* -------------------------------------------------------------------------- */
#define TOTAL_PLAYABLE_SONGS            8       /* Tracks 1 to 8              */

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H_ */
