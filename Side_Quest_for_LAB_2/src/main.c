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
#include "karaoke_settings.h"
#include "sd_card_reader.h"
#include "random_play.h"

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/printk.h>
#include <stm32f4xx.h>
#include <stdio.h>
#include <string.h>

static int configure_overclock_flash_latency(void)
{
    /* Set 7 Flash wait states for 210 MHz HCLK operation with ART I/D cache */
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) |
                 FLASH_ACR_LATENCY_7WS |
                 FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN;
    return 0;
}
SYS_INIT(configure_overclock_flash_latency, PRE_KERNEL_1, 2);

/* Global Application State */
static struct {
    uint32_t current_song_index;
    ui_view_mode_t ui_mode;
    uint8_t browser_cursor;
    uint32_t browser_page;

    /* Number Select Mode State (5-digit input) */
    uint8_t num_digits[5];
    uint8_t num_cursor;

    /* Settings View State */
    uint8_t settings_cursor;

    /* Random play: songs picked lately, when a button was last touched, last automatic pick */
    random_play_t random;
    uint32_t last_input_ms;
    uint32_t last_auto_ms;

    struct k_mutex lock;
} s_app;

/* Pause between automatic picks, so a song that will not load cannot make the player spin. */
#define AUTO_PICK_MIN_GAP_MS    1000U

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

static bool play_song(uint32_t index)
{
    song_entry_t s;
    if (!karaoke_catalog_get_song(index, &s)) return false;

    /* Immediately paint the new song title & [Loading Song...] on the LCD (zero black screen delay!) */
    karaoke_ui_set_current_song(&s);

    /* Stop the sequencer first so that it reads nothing from the file about to be closed. */
    midi_karaoke_stop();
    yamaha_fm_synth_reset();

    midi_source_t src;
    if (!karaoke_catalog_open_song(index, &src) || !midi_karaoke_load(&src)) {
        printk("[App] Failed to load song data for index %u\n", (unsigned)index);
        return false;
    }
    yamaha_fm_set_melody_channel(midi_karaoke_get_melody_channel());
    midi_karaoke_play();
    random_play_note(&s_app.random, index);
    printk("[App] Started playing: #%u - %s (%s)\n", (unsigned)s.song_code, s.title, s.singer);
    return true;
}

/* Hardware RNG through Zephyr's generator, mixed with the cycle counter so that a missing or
 * failed RNG still gives a different start after every reset. */
static uint32_t random_u32(void)
{
    return sys_rand32_get() ^ (k_cycle_get_32() * 2654435761U);
}

/* Picks a song nobody played lately and starts it. Call with s_app.lock held (or before the
 * threads exist). */
