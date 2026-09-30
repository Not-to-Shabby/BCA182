#include "wav_player.h"
#include "sd_card_reader.h"
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
#include "sd_card_reader.h"
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

    /* 6. Configure Direct Hardware USART1 on PA9/PA10 for ST-LINK VCP (COM7) */
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    GPIOA->MODER = (GPIOA->MODER & ~((3U << (9 * 2)) | (3U << (10 * 2)))) |
                   ((2U << (9 * 2)) | (2U << (10 * 2)));
    GPIOA->OTYPER &= ~(1U << 9);
    GPIOA->OSPEEDR |= (3U << (9 * 2)) | (3U << (10 * 2));
    GPIOA->PUPDR = (GPIOA->PUPDR & ~((3U << (9 * 2)) | (3U << (10 * 2)))) |
                   ((1U << (9 * 2)) | (1U << (10 * 2))); /* Pull-up */
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~((0xFU << ((9 - 8) * 4)) | (0xFU << ((10 - 8) * 4)))) |
                    ((7U << ((9 - 8) * 4)) | (7U << ((10 - 8) * 4))); /* AF7 */
    USART1->BRR = 0x2D9; /* 84 MHz / 115200 baud */
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

void uart1_direct_send_char(char c)
{
    while (!(USART1->SR & USART_SR_TXE));
    USART1->DR = (uint8_t)c;
}

