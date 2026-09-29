/**
 * @file threads.c
 * @brief Implementation of the 3 cooperative Zephyr RTOS threads for the
 *        Personal MP3 Player with intuitive directional D-pad controls:
 *        - UP / DOWN    : Scroll through musical repertoire tracks (1 to 8)
 *        - LEFT / RIGHT : Smooth volume adjustment (0% to 100%)
 *        - PRESS / PA0  : Play / Pause toggle
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
    .volume_percent = VOLUME_DEFAULT_PERCENT,
    .state_changed = true
};

/* Mutexes for exclusive resource access */
K_MUTEX_DEFINE(g_lcd_mutex);
K_MUTEX_DEFINE(g_player_mutex);

/* -------------------------------------------------------------------------- */
/* Physical Hardware GPIO Initialization (D-Pad, LEDs, Aux)                   */
/* -------------------------------------------------------------------------- */
static void init_hardware_peripherals(void)
{
    /* 1. Enable Clocks for GPIOA, GPIOC, GPIOE, GPIOF */
    RCC->AHB1ENR |= (RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOCEN |
                     RCC_AHB1ENR_GPIOEEN | RCC_AHB1ENR_GPIOFEN);

    /* 2. Configure Status LEDs: PF11 (Blue) and PF12 (Red) as Push-Pull Outputs */
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

    /* Turn LEDs OFF initially (Onboard PF11/PF12 are active LOW on RT-Spark) */
    GPIOF->BSRR = (1U << 11) | (1U << 12);
    GPIOE->BSRR = (1U << (3 + 16));

    /* 3. Configure Directional D-Pad Buttons on GPIOC:
     *    PC5: SW2 / GPIO_BTN_UP    (Scroll Track Up)
     *    PC1: SW4 / GPIO_BTN_DOWN  (Scroll Track Down)
     *    PC0: SW3 / GPIO_BTN_LEFT  (Volume Down)
     *    PC4: SW5 / GPIO_BTN_RIGHT (Volume Up)
     *    Mode: Input (00b), Pull-Up (01b) -> Active LOW on RT-Spark */
    static const uint8_t dpad_pins[] = {0, 1, 4, 5};
    for (uint32_t i = 0; i < sizeof(dpad_pins); i++) {
        uint32_t pin = dpad_pins[i];
        GPIOC->MODER &= ~(3U << (pin * 2));
        GPIOC->PUPDR = (GPIOC->PUPDR & ~(3U << (pin * 2))) | (1U << (pin * 2));
    }

    /* 4. Configure PRESS / USER_BUTTON on GPIOA:
     *    PA0 (PA0-WKUP / USER_BUTTON)
     *    Mode: Input (00b), Pull-Down (10b) -> Active HIGH on onboard key */
    GPIOA->MODER &= ~(3U << 0);
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(3U << 0)) | (2U << 0);

    /* 5. Configure Optional 5th Button on PA1:
     *    PA1 (Auxiliary Button)
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
    } else {
        /* Red LED ON, Blue OFF, Green OFF (Paused or Stopped) */
        GPIOF->BSRR = (1U << 11);        /* Blue OFF */
        GPIOF->BSRR = (1U << (12 + 16)); /* Red ON */
        GPIOE->BSRR = (1U << (3 + 16));  /* Green OFF */
    }
}

