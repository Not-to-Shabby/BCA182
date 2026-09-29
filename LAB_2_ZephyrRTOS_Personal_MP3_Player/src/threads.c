/**
 * @file threads.c
 * @brief Implementation of the 3 cooperative Zephyr RTOS threads for the
 *        Personal MP3 Player with ST7789 LCD graphics, physical GPIO button
 *        scanning, binary selection gesture, 5s confirmation, and button-driven volume.
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
/* Physical GPIO Hardware Configuration (LEDs, Buttons)                       */
/* -------------------------------------------------------------------------- */
static void init_hardware_peripherals(void)
{
    /* 1. Enable Clocks for GPIOA, GPIOC, GPIOE, GPIOF */
    RCC->AHB1ENR |= (RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOCEN |
                     RCC_AHB1ENR_GPIOEEN | RCC_AHB1ENR_GPIOFEN);

    /* 2. Configure LEDs: PF11 (Blue) and PF12 (Red) as Push-Pull Outputs */
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

    /* Turn all LEDs OFF initially (Onboard PF11/PF12 are active LOW on RT-Spark) */
    GPIOF->BSRR = (1U << 11) | (1U << 12);
    GPIOE->BSRR = (1U << (3 + 16));

    /* 3. Configure Buttons on GPIOC:
     *    PC0 (Button 1 / KEY0: Latch & Confirm),
     *    PC1 (Button 2 / KEY1: Bit 0 / Vol -),
     *    PC4 (Button 3 / KEY2: Bit 1 / Vol +),
     *    PC5 (Button 4 / WK_UP: Bit 2)
     *    Mode: Input (00b), Pull-Up (01b) -> Active LOW */
    static const uint8_t c_btn_pins[] = {0, 1, 4, 5};
    for (uint32_t i = 0; i < sizeof(c_btn_pins); i++) {
        uint32_t pin = c_btn_pins[i];
        GPIOC->MODER &= ~(3U << (pin * 2));
        GPIOC->PUPDR = (GPIOC->PUPDR & ~(3U << (pin * 2))) | (1U << (pin * 2));
    }

    /* 4. Configure USER_BUTTON on GPIOA:
     *    PA0 (USER_BUTTON / Wakeup)
     *    Mode: Input (00b), Pull-Down (10b) -> Active HIGH on onboard key */
    GPIOA->MODER &= ~(3U << 0);
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(3U << 0)) | (2U << 0);

    /* 5. Configure Optional 5th Button on PA1:
     *    PA1 (Auxiliary Volume Cycle Button)
     *    Mode: Input (00b), Pull-Up (01b) -> Active LOW */
    GPIOA->MODER &= ~(3U << 2);
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(3U << 2)) | (1U << 2);
}

