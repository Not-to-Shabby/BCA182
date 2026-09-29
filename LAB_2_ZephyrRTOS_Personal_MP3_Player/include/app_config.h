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
/* On-Board / External GPIO Push Buttons                                      */
/* -------------------------------------------------------------------------- */
/* Button 1: Selection latch & Confirmation (Active Low with Pull-Up)        */
#define BUTTON_1_PORT                   "GPIOC"
#define BUTTON_1_PIN                    0       /* KEY0 on RT-Spark */

/* Button 2: Binary Bit 0 (LSB)                                              */
#define BUTTON_2_PORT                   "GPIOC"
#define BUTTON_2_PIN                    1       /* KEY1 on RT-Spark */

/* Button 3: Binary Bit 1                                                     */
#define BUTTON_3_PORT                   "GPIOC"
#define BUTTON_3_PIN                    4       /* KEY2 on RT-Spark */

/* Button 4: Binary Bit 2 (MSB)                                               */
#define BUTTON_4_PORT                   "GPIOC"
#define BUTTON_4_PIN                    5       /* WK_UP on RT-Spark */

/* On-board USER_BUTTON: Play / Pause / Replay toggle                         */
#define USER_BUTTON_PORT                "GPIOA"
#define USER_BUTTON_PIN                 0

/* -------------------------------------------------------------------------- */
/* RGB LED Indicators                                                         */
/* -------------------------------------------------------------------------- */
/* Red LED: ON when audio is Paused or Stopped                                */
#define LED_RED_PORT                    "GPIOF"
#define LED_RED_PIN                     12      /* On-board Red LED */

/* Green LED: ON when in Song Confirmation Window (5-second timeout)         */
#define LED_GREEN_PORT                  "GPIOE"
#define LED_GREEN_PIN                   3       /* Expansion / external LED */

/* Blue LED: ON when Song is actively Playing                                 */
#define LED_BLUE_PORT                   "GPIOF"
#define LED_BLUE_PIN                    11      /* On-board Blue LED */

/* -------------------------------------------------------------------------- */
/* Audio & Volume ADC Configuration                                           */
/* -------------------------------------------------------------------------- */
#define POTENTIOMETER_ADC_PORT          "GPIOA"
#define POTENTIOMETER_ADC_PIN           1       /* ADC1 Channel 1 */
#define ADC_RESOLUTION_BITS             12
#define ADC_MAX_RAW_VALUE               4095

/* -------------------------------------------------------------------------- */
/* Timing Constraints                                                         */
/* -------------------------------------------------------------------------- */
#define CONFIRMATION_TIMEOUT_MS         5000    /* 5 seconds confirmation window */
#define BUTTON_DEBOUNCE_TIME_MS         20      /* 20 ms debounce filter */
#define LCD_LED_THREAD_PERIOD_MS        100     /* 10 Hz refresh rate */
#define BUTTON_POLL_PERIOD_MS           20      /* 50 Hz poll rate */
#define VOLUME_THREAD_PERIOD_MS         100     /* 10 Hz poll rate */

/* -------------------------------------------------------------------------- */
/* Audio Player Specifications                                                */
/* -------------------------------------------------------------------------- */
#define TOTAL_PLAYABLE_SONGS            8       /* Binary indexed: 000 to 111 */

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H_ */
