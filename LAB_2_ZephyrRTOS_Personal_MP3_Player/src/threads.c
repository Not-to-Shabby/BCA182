/**
 * @file threads.c
 * @brief Implementation of the 3 cooperative Zephyr RTOS threads for the
 *        Personal MP3 Player with unified non-racing Short-Press & Long-Press
 *        architecture and FLICKER-FREE partial LCD rendering:
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
#include "audio_engine.h"
#include "audio_codec_es8388.h"
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
/* Flicker-Free Partial LCD Rendering Routines                                */
/* -------------------------------------------------------------------------- */
static void render_note_row(uint16_t current_note, uint16_t total_notes, player_state_t state)
{
    char buf[32];
    if (state == PLAYER_STATE_PLAYING) {
        snprintf(buf, sizeof(buf), "NOTE: %-3u / %-3u     ", current_note + 1, total_notes);
        lcd_show_string(14, 130, buf, LCD_COLOR_GREEN, LCD_COLOR_BLACK);
    } else if (state == PLAYER_STATE_PAUSED) {
        snprintf(buf, sizeof(buf), "PAUSED AT NOTE %-3u ", current_note + 1);
        lcd_show_string(14, 130, buf, LCD_COLOR_YELLOW, LCD_COLOR_BLACK);
    } else {
        lcd_show_string(14, 130, "PRESS PA0 TO PLAY   ", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
    }
}

static void render_volume_row(uint8_t volume)
{
    char buf[32];
    if (volume == 0) {
        snprintf(buf, sizeof(buf), "VOLUME: MUTE (0%%)  ");
    } else {
        snprintf(buf, sizeof(buf), "VOLUME: %3u%%        ", volume);
    }
    lcd_show_string(14, 150, buf, (volume == 0) ? LCD_COLOR_ORANGE : LCD_COLOR_WHITE, LCD_COLOR_BLACK);

    uint16_t bar_width = (uint16_t)((volume * 210U) / 100U);
    if (bar_width > 0) {
        lcd_fill_rect(15, 169, 15 + bar_width, 179, LCD_COLOR_GREEN);
    }
    if (bar_width < 210) {
        lcd_fill_rect(15 + bar_width + 1, 169, 225, 179, LCD_COLOR_BLACK);
    }
}

static void render_full_screen(player_state_t state, uint8_t cur_song_idx, uint8_t volume)
{
    char buf[32];

    /* 1. Top Header Banner */
    lcd_fill_rect(0, 0, LCD_WIDTH - 1, 26, LCD_COLOR_NAVY);
    lcd_show_string(16, 6, "BCA182: MP3 PLAYER", LCD_COLOR_WHITE, LCD_COLOR_NAVY);
    lcd_draw_line(0, 27, LCD_WIDTH - 1, 27, LCD_COLOR_CYAN);

    /* 2. Main Body Clear */
    lcd_fill_rect(0, 28, LCD_WIDTH - 1, LCD_HEIGHT - 1, LCD_COLOR_BLACK);

    /* Status Badge */
    uint16_t status_color = (state == PLAYER_STATE_PLAYING) ? LCD_COLOR_GREEN :
                            (state == PLAYER_STATE_PAUSED)  ? LCD_COLOR_YELLOW :
                                                              LCD_COLOR_RED;
    snprintf(buf, sizeof(buf), "STATUS: %s", get_player_state_str(state));
    lcd_show_string(14, 34, buf, status_color, LCD_COLOR_BLACK);

    /* Track Number and Title */
    const song_info_t *c_song = get_song_info(cur_song_idx);
    snprintf(buf, sizeof(buf), "Track #%u of %u", cur_song_idx + 1, TOTAL_PLAYABLE_SONGS);
    lcd_show_string(14, 54, buf, LCD_COLOR_GRAY, LCD_COLOR_BLACK);

    lcd_show_string(14, 74, "TITLE:", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    lcd_show_string(14, 92, c_song->name1, LCD_COLOR_CYAN, LCD_COLOR_BLACK);
    lcd_show_string(14, 110, c_song->name2, LCD_COLOR_CYAN, LCD_COLOR_BLACK);

    /* Note Progress Row */
    render_note_row(audio_engine_get_note_index(), c_song->length, state);

    /* Volume Level Bar */
    lcd_draw_rect(14, 168, 226, 180, LCD_COLOR_WHITE);
    render_volume_row(volume);

    /* Directional D-Pad Controls Footer */
    lcd_draw_line(0, 188, LCD_WIDTH - 1, 188, LCD_COLOR_DARKGREY);
    lcd_show_string(10, 196, "UP/DN:Track (Hold:P/P)", LCD_COLOR_YELLOW, LCD_COLOR_BLACK);
    lcd_show_string(10, 216, "L/R:Vol | PA0:Play/Stop", LCD_COLOR_GRAY, LCD_COLOR_BLACK);

    /* Codec/I2S diagnostic line (ground truth for the audio path) */
    lcd_show_string(10, 232, audio_hardware_dac_status(),
                    LCD_COLOR_MAGENTA, LCD_COLOR_BLACK);
}

/* -------------------------------------------------------------------------- */
/* Thread 1: Update LCD and RGB LEDs (Flicker-Free Differential Render)       */
/* -------------------------------------------------------------------------- */
void update_lcd_leds_thread(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    printk("[LCD_LED_Thread] Started (Flicker-Free Differential Render)\n");

    uint8_t last_rendered_song = 0xFF;
    player_state_t last_rendered_state = (player_state_t)0xFF;
    uint8_t last_rendered_vol = 0xFF;
    uint16_t last_rendered_note = 0xFFFF;

    uint8_t last_audio_song = 0xFF;
    player_state_t last_audio_state = (player_state_t)0xFF;
    uint8_t last_audio_vol = 0xFF;

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

        /* 2. Synchronize Audio Synthesizer Engine */
        if (current_state == PLAYER_STATE_PLAYING) {
            if (last_audio_state != PLAYER_STATE_PLAYING || current_song != last_audio_song) {
                audio_engine_start_song(current_song);
                last_audio_song = current_song;
            } else if (!audio_engine_is_playing()) {
                audio_engine_resume();
            }
        } else if (current_state == PLAYER_STATE_PAUSED) {
            if (audio_engine_is_playing()) {
                audio_engine_pause();
            }
        } else if (current_state == PLAYER_STATE_STOPPED) {
            if (audio_engine_is_playing()) {
                audio_engine_stop();
            }
        }
        last_audio_state = current_state;

        if (volume != last_audio_vol) {
            audio_engine_set_volume(volume);
            last_audio_vol = volume;
        }

        /* 3. Flicker-Free Differential Screen Update:
         *    - Full screen redraw ONLY when track or player state changes.
         *    - When only the note advances or volume changes, ONLY update that specific line!
         *    - Never clear the entire screen on note changes!
         */
        uint16_t current_note = audio_engine_get_note_index();
        bool full_redraw = force_redraw ||
                           (current_state != last_rendered_state) ||
                           (current_song != last_rendered_song);
        bool vol_changed = (volume != last_rendered_vol);
        bool note_changed = (current_note != last_rendered_note);

        if (full_redraw) {
            if (k_mutex_lock(&g_lcd_mutex, K_MSEC(50)) == 0) {
                render_full_screen(current_state, current_song, volume);
                last_rendered_state = current_state;
                last_rendered_song = current_song;
                last_rendered_vol = volume;
                last_rendered_note = current_note;
                k_mutex_unlock(&g_lcd_mutex);
            }
        } else {
            if (vol_changed) {
                if (k_mutex_lock(&g_lcd_mutex, K_MSEC(50)) == 0) {
                    render_volume_row(volume);
                    last_rendered_vol = volume;
                    k_mutex_unlock(&g_lcd_mutex);
                }
            }
            if (note_changed) {
                if (k_mutex_lock(&g_lcd_mutex, K_MSEC(50)) == 0) {
                    const song_info_t *c_song = get_song_info(current_song);
                    render_note_row(current_note, c_song->length, current_state);
                    last_rendered_note = current_note;
                    k_mutex_unlock(&g_lcd_mutex);
                }
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
