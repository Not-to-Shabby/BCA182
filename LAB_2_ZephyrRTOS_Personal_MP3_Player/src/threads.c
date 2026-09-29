/**
 * @file threads.c
 * @brief Implementation of the 3 cooperative Zephyr RTOS threads for the
 *        Personal MP3 Player with ST7789 LCD graphics and GPIO LED control.
 *
 * Course: BCA182 Embedded Systems Programming
 * Laboratory Activity 2: Personal MP3 Player
 */

#include "threads.h"
#include "lcd_st7789.h"
#include <zephyr/sys/printk.h>
#include <stm32f4xx.h>
#include <stdio.h>

/* Global player context */
player_context_t g_player = {
    .state = PLAYER_STATE_STOPPED,
    .previous_state = PLAYER_STATE_STOPPED,
    .current_song_index = 0,
    .prospective_song_index = 0,
    .confirmation_start_ms = 0,
    .volume_percent = 70,
    .state_changed = true
};

/* Mutexes for exclusive resource access */
K_MUTEX_DEFINE(g_lcd_mutex);
K_MUTEX_DEFINE(g_player_mutex);

/* -------------------------------------------------------------------------- */
/* Physical GPIO LED Control for RT-Thread Spark Board                        */
/* -------------------------------------------------------------------------- */
static void init_status_leds(void)
{
    /* Enable GPIOF clock (PF11 Blue, PF12 Red) */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOFEN | RCC_AHB1ENR_GPIOEEN;

    /* Configure PF11 (Blue) and PF12 (Red) as Push-Pull Outputs */
    GPIOF->MODER = (GPIOF->MODER & ~((3U << 22) | (3U << 24))) |
                   ((1U << 22) | (1U << 24));
    GPIOF->OTYPER &= ~((1U << 11) | (1U << 12));
    GPIOF->OSPEEDR |= ((3U << 22) | (3U << 24));
    GPIOF->PUPDR &= ~((3U << 22) | (3U << 24));

    /* Configure PE3 (Green LED) as Push-Pull Output */
    GPIOE->MODER = (GPIOE->MODER & ~(3U << 6)) | (1U << 6);
    GPIOE->OTYPER &= ~(1U << 3);
    GPIOE->OSPEEDR |= (3U << 6);
    GPIOE->PUPDR &= ~(3U << 6);

    /* Turn all off initially (Active Low on RT-Spark onboard LEDs) */
    GPIOF->BSRR = (1U << 11) | (1U << 12);
    GPIOE->BSRR = (1U << (3 + 16));
}

static void update_status_leds(player_state_t state)
{
    /* Onboard LEDs on RT-Spark are active LOW for PF11 and PF12 */
    if (state == PLAYER_STATE_PLAYING) {
        /* Blue LED ON, Red OFF, Green OFF */
        GPIOF->BSRR = (1U << (11 + 16)); /* Blue ON (Low) */
        GPIOF->BSRR = (1U << 12);        /* Red OFF (High) */
        GPIOE->BSRR = (1U << (3 + 16));  /* Green OFF (Low) */
    } else if (state == PLAYER_STATE_PAUSED || state == PLAYER_STATE_STOPPED) {
        /* Red LED ON, Blue OFF, Green OFF */
        GPIOF->BSRR = (1U << 11);        /* Blue OFF (High) */
        GPIOF->BSRR = (1U << (12 + 16)); /* Red ON (Low) */
        GPIOE->BSRR = (1U << (3 + 16));  /* Green OFF (Low) */
    } else if (state == PLAYER_STATE_CONFIRMING) {
        /* Green LED ON, Red OFF, Blue OFF */
        GPIOF->BSRR = (1U << 11);        /* Blue OFF (High) */
        GPIOF->BSRR = (1U << 12);        /* Red OFF (High) */
        GPIOE->BSRR = (1U << 3);         /* Green ON (High) */
    }
}

