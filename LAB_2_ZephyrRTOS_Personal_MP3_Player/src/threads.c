/**
 * @file threads.c
 * @brief Implementation of the 3 cooperative Zephyr RTOS threads for the
 *        Personal MP3 Player with unified non-racing Short-Press & Long-Press
 *        architecture on ALL buttons:
 *        - UP (PC5)     : Short -> Next Track | Long -> Jump to Track 1
 *        - DOWN (PC1)   : Short -> Prev Track | Long -> Play / Pause Toggle
 *        - LEFT (PC0)   : Short -> Vol -5%    | Long / Hold -> Rapid Smooth Vol Down
 *        - RIGHT (PC4)  : Short -> Vol +5%    | Long / Hold -> Rapid Smooth Vol Up
 *        - PRESS (PA0)  : Short -> Play/Pause | Long -> Stop Playback
 *        - AUX (PA1)    : Short -> Cycle Preset | Long -> Mute / Unmute Toggle
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
    .pre_mute_volume = VOLUME_DEFAULT_PERCENT,
    .state_changed = true
};

/* Mutexes for exclusive resource access */
K_MUTEX_DEFINE(g_lcd_mutex);
K_MUTEX_DEFINE(g_player_mutex);

/* -------------------------------------------------------------------------- */
/* Unified Button State Machine (Zero Race Condition)                        */
/* -------------------------------------------------------------------------- */
typedef struct {
    bool is_pressed;
    bool long_triggered;
    uint8_t stable_count;
    uint32_t press_start_ms;
    uint32_t last_repeat_ms;
} button_tracker_t;

typedef enum {
    BTN_EVT_NONE = 0,
    BTN_EVT_SHORT_PRESS,    /**< Released before long threshold: genuine click */
    BTN_EVT_LONG_PRESS,     /**< Held >= threshold: long press fired once */
    BTN_EVT_HOLD_REPEAT     /**< Continuously held: auto-repeat event */
} button_event_t;

/**
 * @brief Update a button's debounced state machine and generate mutually
 *        exclusive short-press and long-press events.
 *
 * @param btn Pointer to button tracker state
 * @param raw_active True if physical button is currently active
 * @param now_ms Current system timestamp in ms
 * @param long_threshold_ms Time in ms to qualify as a long-press (e.g. 450 ms)
 * @param repeat_period_ms Time in ms between repeat events while held (0 = no repeat)
 * @return button_event_t Detected event
 */
