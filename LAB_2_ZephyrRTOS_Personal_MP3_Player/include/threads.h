/**
 * @file threads.h
 * @brief Thread entry points, stack sizes, and synchronization primitives
 *        for the 3-thread Zephyr RTOS Personal MP3 Player.
 *
 * Course: BCA182 Embedded Systems Programming
 * Laboratory Activity 2: Personal MP3 Player
 */

#ifndef THREADS_H_
#define THREADS_H_

#include <zephyr/kernel.h>
#include "app_config.h"
#include "player_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Thread Stack Sizes & Priorities                                            */
/* -------------------------------------------------------------------------- */
#define THREAD_STACK_SIZE_LCD_LEDS      2048
#define THREAD_STACK_SIZE_BUTTONS       1024
#define THREAD_STACK_SIZE_VOLUME        1024

#define THREAD_PRIORITY_LCD_LEDS        3
#define THREAD_PRIORITY_BUTTONS         2
#define THREAD_PRIORITY_VOLUME          3

/* -------------------------------------------------------------------------- */
/* Shared Player System State (Protected by IPC primitives)                   */
/* -------------------------------------------------------------------------- */
typedef struct {
    player_state_t state;
    player_state_t previous_state;
    uint8_t current_song_index;
    uint8_t prospective_song_index;
    uint32_t confirmation_start_ms;
    uint8_t volume_percent;
    bool state_changed;
} player_context_t;

extern player_context_t g_player;
extern struct k_mutex g_lcd_mutex;
extern struct k_mutex g_player_mutex;

/* -------------------------------------------------------------------------- */
/* Peripheral & Thread Function Prototypes                                    */
/* -------------------------------------------------------------------------- */
/**
 * @brief Initialize button GPIOs, status LEDs, and ADC1 for potentiometer.
 */
void init_player_peripherals(void);

/**
 * @brief Thread 1: Updates LCD display and RGB status LEDs.
 */
void update_lcd_leds_thread(void *arg1, void *arg2, void *arg3);

/**
 * @brief Thread 2: Polls buttons 1-4 and USER_BUTTON, handles binary selection
 *                  and 5-second confirmation window.
 */
void polling_buttons(void *arg1, void *arg2, void *arg3);

/**
 * @brief Thread 3: Samples ADC potentiometer and updates volume.
 */
void adjust_volume(void *arg1, void *arg2, void *arg3);

#ifdef __cplusplus
}
#endif

#endif /* THREADS_H_ */
