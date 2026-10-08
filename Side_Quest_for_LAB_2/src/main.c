/**
 * @file main.c
 * @brief Standalone MIDI Karaoke Player with Yamaha FM Synthesis and ST7789 UI.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include "app_config.h"
#include "audio_codec_es8388.h"
#include "yamaha_fm_synth.h"
#include "midi_karaoke_parser.h"
#include "karaoke_ui.h"
#include "karaoke_catalog.h"
#include "sd_card_reader.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <stm32f4xx.h>
#include <stdio.h>
#include <string.h>

/* Global Application State */
static struct {
    uint32_t current_song_index;
    ui_view_mode_t ui_mode;
    uint8_t browser_cursor;
    uint32_t browser_page;

    /* Number Select Mode State (5-digit input) */
    uint8_t num_digits[5];
    uint8_t num_cursor;

    struct k_mutex lock;
} s_app;

/* Hardware Peripheral GPIOs (D-Pad, LEDs, USART1) */
static void init_board_peripherals(void)
{
    /* 1. Clocks */
    RCC->AHB1ENR |= (RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOCEN |
                     RCC_AHB1ENR_GPIOEEN | RCC_AHB1ENR_GPIOFEN);

    /* 2. Status LEDs: PF11 (Blue), PF12 (Red), PE3 (Green) */
    GPIOF->MODER = (GPIOF->MODER & ~((3U << 22) | (3U << 24))) |
                   ((1U << 22) | (1U << 24));
    GPIOF->OTYPER &= ~((1U << 11) | (1U << 12));
    GPIOF->OSPEEDR |= ((3U << 22) | (3U << 24));
    GPIOF->PUPDR &= ~((3U << 22) | (3U << 24));

    GPIOE->MODER = (GPIOE->MODER & ~(3U << 6)) | (1U << 6);
    GPIOE->OTYPER &= ~(1U << 3);
    GPIOE->OSPEEDR |= (3U << 6);
    GPIOE->PUPDR &= ~(3U << 6);

    /* Turn off all LEDs initially (active LOW) */
    GPIOF->BSRR = (1U << 11) | (1U << 12);
    GPIOE->BSRR = (1U << 3);

    /* 3. D-Pad on GPIOC: PC5 (UP), PC1 (DOWN), PC0 (LEFT), PC4 (RIGHT) */
    static const uint8_t dpad_pins[] = {0, 1, 4, 5};
    for (size_t i = 0; i < sizeof(dpad_pins); i++) {
        uint32_t pin = dpad_pins[i];
        GPIOC->MODER &= ~(3U << (pin * 2));
        GPIOC->PUPDR = (GPIOC->PUPDR & ~(3U << (pin * 2))) | (1U << (pin * 2));
    }

    /* 4. USER Button on PA0 (Active HIGH, Pull-down) */
    GPIOA->MODER &= ~(3U << 0);
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(3U << 0)) | (2U << 0);

    /* 5. AUX Button on PA1 (Active LOW, Pull-up) */
    GPIOA->MODER &= ~(3U << 2);
    GPIOA->PUPDR = (GPIOA->PUPDR & ~(3U << 2)) | (1U << 2);
}

static void update_status_led(bool is_playing)
{
    if (is_playing) {
        GPIOF->BSRR = (1U << (11 + 16)); /* Blue ON */
        GPIOF->BSRR = (1U << 12);        /* Red OFF */
    } else {
        GPIOF->BSRR = (1U << 11);        /* Blue OFF */
        GPIOF->BSRR = (1U << (12 + 16)); /* Red ON */
    }
}

static void play_song(uint32_t index)
{
    song_entry_t s;
    if (!karaoke_catalog_get_song(index, &s)) return;

    const uint8_t *midi_data = NULL;
    uint32_t midi_len = 0;
    if (!karaoke_catalog_load_midi_data(index, &midi_data, &midi_len)) {
        printk("[App] Failed to load song data for index %u\n", (unsigned)index);
        return;
    }

    /* Stop the sequencer first so no event arrives while the channels are reset. */
    midi_karaoke_stop();
    yamaha_fm_synth_reset();
    midi_karaoke_load_memory(midi_data, midi_len);
    karaoke_ui_set_current_song(&s);
    midi_karaoke_play();
    printk("[App] Started playing: #%u - %s (%s)\n", (unsigned)s.song_code, s.title, s.singer);
}