static bool play_random_song(void)
{
    uint32_t total = karaoke_catalog_get_total_songs();

    if (total == 0U) {
        return false;
    }
    uint32_t index = random_play_pick(&s_app.random, total, random_u32());

    s_app.current_song_index = index;
    printk("[App] Random pick: catalog index %u of %u\n", (unsigned)index, (unsigned)total);
    return play_song(index);
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
        uint8_t set_cursor = s_app.settings_cursor;
        k_mutex_unlock(&s_app.lock);

        if (mode == UI_VIEW_SETTINGS) {
            const karaoke_settings_t *cfg = karaoke_settings_get();
            karaoke_ui_render_settings(set_cursor, cfg->master_volume, cfg->instrument_gain, cfg->drum_gain, cfg->melody_gain);
        } else if (mode == UI_VIEW_NUMBER_SELECT) {
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

    static uint32_t up_hold_ticks = 0;
    static uint32_t dn_hold_ticks = 0;
    static uint32_t both_ud_hold_ticks = 0;
    static uint32_t both_lr_hold_ticks = 0;
    static uint32_t press_hold_ticks = 0;

    while (1) {
        bool up = ((GPIOC->IDR & (1U << 5)) == 0);
        bool dn = ((GPIOC->IDR & (1U << 1)) == 0);
        bool lt = ((GPIOC->IDR & (1U << 0)) == 0);
        bool rt = ((GPIOC->IDR & (1U << 4)) == 0);
        bool press = ((GPIOA->IDR & (1U << 0)) != 0);

        if (up || dn || lt || rt || press) {
            s_app.last_input_ms = k_uptime_get_32();
        }

        /* 1. Simultaneous UP + DOWN Detection -> Enter NUMBER SELECT Mode */
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

        /* 2. Simultaneous LEFT + RIGHT Detection -> Enter AUDIO SETTINGS Mixer */
        if (lt && rt) {
            both_lr_hold_ticks++;
            if (both_lr_hold_ticks == 10) { /* ~200 ms debounce */
                k_mutex_lock(&s_app.lock, K_FOREVER);
                if (s_app.ui_mode != UI_VIEW_SETTINGS) {
                    s_app.ui_mode = UI_VIEW_SETTINGS;
                    s_app.settings_cursor = 0;
                    karaoke_ui_set_view(UI_VIEW_SETTINGS);
                    printk("[Nav] Entered AUDIO SETTINGS mode!\n");
                }
                k_mutex_unlock(&s_app.lock);
            }
        } else {
            both_lr_hold_ticks = 0;
        }

        /* 3. CENTER / USER Button (PA0): Click = Play/Pause/Action, Long-Press = Toggle Browser / Return */
        if (press) {
            press_hold_ticks++;
        } else {
            if (last_press) {
                if (press_hold_ticks >= 20) { /* Long press >= 400 ms */
                    k_mutex_lock(&s_app.lock, K_FOREVER);
                    if (s_app.ui_mode == UI_VIEW_PLAYING) {
                        s_app.ui_mode = UI_VIEW_BROWSER;
                        karaoke_ui_set_view(UI_VIEW_BROWSER);
                        printk("[Nav] Opened Song Browser via Long-Press OK!\n");
                    } else if (s_app.ui_mode == UI_VIEW_SETTINGS) {
                        karaoke_settings_save();
                        s_app.ui_mode = UI_VIEW_PLAYING;
                        karaoke_ui_set_view(UI_VIEW_PLAYING);
                    } else {
                        s_app.ui_mode = UI_VIEW_PLAYING;
                        karaoke_ui_set_view(UI_VIEW_PLAYING);
                    }
                    k_mutex_unlock(&s_app.lock);
                } else if (press_hold_ticks > 0) { /* Short click < 400 ms */
                    k_mutex_lock(&s_app.lock, K_FOREVER);
                    if (s_app.ui_mode == UI_VIEW_PLAYING) {
                        midi_player_status_t status;
                        midi_karaoke_get_status(&status);
                        if (status.is_playing) {
                            midi_karaoke_pause();
                        } else {
                            play_song(s_app.current_song_index);
                        }
                    } else if (s_app.ui_mode == UI_VIEW_SETTINGS) {
                        karaoke_settings_save();
                        s_app.ui_mode = UI_VIEW_PLAYING;
                        karaoke_ui_set_view(UI_VIEW_PLAYING);
                    } else if (s_app.ui_mode == UI_VIEW_BROWSER) {
                        uint32_t total = karaoke_catalog_get_total_songs();
                        uint32_t sel = s_app.browser_page * 5 + s_app.browser_cursor;
                        if (sel < total) {
                            s_app.current_song_index = sel;
                            s_app.ui_mode = UI_VIEW_PLAYING;
                            karaoke_ui_set_view(UI_VIEW_PLAYING);
                            play_song(s_app.current_song_index);
                        }
                    } else if (s_app.ui_mode == UI_VIEW_NUMBER_SELECT) {
                        uint32_t target_code = digits_to_code(s_app.num_digits);
                        uint32_t found_idx = 0;
                        if (karaoke_catalog_find_by_code(target_code, &found_idx)) {
                            s_app.current_song_index = found_idx;
                            s_app.ui_mode = UI_VIEW_PLAYING;
                            karaoke_ui_set_view(UI_VIEW_PLAYING);
                            play_song(s_app.current_song_index);
                        }
                    }
                    k_mutex_unlock(&s_app.lock);
                }
            }
            press_hold_ticks = 0;
        }

        /* 4. Single-Button Directional Actions (only if not holding multi-button combos) */
        if (!(up && dn) && !(lt && rt)) {
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

                if (up_hold_ticks == 25 || dn_hold_ticks == 25) { /* ~500 ms hold */
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
            } else if (mode == UI_VIEW_SETTINGS) {
                /* Audio Settings Mixer View */
                const karaoke_settings_t *cfg = karaoke_settings_get();
                uint8_t cur_vol  = cfg->master_volume;
                uint8_t cur_inst = cfg->instrument_gain;
                uint8_t cur_drum = cfg->drum_gain;
                uint8_t cur_mel  = cfg->melody_gain;

                if (up && !last_up) {
                    s_app.settings_cursor = (s_app.settings_cursor + 3) % 4;
                }
                if (dn && !last_dn) {
                    s_app.settings_cursor = (s_app.settings_cursor + 1) % 4;
                }

                if (lt && !last_lt) {
                    if (s_app.settings_cursor == 0) {
                        karaoke_settings_set_volume(cur_vol >= 5 ? cur_vol - 5 : 0);
                    } else if (s_app.settings_cursor == 1) {
                        karaoke_settings_set_instrument_gain(cur_inst >= 25 ? cur_inst - 5 : 20);
                    } else if (s_app.settings_cursor == 2) {
                        karaoke_settings_set_drum_gain(cur_drum >= 25 ? cur_drum - 5 : 20);
                    } else if (s_app.settings_cursor == 3) {
                        karaoke_settings_set_melody_gain(cur_mel >= 25 ? cur_mel - 5 : 20);
                    }
                }

                if (rt && !last_rt) {
                    if (s_app.settings_cursor == 0) {
                        karaoke_settings_set_volume(cur_vol <= 95 ? cur_vol + 5 : 100);
                    } else if (s_app.settings_cursor == 1) {
                        karaoke_settings_set_instrument_gain(cur_inst <= 195 ? cur_inst + 5 : 200);
                    } else if (s_app.settings_cursor == 2) {
                        karaoke_settings_set_drum_gain(cur_drum <= 195 ? cur_drum + 5 : 200);
                    } else if (s_app.settings_cursor == 3) {
                        karaoke_settings_set_melody_gain(cur_mel <= 245 ? cur_mel + 5 : 250);
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
            } else {
                /* Player Mode (UI_VIEW_PLAYING) */
                const karaoke_settings_t *cfg = karaoke_settings_get();

                /* Volume: LEFT (-5%) / RIGHT (+5%) */
                if (lt && !last_lt) {
                    uint8_t v = cfg->master_volume;
                    karaoke_settings_set_volume(v >= 5 ? v - 5 : 0);
                }
                if (rt && !last_rt) {
                    uint8_t v = cfg->master_volume;
                    karaoke_settings_set_volume(v <= 95 ? v + 5 : 100);
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
            }
            k_mutex_unlock(&s_app.lock);
        }

        /* 5. Nobody has touched a button for a while and the song is over: play another. */
        uint32_t now = k_uptime_get_32();
        if (random_play_idle_due(now, s_app.last_input_ms, true) &&
            (uint32_t)(now - s_app.last_auto_ms) >= AUTO_PICK_MIN_GAP_MS) {
            midi_player_status_t st;
            midi_karaoke_get_status(&st);
            if (!st.is_playing) {
                k_mutex_lock(&s_app.lock, K_FOREVER);
                s_app.last_auto_ms = now;
                if (s_app.ui_mode == UI_VIEW_SETTINGS) {
                    karaoke_settings_save();
                }
                if (play_random_song() && s_app.ui_mode != UI_VIEW_PLAYING) {
                    s_app.ui_mode = UI_VIEW_PLAYING;
                    karaoke_ui_set_view(UI_VIEW_PLAYING);
                }
                k_mutex_unlock(&s_app.lock);
            }
        }

        last_up = up;
        last_dn = dn;
        last_lt = lt;
        last_rt = rt;
        last_press = press;

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
    printk("  Yamaha FM Synth (70 Voices) @ %u MHz\n", (unsigned)(SystemCoreClock / 1000000U));
    printk("==================================================\n");

    k_mutex_init(&s_app.lock);
    random_play_reset(&s_app.random);
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

    /* 5. Initialize Persistent Audio Settings (survives reset & shutdown) */
    karaoke_settings_init();

    /* 6. Start a random song (a KARAOKE_BOOT_SONG of 0 or more picks a fixed one for bench builds) */
    k_msleep(100);
    s_app.last_input_ms = k_uptime_get_32();
    if (KARAOKE_BOOT_SONG >= 0) {
        s_app.current_song_index = (uint32_t)KARAOKE_BOOT_SONG;
        play_song(s_app.current_song_index);
    } else {
        play_random_song();
    }

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