static button_event_t update_button_state(button_tracker_t *btn, bool raw_active,
                                          uint32_t now_ms, uint32_t long_threshold_ms,
                                          uint32_t repeat_period_ms)
{
    if (raw_active) {
        if (!btn->is_pressed) {
            /* Debounce filter: require 3 consecutive active samples */
            if (btn->stable_count < 3) {
                btn->stable_count++;
                if (btn->stable_count == 3) {
                    btn->is_pressed = true;
                    btn->press_start_ms = now_ms;
                    btn->last_repeat_ms = now_ms;
                    btn->long_triggered = false;
                }
            }
        } else {
            /* Button is actively held */
            if (!btn->long_triggered) {
                if ((now_ms - btn->press_start_ms) >= long_threshold_ms) {
                    btn->long_triggered = true;
                    btn->last_repeat_ms = now_ms;
                    return BTN_EVT_LONG_PRESS;
                }
            } else if (repeat_period_ms > 0) {
                if ((now_ms - btn->last_repeat_ms) >= repeat_period_ms) {
                    btn->last_repeat_ms = now_ms;
                    return BTN_EVT_HOLD_REPEAT;
                }
            }
        }
    } else {
        /* Physical pin is inactive / released */
        button_event_t release_evt = BTN_EVT_NONE;
        if (btn->is_pressed) {
            btn->is_pressed = false;
            /* Only fire short press if long-press was NEVER triggered */
            if (!btn->long_triggered) {
                release_evt = BTN_EVT_SHORT_PRESS;
            }
        }
        btn->stable_count = 0;
        btn->press_start_ms = 0;
        btn->last_repeat_ms = 0;
        btn->long_triggered = false;
        return release_evt;
    }
    return BTN_EVT_NONE;
}

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
     *    PC5: SW2 / GPIO_BTN_UP    (Scroll Track Up / Hold: Reset to Track 1)
     *    PC1: SW4 / GPIO_BTN_DOWN  (Scroll Track Down / Hold: Play-Pause)
     *    PC0: SW3 / GPIO_BTN_LEFT  (Volume Down / Hold: Smooth Vol Down)
     *    PC4: SW5 / GPIO_BTN_RIGHT (Volume Up / Hold: Smooth Vol Up)
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
    if (volume == 0) {
        snprintf(buf, sizeof(buf), "VOLUME: MUTE (0%%)");
    } else {
        snprintf(buf, sizeof(buf), "VOLUME: %u%%", volume);
    }
    lcd_show_string(14, 146, buf, (volume == 0) ? LCD_COLOR_ORANGE : LCD_COLOR_WHITE, LCD_COLOR_BLACK);
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
    lcd_show_string(10, 198, "UP/DN:Track (Hold:P/P)", LCD_COLOR_YELLOW, LCD_COLOR_BLACK);
    lcd_show_string(10, 218, "L/R:Vol | PA0:Play/Stop", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
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
/* Thread 2: Polling Navigation Buttons (UP, DOWN, PA0/USER)                   */
/* -------------------------------------------------------------------------- */
void polling_buttons(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    printk("[Button_Thread] Started (UP/DOWN/PA0 Navigation Tracker)\n");

    /* Unified button trackers */
    static button_tracker_t btn_up    = {0};
    static button_tracker_t btn_down  = {0};
    static button_tracker_t btn_press = {0};

    while (1) {
        uint32_t now = k_uptime_get_32();

        /* Sample physical pins:
         * PC5 (UP): active LOW (pull-up)
         * PC1 (DOWN): active LOW (pull-up)
         * PA0 (PRESS / USER_BUTTON): active HIGH (pull-down)
         */
        bool up_active    = ((GPIOC->IDR & (1U << 5)) == 0);
        bool down_active  = ((GPIOC->IDR & (1U << 1)) == 0);
        bool press_active = ((GPIOA->IDR & (1U << 0)) != 0);

        button_event_t up_evt    = update_button_state(&btn_up, up_active, now, 450, 0);
        button_event_t down_evt  = update_button_state(&btn_down, down_active, now, 450, 0);
        button_event_t press_evt = update_button_state(&btn_press, press_active, now, 500, 0);

        /* 1. UP Button Events */
        if (up_evt == BTN_EVT_SHORT_PRESS) {
            /* Short Click: Next Track (+1) */
            k_mutex_lock(&g_player_mutex, K_FOREVER);
            g_player.current_song_index = (g_player.current_song_index + 1) % TOTAL_PLAYABLE_SONGS;
            g_player.state_changed = true;
            const song_info_t *s = get_song_info(g_player.current_song_index);
            printk("[Nav] UP (Click) -> Next Track [%u/8]: %s %s\n",
                   g_player.current_song_index + 1, s->name1, s->name2);
            k_mutex_unlock(&g_player_mutex);
        } else if (up_evt == BTN_EVT_LONG_PRESS) {
            /* Long Press: Jump to Track 1 (Für Elise) */
            k_mutex_lock(&g_player_mutex, K_FOREVER);
            g_player.current_song_index = 0;
            g_player.state_changed = true;
            const song_info_t *s = get_song_info(0);
            printk("[Nav] UP (Hold) -> Jump to Track [1/8]: %s %s\n", s->name1, s->name2);
            k_mutex_unlock(&g_player_mutex);
        }

        /* 2. DOWN Button Events */
        if (down_evt == BTN_EVT_SHORT_PRESS) {
            /* Short Click: Previous Track (-1) */
            k_mutex_lock(&g_player_mutex, K_FOREVER);
            g_player.current_song_index = (g_player.current_song_index + TOTAL_PLAYABLE_SONGS - 1) % TOTAL_PLAYABLE_SONGS;
            g_player.state_changed = true;
            const song_info_t *s = get_song_info(g_player.current_song_index);
            printk("[Nav] DOWN (Click) -> Prev Track [%u/8]: %s %s\n",
                   g_player.current_song_index + 1, s->name1, s->name2);
            k_mutex_unlock(&g_player_mutex);
        } else if (down_evt == BTN_EVT_LONG_PRESS) {
            /* Long Press: Toggle Play / Pause on current track without changing tracks */
            k_mutex_lock(&g_player_mutex, K_FOREVER);
            g_player.state = toggle_play_pause(g_player.state);
            g_player.state_changed = true;
            const song_info_t *s = get_song_info(g_player.current_song_index);
            printk("[Nav] DOWN (Hold) -> PLAY/PAUSE: Track [%u] '%s %s' is now %s\n",
                   g_player.current_song_index + 1, s->name1, s->name2,
                   get_player_state_str(g_player.state));
            k_mutex_unlock(&g_player_mutex);
        }

        /* 3. PRESS / PA0 (USER_BUTTON) Events */
        if (press_evt == BTN_EVT_SHORT_PRESS) {
            /* Short Click: Toggle Play / Pause */
            k_mutex_lock(&g_player_mutex, K_FOREVER);
            g_player.state = toggle_play_pause(g_player.state);
            g_player.state_changed = true;
            const song_info_t *s = get_song_info(g_player.current_song_index);
            printk("[Nav] USER_BUTTON (Click) -> Track [%u] '%s %s' is now %s\n",
                   g_player.current_song_index + 1, s->name1, s->name2,
                   get_player_state_str(g_player.state));
            k_mutex_unlock(&g_player_mutex);
        } else if (press_evt == BTN_EVT_LONG_PRESS) {
            /* Long Press: Full Stop Playback */
            k_mutex_lock(&g_player_mutex, K_FOREVER);
            g_player.state = PLAYER_STATE_STOPPED;
            g_player.state_changed = true;
            printk("[Nav] USER_BUTTON (Hold) -> STOPPED playback.\n");
            k_mutex_unlock(&g_player_mutex);
        }

        /* Cooperative sleep */
        k_sleep(K_MSEC(BUTTON_POLL_PERIOD_MS));
    }
}

/* -------------------------------------------------------------------------- */
/* Thread 3: Adjust Volume (LEFT, RIGHT, AUX Button Trackers)                 */
/* -------------------------------------------------------------------------- */
void adjust_volume(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    printk("[Volume_Thread] Started (LEFT/RIGHT/AUX Volume Tracker)\n");

    /* Unified button trackers for volume controls */
    static button_tracker_t btn_left  = {0};
    static button_tracker_t btn_right = {0};
    static button_tracker_t btn_aux   = {0};

    while (1) {
        uint32_t now = k_uptime_get_32();

        /* Sample physical pins:
         * PC0 (LEFT): Volume Down (active LOW)
         * PC4 (RIGHT): Volume Up (active LOW)
         * PA1 (AUX): Optional Preset Cycle / Mute (active LOW)
         */
        bool left_active  = ((GPIOC->IDR & (1U << 0)) == 0);
        bool right_active = ((GPIOC->IDR & (1U << 4)) == 0);
        bool aux_active   = ((GPIOA->IDR & (1U << 1)) == 0);

        /* LEFT and RIGHT use repeat rate of 90ms for smooth volume ramping while held */
        button_event_t left_evt  = update_button_state(&btn_left, left_active, now, 400, 90);
        button_event_t right_evt = update_button_state(&btn_right, right_active, now, 400, 90);
        button_event_t aux_evt   = update_button_state(&btn_aux, aux_active, now, 450, 0);

        /* 1. LEFT Button Events (Volume Down) */
        if (left_evt == BTN_EVT_SHORT_PRESS || left_evt == BTN_EVT_LONG_PRESS || left_evt == BTN_EVT_HOLD_REPEAT) {
            k_mutex_lock(&g_player_mutex, K_FOREVER);
            if (g_player.volume_percent >= VOLUME_STEP_PERCENT) {
                g_player.volume_percent -= VOLUME_STEP_PERCENT;
            } else {
                g_player.volume_percent = 0;
            }
            g_player.state_changed = true;
            printk("[Volume] LEFT (%s) -> Vol: %u%%\n",
                   (left_evt == BTN_EVT_SHORT_PRESS) ? "Click" : "Hold",
                   g_player.volume_percent);
            k_mutex_unlock(&g_player_mutex);
        }

        /* 2. RIGHT Button Events (Volume Up) */
        if (right_evt == BTN_EVT_SHORT_PRESS || right_evt == BTN_EVT_LONG_PRESS || right_evt == BTN_EVT_HOLD_REPEAT) {
            k_mutex_lock(&g_player_mutex, K_FOREVER);
            if (g_player.volume_percent <= (100 - VOLUME_STEP_PERCENT)) {
                g_player.volume_percent += VOLUME_STEP_PERCENT;
            } else {
                g_player.volume_percent = 100;
            }
            g_player.state_changed = true;
            printk("[Volume] RIGHT (%s) -> Vol: %u%%\n",
                   (right_evt == BTN_EVT_SHORT_PRESS) ? "Click" : "Hold",
                   g_player.volume_percent);
            k_mutex_unlock(&g_player_mutex);
        }

        /* 3. AUX Button Events (PA1) */
        if (aux_evt == BTN_EVT_SHORT_PRESS) {
            /* Short Click: Cycle presets 25% -> 50% -> 75% -> 100% -> 0% */
            k_mutex_lock(&g_player_mutex, K_FOREVER);
            g_player.volume_percent = (g_player.volume_percent + 25) % 125;
            if (g_player.volume_percent > 100) {
                g_player.volume_percent = 0;
            }
            g_player.state_changed = true;
            printk("[Volume] AUX (Click) -> Preset: %u%%\n", g_player.volume_percent);
            k_mutex_unlock(&g_player_mutex);
        } else if (aux_evt == BTN_EVT_LONG_PRESS) {
            /* Long Press: Instant Mute / Unmute Toggle */
            k_mutex_lock(&g_player_mutex, K_FOREVER);
            if (g_player.volume_percent > 0) {
                /* Mute: remember current volume */
                g_player.pre_mute_volume = g_player.volume_percent;
                g_player.volume_percent = 0;
                printk("[Volume] AUX (Hold) -> MUTED (0%%)\n");
            } else {
                /* Unmute: restore previous volume or 50% default */
                g_player.volume_percent = (g_player.pre_mute_volume > 0) ?
                                          g_player.pre_mute_volume : 50;
                printk("[Volume] AUX (Hold) -> UNMUTED (%u%%)\n", g_player.volume_percent);
            }
            g_player.state_changed = true;
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