void uart1_direct_print(const char *str)
{
    if (str == NULL) return;
    while (*str) {
        if (*str == '\n') {
            uart1_direct_send_char('\r');
        }
        uart1_direct_send_char(*str++);
    }
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
        lcd_show_string(14, 90, buf, LCD_COLOR_GREEN, LCD_COLOR_BLACK);
    } else if (state == PLAYER_STATE_PAUSED) {
        snprintf(buf, sizeof(buf), "PAUSED AT NOTE %-3u ", current_note + 1);
        lcd_show_string(14, 90, buf, LCD_COLOR_YELLOW, LCD_COLOR_BLACK);
    } else {
        lcd_show_string(14, 90, "PRESS PA0 TO PLAY   ", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
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
    lcd_show_string(14, 106, buf, (volume == 0) ? LCD_COLOR_ORANGE : LCD_COLOR_WHITE, LCD_COLOR_BLACK);

    uint16_t bar_width = (uint16_t)((volume * 210U) / 100U);
    if (bar_width > 0) {
        lcd_fill_rect(15, 122, 15 + bar_width, 128, LCD_COLOR_GREEN);
    }
    if (bar_width < 210) {
        lcd_fill_rect(15 + bar_width + 1, 122, 225, 128, LCD_COLOR_BLACK);
    }
}

static void render_diag_box(void)
{
    const audio_diagnostics_t *diag = audio_get_diagnostics();
    bool buz_muted = audio_engine_is_buzzer_muted();
    char buf[36];

    /* Card background box: y=134 to y=192 */
    lcd_fill_rect(8, 134, LCD_WIDTH - 9, 192, LCD_COLOR_DARKGREY);
    lcd_draw_rect(8, 134, LCD_WIDTH - 9, 192, LCD_COLOR_CYAN);

    /* Line 1: ES8388 I2C detection & readback */
    if (diag->reg04_verified) {
        snprintf(buf, sizeof(buf), "CODEC:0x%02X R04:0x%02X(OK)", diag->es_addr, diag->reg04_readback);
        lcd_show_string(12, 138, buf, LCD_COLOR_GREEN, LCD_COLOR_DARKGREY);
    } else if (diag->es_detected) {
        snprintf(buf, sizeof(buf), "CODEC:0x%02X R04:0x%02X(!)", diag->es_addr, diag->reg04_readback);
        lcd_show_string(12, 138, buf, LCD_COLOR_YELLOW, LCD_COLOR_DARKGREY);
    } else {
        snprintf(buf, sizeof(buf), "CODEC:NACK (Check I2C)");
        lcd_show_string(12, 138, buf, LCD_COLOR_RED, LCD_COLOR_DARKGREY);
    }

    /* Line 2: Waveform & Live Buzzer State */
    audio_waveform_t wave = audio_hardware_dac_get_waveform();
    uint16_t wave_col = (wave == AUDIO_WAVE_SINE)     ? LCD_COLOR_CYAN :
                        (wave == AUDIO_WAVE_TRIANGLE) ? LCD_COLOR_YELLOW :
                        (wave == AUDIO_WAVE_SAWTOOTH) ? LCD_COLOR_ORANGE :
                                                        LCD_COLOR_MAGENTA;
    snprintf(buf, sizeof(buf), "WAVE:%-4s|BUZZER:%-5s",
             audio_hardware_dac_get_waveform_name(),
             buz_muted ? "MUTED" : "ON");
    lcd_show_string(12, 154, buf, wave_col, LCD_COLOR_DARKGREY);

    /* Line 3: SD Card Status & I2S samples */
    const sd_card_inspection_t *sd_insp = sd_card_get_inspection();
    if (sd_card_is_mounted()) {
        snprintf(buf, sizeof(buf), "SD:FAT32 %uTRK|I2S:%uK",
                 sd_card_get_track_count(), (unsigned int)(diag->i2s_tx_samples / 1000U));
        lcd_show_string(12, 172, buf, LCD_COLOR_GREEN, LCD_COLOR_DARKGREY);
    } else if (sd_insp->card_initialized) {
        snprintf(buf, sizeof(buf), "SD:%-5s(L+R+D:FMT)|I2S",
                 sd_insp->detected_fs_name);
        lcd_show_string(12, 172, buf, LCD_COLOR_ORANGE, LCD_COLOR_DARKGREY);
    } else {
        snprintf(buf, sizeof(buf), "SD:NO CARD |I2S:%uK",
                 (unsigned int)(diag->i2s_tx_samples / 1000U));
        lcd_show_string(12, 172, buf, LCD_COLOR_CYAN, LCD_COLOR_DARKGREY);
    }
}

static void render_usb_msc_screen(void)
{
    /* 1. Top Header Banner */
    lcd_fill_rect(0, 0, LCD_WIDTH - 1, 24, LCD_COLOR_NAVY);
    lcd_show_string(16, 4, "USB SD CARD READER", LCD_COLOR_YELLOW, LCD_COLOR_NAVY);
    lcd_draw_line(0, 25, LCD_WIDTH - 1, 25, LCD_COLOR_YELLOW);

    /* 2. Main Body Clear */
    lcd_fill_rect(0, 26, LCD_WIDTH - 1, LCD_HEIGHT - 1, LCD_COLOR_BLACK);
    lcd_show_string(14, 34, "STATUS: U-DISK BRIDGE", LCD_COLOR_CYAN, LCD_COLOR_BLACK);
    lcd_show_string(14, 52, "Connected via USB CN4", LCD_COLOR_WHITE, LCD_COLOR_BLACK);

    /* Info card */
    lcd_fill_rect(8, 76, LCD_WIDTH - 9, 184, LCD_COLOR_DARKGREY);
    lcd_draw_rect(8, 76, LCD_WIDTH - 9, 184, LCD_COLOR_YELLOW);
    lcd_show_string(12, 84,  "Windows File Explorer", LCD_COLOR_YELLOW, LCD_COLOR_DARKGREY);
    lcd_show_string(12, 102, "Mounted Removable Disk", LCD_COLOR_WHITE, LCD_COLOR_DARKGREY);
    lcd_show_string(12, 122, "Drag & Drop .WAV/.MP3", LCD_COLOR_GREEN, LCD_COLOR_DARKGREY);
    lcd_show_string(12, 142, "Files directly to SD!", LCD_COLOR_GREEN, LCD_COLOR_DARKGREY);
    lcd_show_string(12, 162, "Filesystem: FAT32", LCD_COLOR_CYAN, LCD_COLOR_DARKGREY);

    /* Footer Controls */
    lcd_draw_line(0, 196, LCD_WIDTH - 1, 196, LCD_COLOR_DARKGREY);
    lcd_show_string(10, 202, "Click UP to Exit &", LCD_COLOR_ORANGE, LCD_COLOR_BLACK);
    lcd_show_string(10, 218, "Re-Mount for Playback", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
}

static void render_full_screen(player_state_t state, uint8_t cur_song_idx, uint8_t volume)
{
    if (sd_card_get_mode() == SD_MODE_USB_CARD_READER) {
        render_usb_msc_screen();
        return;
    }

    char buf[32];

    /* 1. Top Header Banner */
    lcd_fill_rect(0, 0, LCD_WIDTH - 1, 24, LCD_COLOR_NAVY);
    lcd_show_string(16, 4, "BCA182: MP3 PLAYER", LCD_COLOR_WHITE, LCD_COLOR_NAVY);
    lcd_draw_line(0, 25, LCD_WIDTH - 1, 25, LCD_COLOR_CYAN);

    /* 2. Main Body Clear */
    lcd_fill_rect(0, 26, LCD_WIDTH - 1, LCD_HEIGHT - 1, LCD_COLOR_BLACK);

    /* Status Badge */
    uint16_t status_color = (state == PLAYER_STATE_PLAYING) ? LCD_COLOR_GREEN :
                            (state == PLAYER_STATE_PAUSED)  ? LCD_COLOR_YELLOW :
                                                              LCD_COLOR_RED;
    snprintf(buf, sizeof(buf), "STATUS: %s", get_player_state_str(state));
    lcd_show_string(14, 28, buf, status_color, LCD_COLOR_BLACK);

    /* Track Number and Title */
    const song_info_t *c_song = get_song_info(cur_song_idx);
    snprintf(buf, sizeof(buf), "Track #%u of %u", cur_song_idx + 1, (8 + sd_card_get_track_count()));
    lcd_show_string(14, 44, buf, LCD_COLOR_GRAY, LCD_COLOR_BLACK);

    lcd_show_string(14, 60, c_song->name1, LCD_COLOR_CYAN, LCD_COLOR_BLACK);
    lcd_show_string(14, 76, c_song->name2, LCD_COLOR_CYAN, LCD_COLOR_BLACK);

    /* Note Progress Row */
    render_note_row(audio_engine_get_note_index(), c_song->length, state);

    /* Volume Level Bar */
    lcd_draw_rect(14, 121, 226, 129, LCD_COLOR_WHITE);
    render_volume_row(volume);

    /* Live Hardware Diagnostic Card */
    render_diag_box();

    /* Directional D-Pad Controls Footer with Waveform & Buzzer Mute Hints */
    lcd_draw_line(0, 196, LCD_WIDTH - 1, 196, LCD_COLOR_DARKGREY);
    lcd_show_string(10, 202, "UP/DN:Track | L/R:Vol", LCD_COLOR_YELLOW, LCD_COLOR_BLACK);
    lcd_show_string(10, 218, "Hold UP:USB Disk Mode", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
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
    uint32_t last_rendered_samples = 0xFFFFFFFF;
    bool last_rendered_buzzer = false;
    audio_waveform_t last_rendered_wave = (audio_waveform_t)0xFF;
    sd_reader_mode_t last_rendered_sd_mode = (sd_reader_mode_t)0xFF;

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
                if (current_song < sd_card_get_track_count()) {
                    wav_player_start(sd_card_get_track(current_song)->filename);
                } else {
                    audio_engine_start_song(current_song - sd_card_get_track_count());
                }
                last_audio_song = current_song;
            } else if (!(audio_engine_is_playing() || wav_player_is_active())) {
                if (current_song < sd_card_get_track_count()) { wav_player_resume(); } else { audio_engine_resume(); }
            }
        } else if (current_state == PLAYER_STATE_PAUSED) {
            if (audio_engine_is_playing() || wav_player_is_active()) {
                if (wav_player_is_active()) { wav_player_pause(); } else { audio_engine_pause(); }
            }
        } else if (current_state == PLAYER_STATE_STOPPED) {
            if (audio_engine_is_playing() || wav_player_is_active()) {
                wav_player_stop(); audio_engine_stop();
            }
        }
        last_audio_state = current_state;

        if (volume != last_audio_vol) {
            audio_engine_set_volume(volume);
            last_audio_vol = volume;
        }

        /* 3. Flicker-Free Differential Screen Update */
        uint16_t current_note = audio_engine_get_note_index();
        const audio_diagnostics_t *diag = audio_get_diagnostics();
        uint32_t current_samples = diag->i2s_tx_samples;
        bool current_buzzer = audio_engine_is_buzzer_muted();
        audio_waveform_t current_wave = audio_hardware_dac_get_waveform();
        sd_reader_mode_t current_sd_mode = sd_card_get_mode();

        bool full_redraw = force_redraw ||
                           (current_state != last_rendered_state) ||
                           (current_song != last_rendered_song) ||
                           (current_sd_mode != last_rendered_sd_mode);
        bool vol_changed = (volume != last_rendered_vol);
        bool note_changed = (current_note != last_rendered_note);
        bool diag_changed = (current_samples / 5000U != last_rendered_samples / 5000U) ||
                            (current_buzzer != last_rendered_buzzer) ||
                            (current_wave != last_rendered_wave);

        if (full_redraw) {
            if (k_mutex_lock(&g_lcd_mutex, K_MSEC(50)) == 0) {
                render_full_screen(current_state, current_song, volume);
                last_rendered_state = current_state;
                last_rendered_song = current_song;
                last_rendered_vol = volume;
                last_rendered_note = current_note;
                last_rendered_samples = current_samples;
                last_rendered_buzzer = current_buzzer;
                last_rendered_wave = current_wave;
                last_rendered_sd_mode = current_sd_mode;
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
            if (diag_changed) {
                if (k_mutex_lock(&g_lcd_mutex, K_MSEC(50)) == 0) {
                    render_diag_box();
                    last_rendered_samples = current_samples;
                    last_rendered_buzzer = current_buzzer;
                    last_rendered_wave = current_wave;
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

    static bool s_both_ud_active = false;
    static bool s_both_ud_triggered = false;
    static uint32_t s_both_ud_start_ms = 0;
    static uint8_t s_both_ud_stable = 0;

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

        bool both_ud_raw = (up_active && down_active);

        /* Dual-button detection: hold UP + DOWN together (>= 450 ms) to cycle waveform */
        if (both_ud_raw) {
            if (!s_both_ud_active) {
                if (s_both_ud_stable < 3) {
                    s_both_ud_stable++;
                    if (s_both_ud_stable == 3) {
                        s_both_ud_active = true;
                        s_both_ud_start_ms = now;
                        s_both_ud_triggered = false;
                    }
                }
            } else {
                if (!s_both_ud_triggered && (now - s_both_ud_start_ms) >= 450) {
                    s_both_ud_triggered = true;
                    audio_hardware_dac_cycle_waveform();
                    k_mutex_lock(&g_player_mutex, K_FOREVER);
                    g_player.state_changed = true;
                    k_mutex_unlock(&g_player_mutex);

                    printk("[Audio] WAVEFORM CYCLED -> %s!\n", audio_hardware_dac_get_waveform_name());
                    uart1_direct_print("[Audio] WAVEFORM CYCLED -> ");
                    uart1_direct_print(audio_hardware_dac_get_waveform_name());
                    uart1_direct_print("!\n");
                }
            }
            btn_up.long_triggered = true;
            btn_down.long_triggered = true;
        } else {
            if (s_both_ud_triggered) {
                btn_up.long_triggered = true;
                btn_down.long_triggered = true;
            }
            s_both_ud_active = false;
            s_both_ud_stable = 0;
            s_both_ud_start_ms = 0;
            if (!up_active && !down_active) {
                s_both_ud_triggered = false;
            }
        }

        button_event_t up_evt    = update_button_state(&btn_up, up_active, now, 450, 0);
        button_event_t down_evt  = update_button_state(&btn_down, down_active, now, 450, 0);
        button_event_t press_evt = update_button_state(&btn_press, press_active, now, 500, 0);

        if (sd_card_get_mode() == SD_MODE_USB_CARD_READER) {
            /* While in USB Card Reader mode, pressing UP, DOWN, or PA0 exits back to player! */
            if (up_evt == BTN_EVT_SHORT_PRESS || down_evt == BTN_EVT_SHORT_PRESS || press_evt == BTN_EVT_SHORT_PRESS) {
                sd_card_set_mode(SD_MODE_STANDALONE);
                k_mutex_lock(&g_player_mutex, K_FOREVER);
                g_player.state_changed = true;
                k_mutex_unlock(&g_player_mutex);
            }
        } else if (!both_ud_raw && !s_both_ud_triggered) {
            /* 1. UP Button Events */
            if (up_evt == BTN_EVT_SHORT_PRESS) {
                /* Short Click: Next Track (+1) */
                k_mutex_lock(&g_player_mutex, K_FOREVER);
                g_player.current_song_index = (g_player.current_song_index + 1) % (8 + sd_card_get_track_count());
                g_player.state_changed = true;
                const song_info_t *s = get_song_info(g_player.current_song_index);
                printk("[Nav] UP (Click) -> Next Track [%u/8]: %s %s\n",
                       g_player.current_song_index + 1, s->name1, s->name2);
                k_mutex_unlock(&g_player_mutex);
            } else if (up_evt == BTN_EVT_LONG_PRESS) {
                /* Long Press on UP: Enter USB Card Reader mode! */
                wav_player_stop(); audio_engine_stop();
                k_mutex_lock(&g_player_mutex, K_FOREVER);
                g_player.state = PLAYER_STATE_STOPPED;
                g_player.state_changed = true;
                k_mutex_unlock(&g_player_mutex);
                sd_card_set_mode(SD_MODE_USB_CARD_READER);
                printk("[Nav] UP (Hold) -> Entered USB SD Card Reader Mode!\n");
                uart1_direct_print("[Nav] UP (Hold) -> Entered USB SD Card Reader Mode!\n");
            }

            /* 2. DOWN Button Events */
            if (down_evt == BTN_EVT_SHORT_PRESS) {
                /* Short Click: Previous Track (-1) */
                k_mutex_lock(&g_player_mutex, K_FOREVER);
                g_player.current_song_index = (g_player.current_song_index + (8 + sd_card_get_track_count()) - 1) % (8 + sd_card_get_track_count());
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
        }

        /* 3. PRESS / PA0 (USER_BUTTON) Events */
        if (press_evt == BTN_EVT_SHORT_PRESS) {
            if (sd_card_get_mode() == SD_MODE_STANDALONE) {
                /* Short Click: Toggle Play / Pause */
                k_mutex_lock(&g_player_mutex, K_FOREVER);
                g_player.state = toggle_play_pause(g_player.state);
                g_player.state_changed = true;
                const song_info_t *s = get_song_info(g_player.current_song_index);
                printk("[Nav] USER_BUTTON (Click) -> Track [%u] '%s %s' is now %s\n",
                       g_player.current_song_index + 1, s->name1, s->name2,
                       get_player_state_str(g_player.state));
                k_mutex_unlock(&g_player_mutex);
            }
        } else if (press_evt == BTN_EVT_LONG_PRESS) {
            /* Long Press: Stop playback */
            wav_player_stop(); audio_engine_stop();
            k_mutex_lock(&g_player_mutex, K_FOREVER);
            g_player.state = PLAYER_STATE_STOPPED;
            g_player.state_changed = true;
            k_mutex_unlock(&g_player_mutex);
            printk("[Nav] USER_BUTTON (Hold) -> Stopped.\n");
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

    static bool s_both_lr_active = false;
    static bool s_both_lr_triggered = false;
    static uint32_t s_both_lr_start_ms = 0;
    static uint8_t s_both_lr_stable = 0;

    while (1) {
        uint32_t now = k_uptime_get_32();

        /* Sample physical pins:
         * PC0 (LEFT): Volume Down (active LOW)
         * PC4 (RIGHT): Volume Up (active LOW)
         * PA1 (AUX): Optional Preset Cycle / Mute (active LOW)
         */
        bool left_active  = ((GPIOC->IDR & (1U << 0)) == 0);
        bool right_active = ((GPIOC->IDR & (1U << 4)) == 0);
        bool down_active  = ((GPIOC->IDR & (1U << 1)) == 0);
        bool aux_active   = ((GPIOA->IDR & (1U << 1)) == 0);

        bool both_raw = (left_active && right_active);
        bool triple_fmt_raw = (left_active && right_active && down_active);

        static bool s_fmt_active = false;
        static bool s_fmt_triggered = false;
        static uint32_t s_fmt_start_ms = 0;

        /* 1. Triple-button detection: hold LEFT + RIGHT + DOWN (>= 1200 ms) to FORMAT SD to FAT32 */
        if (triple_fmt_raw) {
            if (!s_fmt_active) {
                s_fmt_active = true;
                s_fmt_start_ms = now;
                s_fmt_triggered = false;
            } else if (!s_fmt_triggered && (now - s_fmt_start_ms) >= 1200) {
                s_fmt_triggered = true;
                wav_player_stop(); audio_engine_stop();
                k_mutex_lock(&g_lcd_mutex, K_FOREVER);
                lcd_show_string(12, 172, "FORMATTING FAT32... ", LCD_COLOR_YELLOW, LCD_COLOR_DARKGREY);
                k_mutex_unlock(&g_lcd_mutex);

                int fmt_res = sd_card_format_fat32();

                k_mutex_lock(&g_player_mutex, K_FOREVER);
                g_player.state_changed = true;
                k_mutex_unlock(&g_player_mutex);

                if (fmt_res == 0) {
                    printk("[SD] Micro-SD formatted to FAT32 successfully!\n");
                    uart1_direct_print("[SD] Micro-SD formatted to FAT32 successfully!\n");
                }
            }
            btn_left.long_triggered = true;
            btn_right.long_triggered = true;
        } else {
            s_fmt_active = false;
            s_fmt_start_ms = 0;
            if (!left_active && !right_active && !down_active) {
                s_fmt_triggered = false;
            }

            /* 2. Dual-button detection: hold LEFT + RIGHT together (>= 450 ms) to toggle buzzer mute */
            if (both_raw) {
                if (!s_both_lr_active) {
                    if (s_both_lr_stable < 3) {
                        s_both_lr_stable++;
                        if (s_both_lr_stable == 3) {
                            s_both_lr_active = true;
                            s_both_lr_start_ms = now;
                            s_both_lr_triggered = false;
                        }
                    }
                } else {
                    if (!s_both_lr_triggered && (now - s_both_lr_start_ms) >= 450) {
                        s_both_lr_triggered = true;
                        bool muted = audio_engine_toggle_buzzer();
                        k_mutex_lock(&g_player_mutex, K_FOREVER);
                        g_player.state_changed = true;
                        k_mutex_unlock(&g_player_mutex);

                        if (muted) {
                            printk("[Audio] BUZZER MUTED (Headphone Only Mode)!\n");
                            uart1_direct_print("[Audio] BUZZER MUTED (Headphone Only Mode)!\n");
                        } else {
                            printk("[Audio] BUZZER ENABLED (Dual Audio Mode)!\n");
                            uart1_direct_print("[Audio] BUZZER ENABLED (Dual Audio Mode)!\n");
                        }
                    }
                }
                btn_left.long_triggered = true;
                btn_right.long_triggered = true;
            } else {
                if (s_both_lr_triggered) {
                    btn_left.long_triggered = true;
                    btn_right.long_triggered = true;
                }
                s_both_lr_active = false;
                s_both_lr_stable = 0;
                s_both_lr_start_ms = 0;
                if (!left_active && !right_active) {
                    s_both_lr_triggered = false;
                }
            }
        }

        button_event_t left_evt  = update_button_state(&btn_left, left_active, now, 400, 90);
        button_event_t right_evt = update_button_state(&btn_right, right_active, now, 400, 90);
        button_event_t aux_evt   = update_button_state(&btn_aux, aux_active, now, 450, 0);

        if (!both_raw && !s_both_lr_triggered) {
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
        }

        /* 3. AUX Button Events (PA1) */
        if (aux_evt == BTN_EVT_SHORT_PRESS) {
            /* Short Click: Cycle Waveform (SINE -> TRIANGLE -> SAWTOOTH -> SQUARE) */
            audio_hardware_dac_cycle_waveform();
            k_mutex_lock(&g_player_mutex, K_FOREVER);
            g_player.state_changed = true;
            printk("[Audio] AUX (Click) -> Waveform: %s\n", audio_hardware_dac_get_waveform_name());
            uart1_direct_print("[Audio] AUX (Click) -> Waveform: ");
            uart1_direct_print(audio_hardware_dac_get_waveform_name());
            uart1_direct_print("\n");
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
