/**
 * @file threads.c
 * @brief Implementation of the 3 cooperative Zephyr RTOS threads for the
 *        Personal MP3 Player.
 *
 * Course: BCA182 Embedded Systems Programming
 * Laboratory Activity 2: Personal MP3 Player
 */

#include "threads.h"
#include <zephyr/sys/printk.h>

/* Global player context */
player_context_t g_player = {
    .state = PLAYER_STATE_STOPPED,
    .previous_state = PLAYER_STATE_STOPPED,
    .current_song_index = 0,
    .prospective_song_index = 0,
    .confirmation_start_ms = 0,
    .volume_percent = 50,
    .state_changed = true
};

/* Mutexes for exclusive resource access */
K_MUTEX_DEFINE(g_lcd_mutex);
K_MUTEX_DEFINE(g_player_mutex);

/* -------------------------------------------------------------------------- */
/* Thread 1: Update LCD and RGB LEDs                                          */
/* -------------------------------------------------------------------------- */
void update_lcd_leds_thread(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    printk("[LCD_LED_Thread] Started\n");

    while (1) {
        player_state_t current_state;
        uint8_t current_song;
        uint8_t prospective_song;
        uint8_t volume;
        uint32_t conf_start;

        /* Snapshot state under mutex */
        k_mutex_lock(&g_player_mutex, K_FOREVER);
        current_state = g_player.state;
        current_song = g_player.current_song_index;
        prospective_song = g_player.prospective_song_index;
        volume = g_player.volume_percent;
        conf_start = g_player.confirmation_start_ms;
        bool changed = g_player.state_changed;
        g_player.state_changed = false;
        k_mutex_unlock(&g_player_mutex);

        /* 1. Update RGB LED Indicators based on current state:
         *    - Playing: Blue LED ON
         *    - Paused / Stopped: Red LED ON
         *    - Confirming: Green LED ON
         */
        if (changed) {
            printk("[LED] State: %s | Red: %s | Green: %s | Blue: %s\n",
                   get_player_state_str(current_state),
                   (current_state == PLAYER_STATE_PAUSED || current_state == PLAYER_STATE_STOPPED) ? "ON" : "OFF",
                   (current_state == PLAYER_STATE_CONFIRMING) ? "ON" : "OFF",
                   (current_state == PLAYER_STATE_PLAYING) ? "ON" : "OFF");
        }

        /* 2. Mutex-protected LCD Display update */
        if (k_mutex_lock(&g_lcd_mutex, K_MSEC(50)) == 0) {
            if (current_state == PLAYER_STATE_CONFIRMING) {
                uint32_t now = k_uptime_get_32();
                uint32_t elapsed = now - conf_start;
                uint32_t remaining_s = (elapsed >= CONFIRMATION_TIMEOUT_MS) ? 0 :
                                       (CONFIRMATION_TIMEOUT_MS - elapsed) / 1000 + 1;
                const song_info_t *p_song = get_song_info(prospective_song);
                if (changed) {
                    printk("[LCD] === CONFIRM SONG SELECTION ===\n");
                    printk("[LCD] Song [%u]: %s %s\n", prospective_song, p_song->name1, p_song->name2);
                    printk("[LCD] Press Button 1 to confirm! Timeout: %u s\n", remaining_s);
                }
            } else {
                const song_info_t *c_song = get_song_info(current_song);
                if (changed) {
                    printk("[LCD] -------------------------------\n");
                    printk("[LCD] Status: %s | Vol: %u%%\n", get_player_state_str(current_state), volume);
                    printk("[LCD] Title:  %s %s\n", c_song->name1, c_song->name2);
                    printk("[LCD] Tempo:  %.2f | Length: %d notes\n", (double)c_song->tempo, c_song->length);
                    printk("[LCD] -------------------------------\n");
                }
            }
            k_mutex_unlock(&g_lcd_mutex);
        }

        /* Cooperative yield to let the scheduler run other threads */
        k_sleep(K_MSEC(LCD_LED_THREAD_PERIOD_MS));
    }
}

/* -------------------------------------------------------------------------- */
/* Thread 2: Polling Buttons                                                  */
/* -------------------------------------------------------------------------- */
void polling_buttons(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    printk("[Button_Thread] Started\n");

    while (1) {
        /* Check 5-second confirmation timeout if in CONFIRMING state */
        k_mutex_lock(&g_player_mutex, K_FOREVER);
        if (g_player.state == PLAYER_STATE_CONFIRMING) {
            uint32_t now = k_uptime_get_32();
            if (check_confirmation_timeout(g_player.confirmation_start_ms, now, CONFIRMATION_TIMEOUT_MS)) {
                /* Timeout expired: revert to previous state without changing song */
                printk("[Button_Thread] Confirmation timed out (5s elapsed). Reverting.\n");
                g_player.state = g_player.previous_state;
                g_player.state_changed = true;
            }
        }
        k_mutex_unlock(&g_player_mutex);

        /* Cooperative sleep */
        k_sleep(K_MSEC(BUTTON_POLL_PERIOD_MS));
    }
}

/* -------------------------------------------------------------------------- */
/* Thread 3: Adjust Volume                                                    */
/* -------------------------------------------------------------------------- */
void adjust_volume(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    printk("[Volume_Thread] Started\n");

    while (1) {
        /* Periodic volume sampling from ADC potentiometer */
        k_sleep(K_MSEC(VOLUME_THREAD_PERIOD_MS));
    }
}