/* Helper to convert 5 digits array to numeric song code */
static uint32_t digits_to_code(const uint8_t *digits)
{
    return (uint32_t)digits[0] * 10000U +
           (uint32_t)digits[1] * 1000U  +
           (uint32_t)digits[2] * 100U   +
           (uint32_t)digits[3] * 10U    +
           (uint32_t)digits[4];
}

/* Helper to load numeric code into 5 digits array */
static void code_to_digits(uint32_t code, uint8_t *digits)
{
    digits[0] = (uint8_t)((code / 10000U) % 10U);
    digits[1] = (uint8_t)((code / 1000U) % 10U);
    digits[2] = (uint8_t)((code / 100U) % 10U);
    digits[3] = (uint8_t)((code / 10U) % 10U);
    digits[4] = (uint8_t)(code % 10U);
}

/* -------------------------------------------------------------------------- */
/* Thread: UI Display Refresh                                                 */
/* -------------------------------------------------------------------------- */
static void ui_refresh_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    while (1) {
        k_mutex_lock(&s_app.lock, K_FOREVER);
        ui_view_mode_t mode = s_app.ui_mode;
        uint8_t cursor = s_app.browser_cursor;
        uint32_t page = s_app.browser_page;
        uint8_t num_digits[5];
        memcpy(num_digits, s_app.num_digits, 5);
        uint8_t num_cursor = s_app.num_cursor;
        k_mutex_unlock(&s_app.lock);

        if (mode == UI_VIEW_NUMBER_SELECT) {
            uint32_t search_code = digits_to_code(num_digits);
            uint32_t match_idx = 0;
            bool found = karaoke_catalog_find_by_code(search_code, &match_idx);

            song_entry_t preview;
            if (found && karaoke_catalog_get_song(match_idx, &preview)) {
                karaoke_ui_render_number_select(num_digits, num_cursor, preview.title, preview.singer, true);
            } else {
                karaoke_ui_render_number_select(num_digits, num_cursor, NULL, NULL, false);
            }
        } else if (mode == UI_VIEW_BROWSER) {
            song_entry_t page_songs[5];
            uint8_t count = 0;
            uint32_t total = karaoke_catalog_get_total_songs();
            uint32_t total_pages = (total + 4) / 5;
            if (total_pages == 0) total_pages = 1;

            uint32_t start_idx = page * 5;
            for (uint8_t i = 0; i < 5; i++) {
                if (start_idx + i < total) {
                    if (karaoke_catalog_get_song(start_idx + i, &page_songs[i])) {
                        count++;
                    }
                }
            }
            karaoke_ui_render_browser(page_songs, count, cursor, page + 1, total_pages);
        } else {
            midi_player_status_t status;
            midi_karaoke_get_status(&status);

            const audio_diagnostics_t *diag = audio_get_diagnostics();
            uint8_t voices = yamaha_fm_get_active_voice_count();
            uint8_t vol = audio_get_volume();

            karaoke_ui_update_player(&status, diag->peak_left, diag->peak_right, voices, vol);
            audio_reset_peak_meters();
            update_status_led(status.is_playing && !status.is_paused);
        }

        k_msleep(UI_THREAD_PERIOD_MS);
    }
}

K_THREAD_STACK_DEFINE(s_ui_stack, UI_THREAD_STACK_SIZE);
static struct k_thread s_ui_thread_data;