/* -------------------------------------------------------------------------- */
/* ST7789 Graphical User Interface Rendering                                  */
/* -------------------------------------------------------------------------- */
static void render_lcd_screen(player_state_t state, uint8_t cur_song_idx, uint8_t volume)
{
    char buf[32];

    /* 1. Header Banner */
    lcd_fill_rect(0, 0, LCD_WIDTH - 1, 26, LCD_COLOR_NAVY);
    lcd_show_string(16, 6, "BCA182: MP3 PLAYER", LCD_COLOR_WHITE, LCD_COLOR_NAVY);
    lcd_draw_line(0, 27, LCD_WIDTH - 1, 27, LCD_COLOR_CYAN);

    /* 2. Main Playback Screen Body */
    lcd_fill_rect(0, 28, LCD_WIDTH - 1, LCD_HEIGHT - 1, LCD_COLOR_BLACK);

    /* Status Badge */
    uint16_t status_color = (state == PLAYER_STATE_PLAYING) ? LCD_COLOR_GREEN :
                            (state == PLAYER_STATE_PAUSED)  ? LCD_COLOR_YELLOW :
                                                              LCD_COLOR_RED;
    snprintf(buf, sizeof(buf), "STATUS: %s", get_player_state_str(state));
    lcd_show_string(14, 36, buf, status_color, LCD_COLOR_BLACK);

    /* Track Number and Title */
    const song_info_t *c_song = get_song_info(cur_song_idx);
    snprintf(buf, sizeof(buf), "Track #%u of %u", cur_song_idx + 1, TOTAL_PLAYABLE_SONGS);
    lcd_show_string(14, 58, buf, LCD_COLOR_GRAY, LCD_COLOR_BLACK);

    lcd_show_string(14, 80, "TITLE:", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    lcd_show_string(14, 98, c_song->name1, LCD_COLOR_CYAN, LCD_COLOR_BLACK);
    lcd_show_string(14, 118, c_song->name2, LCD_COLOR_CYAN, LCD_COLOR_BLACK);

    /* Volume Level Bar */
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

    /* Directional D-Pad Controls Footer */
    lcd_draw_line(0, 190, LCD_WIDTH - 1, 190, LCD_COLOR_DARKGREY);
    lcd_show_string(10, 198, "UP/DN: Track | L/R: Vol", LCD_COLOR_YELLOW, LCD_COLOR_BLACK);
    lcd_show_string(10, 218, "PRESS/PA0: Play/Pause", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
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
        uint8_t volume;

        /* Snapshot state under mutex */
        k_mutex_lock(&g_player_mutex, K_FOREVER);
        current_state = g_player.state;
        current_song = g_player.current_song_index;
        volume = g_player.volume_percent;
        bool force_redraw = g_player.state_changed;
        g_player.state_changed = false;
        k_mutex_unlock(&g_player_mutex);

        /* 1. Update physical RGB LEDs */
        update_status_leds(current_state);

        /* 2. Redraw LCD display if state, track, or volume changed */
        bool needs_redraw = force_redraw ||
                            (current_state != last_rendered_state) ||
                            (current_song != last_rendered_song) ||
                            (volume != last_rendered_vol);

        if (needs_redraw) {
            if (k_mutex_lock(&g_lcd_mutex, K_MSEC(50)) == 0) {
                render_lcd_screen(current_state, current_song, volume);
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
/* Thread 2: Polling Buttons (UP/DOWN Track Scrolling & Play/Pause)           */
/* -------------------------------------------------------------------------- */
void polling_buttons(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    printk("[Button_Thread] Started (UP/DOWN Track Navigation)\n");

    /* Software debounce states */
    bool up_last = false;
    bool down_last = false;
    bool press_last = false;
    uint8_t up_stable = 0;
    uint8_t down_stable = 0;
    uint8_t press_stable = 0;
    uint32_t down_press_start = 0;

    while (1) {
        /* Read physical pins:
         * PC5 (UP): active LOW (pull-up)
         * PC1 (DOWN): active LOW (pull-up)
         * PA0 (PRESS / USER_BUTTON): active HIGH (pull-down)
         */
        bool up_raw = ((GPIOC->IDR & (1U << 5)) == 0);
        bool down_raw = ((GPIOC->IDR & (1U << 1)) == 0);
        bool press_raw = ((GPIOA->IDR & (1U << 0)) != 0);

        /* Debounce UP button */
        bool up_event = false;
        if (up_raw) {
            if (up_stable < 3) {
                up_stable++;
                if (up_stable == 3 && !up_last) {
                    up_event = true;
                    up_last = true;
                }
            }
        } else {
            up_stable = 0;
            up_last = false;
        }

        /* Debounce DOWN button */
        bool down_event = false;
        bool down_long_press_event = false;
        if (down_raw) {
            if (down_stable < 3) {
                down_stable++;
                if (down_stable == 3 && !down_last) {
                    down_event = true;
                    down_last = true;
                    down_press_start = k_uptime_get_32();
                }
            } else if (down_last) {
                /* Check for long-press on DOWN (> 600 ms) as alternative Play/Pause */
                if (down_press_start > 0 && (k_uptime_get_32() - down_press_start) > 600) {
                    down_long_press_event = true;
                    down_press_start = 0; /* Consume */
                }
            }
        } else {
            down_stable = 0;
            down_last = false;
            down_press_start = 0;
        }

        /* Debounce PRESS / PA0 button */
        bool press_event = false;
        if (press_raw) {
            if (press_stable < 3) {
                press_stable++;
                if (press_stable == 3 && !press_last) {
                    press_event = true;
                    press_last = true;
                }
            }
        } else {
            press_stable = 0;
            press_last = false;
        }

        /* 1. Handle UP Button Event: Scroll Track Forward */
        if (up_event) {
            k_mutex_lock(&g_player_mutex, K_FOREVER);
            g_player.current_song_index = (g_player.current_song_index + 1) % TOTAL_PLAYABLE_SONGS;
            g_player.state_changed = true;
            const song_info_t *s = get_song_info(g_player.current_song_index);
            printk("[Nav] UP -> Next Track [%u/8]: %s %s\n",
                   g_player.current_song_index + 1, s->name1, s->name2);
            k_mutex_unlock(&g_player_mutex);
        }

        /* 2. Handle DOWN Button Event: Scroll Track Backward */
        if (down_event && !down_long_press_event) {
            k_mutex_lock(&g_player_mutex, K_FOREVER);
            g_player.current_song_index = (g_player.current_song_index + TOTAL_PLAYABLE_SONGS - 1) % TOTAL_PLAYABLE_SONGS;
            g_player.state_changed = true;
            const song_info_t *s = get_song_info(g_player.current_song_index);
            printk("[Nav] DOWN -> Prev Track [%u/8]: %s %s\n",
                   g_player.current_song_index + 1, s->name1, s->name2);
            k_mutex_unlock(&g_player_mutex);
        }

        /* 3. Handle PRESS / PLAY-PAUSE (PA0 or long-press DOWN) */
        if (press_event || down_long_press_event) {
            k_mutex_lock(&g_player_mutex, K_FOREVER);
            g_player.state = toggle_play_pause(g_player.state);
            g_player.state_changed = true;
            printk("[Nav] PRESS -> Player is now %s\n", get_player_state_str(g_player.state));
            k_mutex_unlock(&g_player_mutex);
        }

        /* Cooperative sleep */
        k_sleep(K_MSEC(BUTTON_POLL_PERIOD_MS));
    }
}

/* -------------------------------------------------------------------------- */
/* Thread 3: Adjust Volume (LEFT = Volume Down, RIGHT = Volume Up)            */
/* -------------------------------------------------------------------------- */
void adjust_volume(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    printk("[Volume_Thread] Started (LEFT/RIGHT Volume Navigation)\n");

    bool aux_last = false;
    uint8_t aux_stable = 0;

    while (1) {
        /* Read physical pins:
         * PC0 (LEFT): Volume Down (active LOW)
         * PC4 (RIGHT): Volume Up (active LOW)
         * PA1 (AUX): Optional Preset Cycle (active LOW)
         */
        bool left_pressed = ((GPIOC->IDR & (1U << 0)) == 0);
        bool right_pressed = ((GPIOC->IDR & (1U << 4)) == 0);
        bool aux_raw = ((GPIOA->IDR & (1U << 1)) == 0);

        /* Debounce AUX button */
        bool aux_event = false;
        if (aux_raw) {
            if (aux_stable < 3) {
                aux_stable++;
                if (aux_stable == 3 && !aux_last) {
                    aux_event = true;
                    aux_last = true;
                }
            }
        } else {
            aux_stable = 0;
            aux_last = false;
        }

        if (left_pressed && !right_pressed) {
            /* LEFT: Volume Down */
            k_mutex_lock(&g_player_mutex, K_FOREVER);
            if (g_player.volume_percent >= VOLUME_STEP_PERCENT) {
                g_player.volume_percent -= VOLUME_STEP_PERCENT;
            } else {
                g_player.volume_percent = 0;
            }
            g_player.state_changed = true;
            printk("[Volume] LEFT -> Vol: %u%%\n", g_player.volume_percent);
            k_mutex_unlock(&g_player_mutex);
            k_sleep(K_MSEC(120)); /* Smooth repeat rate */
        } else if (right_pressed && !left_pressed) {
            /* RIGHT: Volume Up */
            k_mutex_lock(&g_player_mutex, K_FOREVER);
            if (g_player.volume_percent <= (100 - VOLUME_STEP_PERCENT)) {
                g_player.volume_percent += VOLUME_STEP_PERCENT;
            } else {
                g_player.volume_percent = 100;
            }
            g_player.state_changed = true;
            printk("[Volume] RIGHT -> Vol: %u%%\n", g_player.volume_percent);
            k_mutex_unlock(&g_player_mutex);
            k_sleep(K_MSEC(120)); /* Smooth repeat rate */
        } else if (aux_event) {
            /* AUX Button on PA1: Cycle 25% -> 50% -> 75% -> 100% -> 0% */
            k_mutex_lock(&g_player_mutex, K_FOREVER);
            g_player.volume_percent = (g_player.volume_percent + 25) % 125;
            if (g_player.volume_percent > 100) {
                g_player.volume_percent = 0;
            }
            g_player.state_changed = true;
            printk("[Volume] AUX -> Vol Preset: %u%%\n", g_player.volume_percent);
            k_mutex_unlock(&g_player_mutex);
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