/* -------------------------------------------------------------------------- */
/* Physical RGB LED Control                                                   */
/* -------------------------------------------------------------------------- */
static void update_status_leds(player_state_t state)
{
    /* Onboard LEDs on RT-Spark are active LOW for PF11 (Blue) and PF12 (Red) */
    if (state == PLAYER_STATE_PLAYING) {
        /* Blue LED ON, Red OFF, Green OFF */
        GPIOF->BSRR = (1U << (11 + 16)); /* Blue ON */
        GPIOF->BSRR = (1U << 12);        /* Red OFF */
        GPIOE->BSRR = (1U << (3 + 16));  /* Green OFF */
    } else if (state == PLAYER_STATE_PAUSED || state == PLAYER_STATE_STOPPED) {
        /* Red LED ON, Blue OFF, Green OFF */
        GPIOF->BSRR = (1U << 11);        /* Blue OFF */
        GPIOF->BSRR = (1U << (12 + 16)); /* Red ON */
        GPIOE->BSRR = (1U << (3 + 16));  /* Green OFF */
    } else if (state == PLAYER_STATE_CONFIRMING) {
        /* Green LED ON, Red OFF, Blue OFF */
        GPIOF->BSRR = (1U << 11);        /* Blue OFF */
        GPIOF->BSRR = (1U << 12);        /* Red OFF */
        GPIOE->BSRR = (1U << 3);         /* Green ON */
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

        /* Footer Controls Guide showing Button 2 / Button 3 Volume Controls */
        lcd_draw_line(0, 192, LCD_WIDTH - 1, 192, LCD_COLOR_DARKGREY);
        lcd_show_string(10, 198, "B2:Vol- | B3:Vol+", LCD_COLOR_YELLOW, LCD_COLOR_BLACK);
        lcd_show_string(10, 216, "B1:Latch | USER:P/P", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
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

        /* 2. Redraw LCD display if state, track, or volume changed, or if in confirmation */
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
/* Thread 2: Polling Buttons with Debouncing & Binary Song Selection          */
/* -------------------------------------------------------------------------- */
void polling_buttons(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    printk("[Button_Thread] Started\n");

    /* Software debounce states */
    bool b1_last = false;
    bool user_last = false;
    uint8_t b1_stable_count = 0;
    uint8_t user_stable_count = 0;

    while (1) {
        /* Read physical pin levels:
         * PC0 (B1), PC1 (B2), PC4 (B3), PC5 (B4) are active LOW (pull-up).
         * PA0 (USER_BUTTON) is active HIGH (pull-down).
         */
        bool b1_raw = ((GPIOC->IDR & (1U << 0)) == 0);
        bool b2_raw = ((GPIOC->IDR & (1U << 1)) == 0);
        bool b3_raw = ((GPIOC->IDR & (1U << 4)) == 0);
        bool b4_raw = ((GPIOC->IDR & (1U << 5)) == 0);
        bool user_raw = ((GPIOA->IDR & (1U << 0)) != 0);

        /* Debounce Button 1 */
        bool b1_pressed_event = false;
        if (b1_raw) {
            if (b1_stable_count < 3) {
                b1_stable_count++;
                if (b1_stable_count == 3 && !b1_last) {
                    b1_pressed_event = true;
                    b1_last = true;
                }
            }
        } else {
            b1_stable_count = 0;
            b1_last = false;
        }

        /* Debounce USER_BUTTON */
        bool user_pressed_event = false;
        if (user_raw) {
            if (user_stable_count < 3) {
                user_stable_count++;
                if (user_stable_count == 3 && !user_last) {
                    user_pressed_event = true;
                    user_last = true;
                }
            }
        } else {
            user_stable_count = 0;
            user_last = false;
        }

        /* 1. Handle Button 1 Event (Selection Latch & Confirmation) */
        if (b1_pressed_event) {
            k_mutex_lock(&g_player_mutex, K_FOREVER);
            if (g_player.state == PLAYER_STATE_CONFIRMING) {
                /* Second press of Button 1 confirms choice */
                g_player.current_song_index = g_player.prospective_song_index;
                g_player.state = PLAYER_STATE_PLAYING;
                g_player.state_changed = true;
                const song_info_t *s = get_song_info(g_player.current_song_index);
                printk("[Button] CONFIRMED: Playing Track [%u] '%s %s'\n",
                       g_player.current_song_index + 1, s->name1, s->name2);
            } else {
                /* First press of Button 1 while holding B2-B4: Latch song */
                uint8_t selected_song = decode_binary_song_index(b2_raw, b3_raw, b4_raw);
                g_player.prospective_song_index = selected_song;
                g_player.confirmation_start_ms = k_uptime_get_32();
                g_player.previous_state = (g_player.state == PLAYER_STATE_STOPPED) ?
                                          PLAYER_STATE_STOPPED : g_player.state;
                g_player.state = PLAYER_STATE_CONFIRMING;
                g_player.state_changed = true;
                const song_info_t *s = get_song_info(selected_song);
                printk("[Button] LATCHED: Track [%u] '%s %s' (B2=%d,B3=%d,B4=%d). Press B1 within 5s to confirm!\n",
                       selected_song + 1, s->name1, s->name2, b2_raw, b3_raw, b4_raw);
            }
            k_mutex_unlock(&g_player_mutex);
        }

        /* 2. Handle USER_BUTTON Event (Play / Pause / Replay) */
        if (user_pressed_event) {
            k_mutex_lock(&g_player_mutex, K_FOREVER);
            if (g_player.state != PLAYER_STATE_CONFIRMING) {
                g_player.state = toggle_play_pause(g_player.state);
                g_player.state_changed = true;
                printk("[Button] USER_BUTTON: Player is now %s\n", get_player_state_str(g_player.state));
            }
            k_mutex_unlock(&g_player_mutex);
        }

        /* 3. Check 5-Second Confirmation Timeout */
        k_mutex_lock(&g_player_mutex, K_FOREVER);
        if (g_player.state == PLAYER_STATE_CONFIRMING) {
            uint32_t now = k_uptime_get_32();
            if (check_confirmation_timeout(g_player.confirmation_start_ms, now, CONFIRMATION_TIMEOUT_MS)) {
                printk("[Button] Confirmation timeout expired (5s). Reverting to previous state.\n");
                g_player.state = g_player.previous_state;
                g_player.state_changed = true;
            }
        }
        k_mutex_unlock(&g_player_mutex);

        /* Cooperative sleep: 20 ms debounce period */
        k_sleep(K_MSEC(BUTTON_POLL_PERIOD_MS));
    }
}

/* -------------------------------------------------------------------------- */
/* Thread 3: Adjust Volume via Push Buttons                                    */
/* -------------------------------------------------------------------------- */
void adjust_volume(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    printk("[Volume_Thread] Started (Button-Controlled Volume)\n");

    bool pa1_last = false;
    uint8_t pa1_stable = 0;

    while (1) {
        /* Volume adjustment is active when NOT in confirmation mode */
        bool can_adjust = false;
        k_mutex_lock(&g_player_mutex, K_FOREVER);
        can_adjust = (g_player.state != PLAYER_STATE_CONFIRMING);
        k_mutex_unlock(&g_player_mutex);

        if (can_adjust) {
            /* Button 1 (PC0) must NOT be pressed to avoid conflicting with song selection */
            bool b1_held = ((GPIOC->IDR & (1U << 0)) == 0);
            if (!b1_held) {
                /* Read Volume Down (Button 2 / PC1) and Volume Up (Button 3 / PC4) */
                bool b2_pressed = ((GPIOC->IDR & (1U << 1)) == 0);
                bool b3_pressed = ((GPIOC->IDR & (1U << 4)) == 0);

                /* Read optional external 5th button on PA1 (active LOW) */
                bool pa1_raw = ((GPIOA->IDR & (1U << 1)) == 0);
                bool pa1_event = false;
                if (pa1_raw) {
                    if (pa1_stable < 3) {
                        pa1_stable++;
                        if (pa1_stable == 3 && !pa1_last) {
                            pa1_event = true;
                            pa1_last = true;
                        }
                    }
                } else {
                    pa1_stable = 0;
                    pa1_last = false;
                }

                if (b2_pressed && !b3_pressed) {
                    /* Volume Down step */
                    k_mutex_lock(&g_player_mutex, K_FOREVER);
                    if (g_player.volume_percent >= VOLUME_STEP_PERCENT) {
                        g_player.volume_percent -= VOLUME_STEP_PERCENT;
                    } else {
                        g_player.volume_percent = 0;
                    }
                    g_player.state_changed = true;
                    printk("[Volume] Volume Down (Button 2): %u%%\n", g_player.volume_percent);
                    k_mutex_unlock(&g_player_mutex);
                    k_sleep(K_MSEC(120)); /* Rate limiter for smooth repeat */
                } else if (b3_pressed && !b2_pressed) {
                    /* Volume Up step */
                    k_mutex_lock(&g_player_mutex, K_FOREVER);
                    if (g_player.volume_percent <= (100 - VOLUME_STEP_PERCENT)) {
                        g_player.volume_percent += VOLUME_STEP_PERCENT;
                    } else {
                        g_player.volume_percent = 100;
                    }
                    g_player.state_changed = true;
                    printk("[Volume] Volume Up (Button 3): %u%%\n", g_player.volume_percent);
                    k_mutex_unlock(&g_player_mutex);
                    k_sleep(K_MSEC(120)); /* Rate limiter for smooth repeat */
                } else if (pa1_event) {
                    /* Cycle preset volume levels: 25% -> 50% -> 75% -> 100% -> 0% */
                    k_mutex_lock(&g_player_mutex, K_FOREVER);
                    g_player.volume_percent = (g_player.volume_percent + 25) % 125;
                    if (g_player.volume_percent > 100) {
                        g_player.volume_percent = 0;
                    }
                    g_player.state_changed = true;
                    printk("[Volume] Preset Cycle (PA1 Button): %u%%\n", g_player.volume_percent);
                    k_mutex_unlock(&g_player_mutex);
                }
            }
        }

        /* Cooperative sleep */
        k_sleep(K_MSEC(VOLUME_THREAD_PERIOD_MS));
    }
}

/* Public initialization helper */
void init_player_peripherals(void)
{
    init_hardware_peripherals();
}