/* -------------------------------------------------------------------------- */
/* Thread: Button Polling & User Navigation                                   */
/* -------------------------------------------------------------------------- */
static void button_poll_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    static bool last_up = false;
    static bool last_dn = false;
    static bool last_lt = false;
    static bool last_rt = false;
    static bool last_press = false;
    static bool last_aux = false;

    static uint32_t up_hold_ticks = 0;
    static uint32_t dn_hold_ticks = 0;
    static uint32_t both_ud_hold_ticks = 0;

    while (1) {
        bool up = ((GPIOC->IDR & (1U << 5)) == 0);
        bool dn = ((GPIOC->IDR & (1U << 1)) == 0);
        bool lt = ((GPIOC->IDR & (1U << 0)) == 0);
        bool rt = ((GPIOC->IDR & (1U << 4)) == 0);
        bool press = ((GPIOA->IDR & (1U << 0)) != 0);
        bool aux = ((GPIOA->IDR & (1U << 1)) == 0);

        /* 1. Simultaneous UP + DOWN Detection (Hold or Press both to enter NUMBER SELECT) */
        if (up && dn) {
            both_ud_hold_ticks++;
            if (both_ud_hold_ticks == 10) { /* ~200 ms debounce */
                k_mutex_lock(&s_app.lock, K_FOREVER);
                if (s_app.ui_mode != UI_VIEW_NUMBER_SELECT) {
                    s_app.ui_mode = UI_VIEW_NUMBER_SELECT;
                    song_entry_t cur;
                    if (karaoke_catalog_get_song(s_app.current_song_index, &cur)) {
                        code_to_digits(cur.song_code, s_app.num_digits);
                    }
                    s_app.num_cursor = 4; /* Focus last digit */
                    karaoke_ui_set_view(UI_VIEW_NUMBER_SELECT);
                    printk("[Nav] Entered NUMBER SELECT mode!\n");
                }
                k_mutex_unlock(&s_app.lock);
            }
        } else {
            both_ud_hold_ticks = 0;
        }

        /* 2. AUX Button: Toggle / Cancel */
        if (aux && !last_aux) {
            k_mutex_lock(&s_app.lock, K_FOREVER);
            if (s_app.ui_mode == UI_VIEW_NUMBER_SELECT) {
                /* Exit Number Select back to Player */
                s_app.ui_mode = UI_VIEW_PLAYING;
                karaoke_ui_set_view(UI_VIEW_PLAYING);
            } else if (s_app.ui_mode == UI_VIEW_PLAYING) {
                s_app.ui_mode = UI_VIEW_BROWSER;
                karaoke_ui_set_view(UI_VIEW_BROWSER);
            } else {
                s_app.ui_mode = UI_VIEW_PLAYING;
                karaoke_ui_set_view(UI_VIEW_PLAYING);
            }
            k_mutex_unlock(&s_app.lock);
        }

        /* Handle mode-specific actions (only if not holding both UP+DOWN) */
        if (!(up && dn)) {
            k_mutex_lock(&s_app.lock, K_FOREVER);
            ui_view_mode_t mode = s_app.ui_mode;

            if (mode == UI_VIEW_NUMBER_SELECT) {
                /* LEFT / RIGHT: Move Digit Cursor */
                if (lt && !last_lt) {
                    s_app.num_cursor = (s_app.num_cursor + 4) % 5; /* Left */
                }
                if (rt && !last_rt) {
                    s_app.num_cursor = (s_app.num_cursor + 1) % 5; /* Right */
                }

                /* UP / DOWN Click: Scroll Digit 0-9 */
                if (up && !last_up) {
                    s_app.num_digits[s_app.num_cursor] = (s_app.num_digits[s_app.num_cursor] + 1) % 10;
                }
                if (dn && !last_dn) {
                    s_app.num_digits[s_app.num_cursor] = (s_app.num_digits[s_app.num_cursor] + 9) % 10;
                }

                /* Track UP / DOWN Hold Duration for Finish & Search */
                if (up) up_hold_ticks++; else up_hold_ticks = 0;
                if (dn) dn_hold_ticks++; else dn_hold_ticks = 0;

                bool submit_search = (press && !last_press) ||
                                     (up_hold_ticks == 25) ||  /* ~500 ms hold */
                                     (dn_hold_ticks == 25);

                if (submit_search) {
                    uint32_t target_code = digits_to_code(s_app.num_digits);
                    uint32_t found_idx = 0;
                    if (karaoke_catalog_find_by_code(target_code, &found_idx)) {
                        s_app.current_song_index = found_idx;
                        s_app.ui_mode = UI_VIEW_PLAYING;
                        karaoke_ui_set_view(UI_VIEW_PLAYING);
                        play_song(s_app.current_song_index);
                        printk("[Nav] Found & Playing Song Code: #%u\n", (unsigned)target_code);
                    } else {
                        printk("[Nav] Song Code #%u not found in catalog!\n", (unsigned)target_code);
                    }
                }
            } else if (mode == UI_VIEW_BROWSER) {
                /* Browser Mode */
                uint32_t total = karaoke_catalog_get_total_songs();
                uint32_t total_pages = (total + 4) / 5;

                if (up && !last_up) {
                    if (s_app.browser_cursor > 0) {
                        s_app.browser_cursor--;
                    } else if (s_app.browser_page > 0) {
                        s_app.browser_page--;
                        s_app.browser_cursor = 4;
                    }
                }
                if (dn && !last_dn) {
                    if (s_app.browser_cursor < 4 && (s_app.browser_page * 5 + s_app.browser_cursor + 1) < total) {
                        s_app.browser_cursor++;
                    } else if (s_app.browser_page + 1 < total_pages) {
                        s_app.browser_page++;
                        s_app.browser_cursor = 0;
                    }
                }
                if (press && !last_press) {
                    uint32_t sel = s_app.browser_page * 5 + s_app.browser_cursor;
                    if (sel < total) {
                        s_app.current_song_index = sel;
                        s_app.ui_mode = UI_VIEW_PLAYING;
                        karaoke_ui_set_view(UI_VIEW_PLAYING);
                        play_song(s_app.current_song_index);
                    }
                }
            } else {
                /* Player Mode (UI_VIEW_PLAYING) */
                /* Volume: LEFT / RIGHT */
                if (lt && !last_lt) {
                    uint8_t v = audio_get_volume();
                    if (v >= 5) audio_set_volume(v - 5);
                }
                if (rt && !last_rt) {
                    uint8_t v = audio_get_volume();
                    if (v <= 95) audio_set_volume(v + 5);
                }

                /* Next / Prev Song: UP / DOWN */
                if (up && !last_up) {
                    if (s_app.current_song_index > 0) {
                        s_app.current_song_index--;
                        play_song(s_app.current_song_index);
                    }
                }
                if (dn && !last_dn) {
                    uint32_t total = karaoke_catalog_get_total_songs();
                    if (s_app.current_song_index + 1 < total) {
                        s_app.current_song_index++;
                        play_song(s_app.current_song_index);
                    }
                }

                /* Play / Pause Click: PA0 */
                if (press && !last_press) {
                    midi_player_status_t status;
                    midi_karaoke_get_status(&status);
                    if (status.is_playing) {
                        midi_karaoke_pause();
                    } else {
                        play_song(s_app.current_song_index);
                    }
                }
            }
            k_mutex_unlock(&s_app.lock);
        }

        last_up = up;
        last_dn = dn;
        last_lt = lt;
        last_rt = rt;
        last_press = press;
        last_aux = aux;

        k_msleep(BUTTON_POLL_PERIOD_MS);
    }
}
K_THREAD_STACK_DEFINE(s_btn_stack, BUTTON_THREAD_STACK_SIZE);
static struct k_thread s_btn_thread_data;