/* -------------------------------------------------------------------------- */
/* ST7789 Graphical User Interface Rendering                                  */
/* -------------------------------------------------------------------------- */
static void render_lcd_screen(player_state_t state, uint8_t cur_song_idx,
                              uint8_t prop_song_idx, uint8_t volume, uint32_t conf_start)
{
    char buf[32];

    /* 1. Top Header Banner */
    lcd_fill_rect(0, 0, LCD_WIDTH - 1, 26, LCD_COLOR_NAVY);
    lcd_show_string(16, 6, "BCA182: MP3 PLAYER", LCD_COLOR_WHITE, LCD_COLOR_NAVY);
    lcd_draw_line(0, 27, LCD_WIDTH - 1, 27, LCD_COLOR_CYAN);

    if (state == PLAYER_STATE_CONFIRMING) {
        /* Confirmation Dialog Window */
        uint32_t now = k_uptime_get_32();
        uint32_t elapsed = now - conf_start;
        uint32_t rem_s = (elapsed >= CONFIRMATION_TIMEOUT_MS) ? 0 :
                         ((CONFIRMATION_TIMEOUT_MS - elapsed) / 1000 + 1);

        lcd_fill_rect(10, 38, LCD_WIDTH - 11, 200, LCD_COLOR_DARKGREY);
        lcd_draw_rect(10, 38, LCD_WIDTH - 11, 200, LCD_COLOR_YELLOW);

        lcd_show_string(24, 48, "** CONFIRM SONG **", LCD_COLOR_YELLOW, LCD_COLOR_DARKGREY);

        const song_info_t *p_song = get_song_info(prop_song_idx);
        snprintf(buf, sizeof(buf), "Track [%u/8]:", prop_song_idx + 1);
        lcd_show_string(20, 75, buf, LCD_COLOR_WHITE, LCD_COLOR_DARKGREY);

        lcd_show_string(20, 95, p_song->name1, LCD_COLOR_CYAN, LCD_COLOR_DARKGREY);
        lcd_show_string(20, 115, p_song->name2, LCD_COLOR_CYAN, LCD_COLOR_DARKGREY);

        snprintf(buf, sizeof(buf), "Press B1 to Confirm!");
        lcd_show_string(20, 145, buf, LCD_COLOR_GREEN, LCD_COLOR_DARKGREY);

        snprintf(buf, sizeof(buf), "Timeout in: %u s", rem_s);
        lcd_show_string(20, 170, buf, LCD_COLOR_ORANGE, LCD_COLOR_DARKGREY);
    } else {
        /* Normal Playback View */
        lcd_fill_rect(0, 28, LCD_WIDTH - 1, LCD_HEIGHT - 1, LCD_COLOR_BLACK);

        /* Status Badge */
        uint16_t status_color = (state == PLAYER_STATE_PLAYING) ? LCD_COLOR_GREEN :
                                (state == PLAYER_STATE_PAUSED)  ? LCD_COLOR_YELLOW :
                                                                  LCD_COLOR_RED;
        snprintf(buf, sizeof(buf), "STATUS: %s", get_player_state_str(state));
        lcd_show_string(14, 36, buf, status_color, LCD_COLOR_BLACK);

        /* Song Info */
        const song_info_t *c_song = get_song_info(cur_song_idx);
        snprintf(buf, sizeof(buf), "Track #%u of 8", cur_song_idx + 1);
        lcd_show_string(14, 58, buf, LCD_COLOR_GRAY, LCD_COLOR_BLACK);

        lcd_show_string(14, 80, "TITLE:", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
        lcd_show_string(14, 98, c_song->name1, LCD_COLOR_CYAN, LCD_COLOR_BLACK);
        lcd_show_string(14, 118, c_song->name2, LCD_COLOR_CYAN, LCD_COLOR_BLACK);

        /* Volume Bar */
        snprintf(buf, sizeof(buf), "VOLUME: %u%%", volume);
        lcd_show_string(14, 146, buf, LCD_COLOR_WHITE, LCD_COLOR_BLACK);
        lcd_draw_rect(14, 166, 226, 180, LCD_COLOR_WHITE);
        uint16_t bar_width = (uint16_t)((volume * 210U) / 100U);
        if (bar_width > 0) {
            lcd_fill_rect(15, 167, 15 + bar_width, 179, LCD_COLOR_GREEN);
        }
        if (bar_width < 210) {
            lcd_fill_rect(15 + bar_width + 1, 167, 225, 179, LCD_COLOR_BLACK);
        }

        /* Footer Controls Guide */
        lcd_draw_line(0, 192, LCD_WIDTH - 1, 192, LCD_COLOR_DARKGREY);
        lcd_show_string(10, 200, "B2-B4: Select Binary", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
        lcd_show_string(10, 218, "B1: Latch | USER: P/P", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
    }
}

/* -------------------------------------------------------------------------- */
/* Thread 1: Update LCD and RGB LEDs                                          */
/* -------------------------------------------------------------------------- */
void update_lcd_leds_thread(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    printk("[LCD_LED_Thread] Started\n");
    init_status_leds();

    uint8_t last_rendered_song = 0xFF;
    player_state_t last_rendered_state = (player_state_t)0xFF;
    uint8_t last_rendered_vol = 0xFF;

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
        bool force_redraw = g_player.state_changed;
        g_player.state_changed = false;
        k_mutex_unlock(&g_player_mutex);

        /* 1. Update physical RGB LEDs */
        update_status_leds(current_state);

        /* 2. Redraw LCD display if state, track, or volume changed, or if confirming */
        bool needs_redraw = force_redraw ||
                            (current_state != last_rendered_state) ||
                            (current_song != last_rendered_song) ||
                            (volume != last_rendered_vol) ||
                            (current_state == PLAYER_STATE_CONFIRMING);

        if (needs_redraw) {
            if (k_mutex_lock(&g_lcd_mutex, K_MSEC(50)) == 0) {
                render_lcd_screen(current_state, current_song, prospective_song, volume, conf_start);
                last_rendered_state = current_state;
                last_rendered_song = current_song;
                last_rendered_vol = volume;
                k_mutex_unlock(&g_lcd_mutex);
            }
        }

        /* Cooperative sleep */
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