/* -------------------------------------------------------------------------- */
/* Main Entry Point                                                           */
/* -------------------------------------------------------------------------- */
int main(void)
{
    printk("\n==================================================\n");
    printk("  BCA182: RT-Spark MIDI Karaoke Player Starting   \n");
    printk("  Yamaha FM Synth (30 Voices) + ST7789 IPS LCD    \n");
    printk("==================================================\n");

    k_mutex_init(&s_app.lock);
    s_app.current_song_index = 0;
    s_app.ui_mode = UI_VIEW_PLAYING;
    s_app.browser_cursor = 0;
    s_app.browser_page = 0;
    s_app.num_cursor = 4;
    memset(s_app.num_digits, 0, sizeof(s_app.num_digits));

    init_board_peripherals();

    /* 1. Initialize ST7789 IPS LCD (FSMC parallel bus) */
    karaoke_ui_init();

    /* 2. Initialize Yamaha FM Synthesizer & MIDI Sequencer */
    yamaha_fm_synth_init(AUDIO_SAMPLE_RATE);
    midi_karaoke_init(yamaha_fm_get_callbacks());

    /* 3. Initialize ES8388 Audio Codec & Hook FM Synthesizer Callback */
    audio_set_pcm_callback(yamaha_fm_synth_render);
    audio_hardware_init();
    audio_set_volume(80);

    /* 4. Initialize SD Card & Song Catalog */
    sd_card_reader_init();
    karaoke_catalog_init();

    /* 5. Start playing first song immediately (KARAOKE_BOOT_SONG selects another for bench builds) */
    k_msleep(100);
    play_song(KARAOKE_BOOT_SONG);
    s_app.current_song_index = KARAOKE_BOOT_SONG;

    /* 6. Spawn Application Background Threads */
    printk("[System] Starting UI and Button threads...\n");
    k_thread_create(&s_btn_thread_data, s_btn_stack,
                    K_THREAD_STACK_SIZEOF(s_btn_stack),
                    button_poll_thread, NULL, NULL, NULL,
                    BUTTON_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&s_btn_thread_data, "btn_task");

    k_thread_create(&s_ui_thread_data, s_ui_stack,
                    K_THREAD_STACK_SIZEOF(s_ui_stack),
                    ui_refresh_thread, NULL, NULL, NULL,
                    UI_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&s_ui_thread_data, "ui_task");

    printk("[App] System fully operational!\n");

    while (1) {
        k_sleep(K_FOREVER);
    }
    return 0;
}
