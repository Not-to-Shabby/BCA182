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
#include "karaoke_mixer.h"
#include "sensme.h"
#include "sd_card_reader.h"
#include "random_play.h"
#include "led_matrix.h"
#include "wifi_audio_receiver.h"

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

    /* Mixer View State */
    uint8_t mixer_cursor;

    /* SensMe View State */
    uint8_t sensme_cursor;

    /* Random play: songs picked lately, when a button was last touched, last automatic pick */
    random_play_t random;
    uint32_t last_input_ms;
    uint32_t last_auto_ms;

    struct k_mutex lock;
} s_app;

/* Pause between automatic picks, so a song that will not load cannot make the player spin. */
#define AUTO_PICK_MIN_GAP_MS    1000U

#if KARAOKE_STRESS_SWITCH_MS > 0
static struct {
    uint32_t switches;
    uint32_t failures;
    uint32_t last_ms;
} s_stress;
#endif

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
    karaoke_ui_set_current_song_index(index);
    karaoke_ui_set_current_song(&s);

    /* The song that was playing keeps the mix it was left with. */
    karaoke_mixer_leave();

    /* Stop the sequencer first so that it reads nothing from the file about to be closed. */
    midi_karaoke_stop();
    yamaha_fm_synth_reset();

    midi_source_t src;
    if (!karaoke_catalog_open_song(index, &src) || !midi_karaoke_load(&src)) {
        printk("[App] Failed to load song data for index %u\n", (unsigned)index);
        return false;
    }
    yamaha_fm_set_melody_channel(midi_karaoke_get_melody_channel());
    karaoke_mixer_song_started(s.song_code);
    {
        midi_usage_t usage;

        midi_karaoke_get_usage(&usage);
        karaoke_mixer_set_usage(usage.valid, usage.sections, usage.channels);
    }
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
    uint32_t index = random_play_pick_filtered(&s_app.random, total, random_u32(), sensme_matches_active_channel);

    s_app.current_song_index = index;
    printk("[App] Random pick: catalog index %u of %u (SensMe=%s)\n",
           (unsigned)index, (unsigned)total, sensme_channel_name(sensme_get_active_channel()));
    return play_song(index);
}

/* Steps forward (+1) or backward (-1) to the next catalog index that matches the active SensMe mood filter. */
static uint32_t find_adjacent_mood_song(uint32_t current, int dir)
{
    uint32_t total = karaoke_catalog_get_total_songs();
    if (total == 0U) return 0U;

    uint32_t cand = (current == KARAOKE_DOOM_SONG_INDEX) ? 0U : current;
    for (uint32_t i = 0; i < total; i++) {
        if (dir > 0) {
            cand = (cand + 1U) % total;
        } else {
            cand = (cand == 0U) ? (total - 1U) : (cand - 1U);
        }
        if (sensme_matches_active_channel(cand)) {
            return cand;
        }
    }
    return cand;
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
        uint8_t mix_cursor = s_app.mixer_cursor;
        mix_view_t mix_view = *karaoke_mixer_view();
        k_mutex_unlock(&s_app.lock);

        if (mode == UI_VIEW_WIFI_AUDIO) {
            wifi_track_meta_t meta;
            wifi_telemetry_t telem;
            int16_t pk_l = 0, pk_r = 0;
            wifi_audio_receiver_get_metadata(&meta);
            wifi_audio_receiver_get_telemetry(&telem);
            wifi_audio_get_peaks(&pk_l, &pk_r);
            karaoke_ui_render_wifi_audio(meta.title, meta.artist, telem.ip_addr, meta.format, pk_l, pk_r, audio_get_volume());
            led_matrix_update_instruments(-1, pk_l, pk_r);
        } else if (mode == UI_VIEW_SENSME) {
            karaoke_ui_render_sensme(s_app.sensme_cursor, sensme_get_active_channel(), s_app.current_song_index);
        } else if (mode == UI_VIEW_MIXER) {
            uint8_t programs[16];
            uint8_t levels[16];
            uint8_t drum_note = 0;

            yamaha_fm_get_channel_activity(programs, levels, &drum_note);
            karaoke_ui_render_mixer(mix_cursor, &mix_view, karaoke_mixer_get(), programs, levels,
                                    yamaha_fm_get_drum_parts(), karaoke_mixer_song_has_own_mix());
        } else if (mode == UI_VIEW_SETTINGS) {
            const karaoke_settings_t *cfg = karaoke_settings_get();
            karaoke_ui_render_settings(set_cursor, cfg->master_volume, cfg->instrument_gain, cfg->drum_gain, cfg->melody_gain, cfg->sound_bank);
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
            int16_t pk_l = diag->peak_left;
            int16_t pk_r = diag->peak_right;
            uint8_t voices = yamaha_fm_get_active_voice_count();
            uint8_t vol = audio_get_volume();

            karaoke_ui_update_player(&status, pk_l, pk_r, voices, vol);
            led_matrix_update_instruments(yamaha_fm_get_lead_channel(), pk_l, pk_r);
            audio_reset_peak_meters();
            update_status_led(status.is_playing && !status.is_paused);
        }

        if (mode != UI_VIEW_PLAYING) {
            const audio_diagnostics_t *diag = audio_get_diagnostics();
            led_matrix_update_instruments(yamaha_fm_get_lead_channel(), diag->peak_left, diag->peak_right);
        }

        k_msleep(UI_THREAD_PERIOD_MS);
    }
}

/* Set when the mixer opens or a button pair was pressed, cleared once all four directions are up. */
static bool s_mixer_latch;

/* The mixer row under the cursor (the cursor counts rows of this song's list, not MIX_ITEM_* numbers). */
static uint8_t mixer_item(void)
{
    const mix_view_t *v = karaoke_mixer_view();
    uint8_t pos = s_app.mixer_cursor;

    if (pos >= v->count) {
        pos = (uint8_t)(v->count - 1U);
    }
    return v->item[pos];
}

/* Called with s_app.lock held. */
static void open_sensme(void)
{
    s_app.sensme_cursor = sensme_get_active_channel();
    s_app.ui_mode = UI_VIEW_SENSME;
    karaoke_ui_set_view(UI_VIEW_SENSME);
    printk("[Nav] Entered SENSME mode!\n");
}

/* Both are called with s_app.lock held. */
static void open_mixer(void)
{
    s_mixer_latch = true;
    s_app.mixer_cursor = 0;
    s_app.ui_mode = UI_VIEW_MIXER;
    karaoke_ui_set_view(UI_VIEW_MIXER);
    printk("[Nav] Entered MIXER mode!\n");
}

static void close_mixer(void)
{
    karaoke_mixer_leave();
    s_app.ui_mode = UI_VIEW_PLAYING;
    karaoke_ui_set_view(UI_VIEW_PLAYING);
}

static void open_wifi_audio(void)
{
    s_app.ui_mode = UI_VIEW_WIFI_AUDIO;
    karaoke_ui_set_view(UI_VIEW_WIFI_AUDIO);
    printk("[Nav] Entered WIFI AUDIO mode!\n");
}

static void audio_master_render_callback(int16_t *buf, size_t samples)
{
    if (s_app.ui_mode == UI_VIEW_WIFI_AUDIO) {
        wifi_audio_render(buf, samples / 2U);
    } else {
        yamaha_fm_synth_render(buf, samples);
    }
}


#ifdef KARAOKE_MIXER_SELFTEST
/* Build with -DKARAOKE_MIXER_SELFTEST: eight seconds after boot the button thread runs the mixer
 * through a full save, reload and delete on the real card, then leaves the answers here for GDB. */
volatile struct {
    uint32_t stage;         /* 0 not run, 1 running, 2 finished */
    uint32_t saved_ok;      /* the song's file exists after leaving the mixer */
    uint32_t reload_ok;     /* the values came back after starting the song again */
    uint32_t deleted_ok;    /* the file is gone after resetting the song */
    uint32_t default_ok;    /* the default mix file was written and read back */
    uint32_t file_len;
    uint32_t checked;
} g_mixer_selftest;

static void mixer_selftest(uint32_t code)
{
    char path[32];
    char buf[400];
    size_t len = 0;

    g_mixer_selftest.stage = 1;
    karaoke_mix_song_path(path, sizeof(path), code);

    k_mutex_lock(&s_app.lock, K_FOREVER);
    open_mixer();
    karaoke_mixer_adjust(0, -1);                              /* piano 95 % */
    karaoke_mixer_adjust(MIX_ITEM_BASS, +1);
    karaoke_mixer_adjust(MIX_ITEM_BASS, +1);                  /* bass +2 dB */
    karaoke_mixer_click(MIX_ITEM_CHANNEL0 + 3);               /* mute channel 4 */
    close_mixer();
    k_mutex_unlock(&s_app.lock);

    g_mixer_selftest.saved_ok = mix_store_read(path, buf, sizeof(buf) - 1U, &len) && len > 0U;
    g_mixer_selftest.file_len = (uint32_t)len;

    k_mutex_lock(&s_app.lock, K_FOREVER);
    karaoke_mixer_song_started(code);                         /* reload from the card */
    const karaoke_mix_t *m = karaoke_mixer_get();
    g_mixer_selftest.reload_ok = (m->section[0] == 95 && m->bass == 2 && m->mute == 0x0008 &&
                                  yamaha_fm_get_section_gain(0) == 95 && yamaha_fm_get_tone_bass() == 2 &&
                                  yamaha_fm_get_channel_mute(3));

    karaoke_mixer_click(MIX_ITEM_SAVE_ALL);                   /* default mix file */
    g_mixer_selftest.default_ok = mix_store_read(MIXER_DEFAULT_PATH, buf, sizeof(buf) - 1U, &len) && len > 0U;

    karaoke_mixer_click(MIX_ITEM_RESET);                      /* back to default ... */
    karaoke_mixer_adjust(0, +1);
    karaoke_mixer_click(MIX_ITEM_RESET);
    karaoke_mixer_adjust(0, +1);
    karaoke_mixer_adjust(0, -1);                              /* equal to the default again */
    karaoke_mixer_leave();                                    /* removes the song's file */
    k_mutex_unlock(&s_app.lock);

    g_mixer_selftest.deleted_ok = !mix_store_read(path, buf, sizeof(buf) - 1U, &len);

    /* put the card back as it was */
    k_mutex_lock(&s_app.lock, K_FOREVER);
    karaoke_mixer_click(MIX_ITEM_RESET);
    for (int i = 0; i < 20; i++) karaoke_mixer_adjust(MIX_ITEM_BASS, -1);
    for (int i = 0; i < 20; i++) karaoke_mixer_adjust(0, +1);
    karaoke_mixer_adjust(MIX_ITEM_BASS, +1);
    k_mutex_unlock(&s_app.lock);
    g_mixer_selftest.checked = 1;
    g_mixer_selftest.stage = 2;
}
#endif

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

    /* Ticks each direction has been held, for the mixer's delayed first step and hold-to-repeat */
    static uint32_t up_rep = 0;
    static uint32_t dn_rep = 0;
    static uint32_t lt_rep = 0;
    static uint32_t rt_rep = 0;

#ifdef KARAOKE_MIXER_SELFTEST
    uint32_t selftest_at = k_uptime_get_32() + 8000U;
#endif
    while (1) {
#ifdef KARAOKE_MIXER_SELFTEST
        if (g_mixer_selftest.stage == 0U && (int32_t)(k_uptime_get_32() - selftest_at) >= 0) {
            song_entry_t cur;
            if (karaoke_catalog_get_song(s_app.current_song_index, &cur)) {
                mixer_selftest(cur.song_code);
            }
        }
#endif
        bool up = ((GPIOC->IDR & (1U << 5)) == 0);
        bool dn = ((GPIOC->IDR & (1U << 1)) == 0);
        bool lt = ((GPIOC->IDR & (1U << 0)) == 0);
        bool rt = ((GPIOC->IDR & (1U << 4)) == 0);
        bool press = ((GPIOA->IDR & (1U << 0)) != 0);

        if (up || dn || lt || rt || press) {
            s_app.last_input_ms = k_uptime_get_32();
        }

        up_rep = up ? up_rep + 1U : 0U;
        dn_rep = dn ? dn_rep + 1U : 0U;
        lt_rep = lt ? lt_rep + 1U : 0U;
        rt_rep = rt ? rt_rep + 1U : 0U;
        if (!up && !dn && !lt && !rt) {
            s_mixer_latch = false;      /* every direction released: the mixer takes input again */
        }

        wifi_audio_process();

        if (wifi_audio_receiver_is_streaming()) {
            k_mutex_lock(&s_app.lock, K_FOREVER);
            if (s_app.ui_mode == UI_VIEW_PLAYING) {
                open_wifi_audio();
            }
            k_mutex_unlock(&s_app.lock);
        }

        /* 1. Simultaneous UP + DOWN Detection -> Enter NUMBER SELECT Mode */
        if (up && dn) {
            s_mixer_latch = true;
            both_ud_hold_ticks++;
            if (both_ud_hold_ticks == 10) { /* ~200 ms debounce */
                k_mutex_lock(&s_app.lock, K_FOREVER);
                if (s_app.ui_mode == UI_VIEW_SETTINGS) {
                    /* No OK button needed: UP + DOWN saves and returns to the song */
                    karaoke_settings_save();
                    s_app.ui_mode = UI_VIEW_PLAYING;
                    karaoke_ui_set_view(UI_VIEW_PLAYING);
                } else if (s_app.ui_mode == UI_VIEW_SENSME) {
                    karaoke_settings_save();
                    s_app.ui_mode = UI_VIEW_PLAYING;
                    karaoke_ui_set_view(UI_VIEW_PLAYING);
                } else if (s_app.ui_mode == UI_VIEW_MIXER) {
                    close_mixer();
                } else if (s_app.ui_mode == UI_VIEW_WIFI_AUDIO) {
                    s_app.ui_mode = UI_VIEW_PLAYING;
                    karaoke_ui_set_view(UI_VIEW_PLAYING);
                    printk("[Nav] Exited WIFI AUDIO mode!\n");
                } else if (s_app.ui_mode == UI_VIEW_NUMBER_SELECT) {
                    /* Cancel: back to the song that is playing */
                    s_app.ui_mode = UI_VIEW_PLAYING;
                    karaoke_ui_set_view(UI_VIEW_PLAYING);
                    printk("[Nav] Number select cancelled\n");
                } else {
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
            s_mixer_latch = true;
            both_lr_hold_ticks++;
            if (both_lr_hold_ticks == 10) { /* ~200 ms debounce */
                k_mutex_lock(&s_app.lock, K_FOREVER);
                if (s_app.ui_mode == UI_VIEW_MIXER) {
                    /* Mute > Solo > Normal on a channel, reset on a value row, or the action rows */
                    if (mixer_item() == MIX_ITEM_DONE) {
                        close_mixer();
                    } else {
                        karaoke_mixer_click(mixer_item());
                    }
                } else if (s_app.ui_mode != UI_VIEW_SETTINGS) {
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
                    } else if (s_app.ui_mode == UI_VIEW_MIXER) {
                        close_mixer();
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
                        if (s_app.settings_cursor == 5) {
                            open_mixer();
                        } else {
                            karaoke_settings_save();
                            s_app.ui_mode = UI_VIEW_PLAYING;
                            karaoke_ui_set_view(UI_VIEW_PLAYING);
                        }
                    } else if (s_app.ui_mode == UI_VIEW_MIXER) {
                        if (mixer_item() == MIX_ITEM_DONE) {
                            close_mixer();
                        } else {
                            karaoke_mixer_click(mixer_item());
                        }
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
                    s_app.settings_cursor = (s_app.settings_cursor + 8) % 9;
                }
                if (dn && !last_dn) {
                    s_app.settings_cursor = (s_app.settings_cursor + 1) % 9;
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
                    } else if (s_app.settings_cursor == 4) {
                        karaoke_settings_set_sound_bank((uint8_t)((cfg->sound_bank + 2U) % 3U));
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
                    } else if (s_app.settings_cursor == 4) {
                        karaoke_settings_set_sound_bank((uint8_t)((cfg->sound_bank + 1U) % 3U));
                    } else if (s_app.settings_cursor == 5) {
                        open_sensme();
                    } else if (s_app.settings_cursor == 6) {
                        open_mixer();
                    } else if (s_app.settings_cursor == 7) {
                        open_wifi_audio();
                    } else if (s_app.settings_cursor == 8) {
                        karaoke_settings_save();
                        s_app.ui_mode = UI_VIEW_PLAYING;
                        karaoke_ui_set_view(UI_VIEW_PLAYING);
                    }
                }
            } else if (mode == UI_VIEW_WIFI_AUDIO) {
                /* Wi-Fi Audio Streaming mode */
                const karaoke_settings_t *cfg = karaoke_settings_get();
                if (lt && !last_lt) {
                    uint8_t v = cfg->master_volume;
                    karaoke_settings_set_volume(v >= 5 ? v - 5 : 0);
                    wifi_audio_send_command(BTN_CMD_VOL_DOWN);
                }
                if (rt && !last_rt) {
                    uint8_t v = cfg->master_volume;
                    karaoke_settings_set_volume(v <= 95 ? v + 5 : 100);
                    wifi_audio_send_command(BTN_CMD_VOL_UP);
                }
                if (dn && !last_dn) {
                    wifi_audio_send_command(BTN_CMD_NEXT);
                }
                if (up && !last_up) {
                    wifi_audio_send_command(BTN_CMD_PREV);
                }
                if (press && !last_press) {
                    wifi_audio_send_command(BTN_CMD_PLAY_PAUSE);
                }
            } else if (mode == UI_VIEW_SENSME) {
                /* SensMe: UP/DOWN navigate 12 mood channels, RIGHT applies filter, saves, and plays a matching song */
                if (up && !last_up) {
                    s_app.sensme_cursor = (s_app.sensme_cursor + SENSME_CH_COUNT - 1U) % SENSME_CH_COUNT;
                }
                if (dn && !last_dn) {
                    s_app.sensme_cursor = (s_app.sensme_cursor + 1U) % SENSME_CH_COUNT;
                }
                if (rt && !last_rt) {
                    karaoke_settings_set_sensme_channel(s_app.sensme_cursor);
                    karaoke_settings_save();
                    printk("[SensMe] Selected & saved mood filter: %s (Channel %u)\n",
                           sensme_channel_name(s_app.sensme_cursor), (unsigned)s_app.sensme_cursor);
                    if (s_app.sensme_cursor != SENSME_CH_ALL &&
                        !sensme_matches_active_channel(s_app.current_song_index)) {
                        play_random_song();
                    }
                }
            } else if (mode == UI_VIEW_MIXER) {
                /* Mixer: UP/DOWN pick a row, LEFT/RIGHT adjust it. The first step comes after 60 ms so that
                 * a pair pressed almost together (the mixer's click) is not taken for two single presses;
                 * holding repeats after 400 ms. Nothing counts until every button was released after a
                 * pair press or after entering the mixer. */
#define MIXER_FIRST_TICKS   3U
#define MIXER_REPEAT_TICKS  20U
                bool free_ud = !s_mixer_latch && !(up && dn);
                bool free_lr = !s_mixer_latch && !(lt && rt);
                bool up_go = free_ud && up && (up_rep == MIXER_FIRST_TICKS || (up_rep > MIXER_REPEAT_TICKS && (up_rep % 3U) == 0U));
                bool dn_go = free_ud && dn && (dn_rep == MIXER_FIRST_TICKS || (dn_rep > MIXER_REPEAT_TICKS && (dn_rep % 3U) == 0U));
                bool lt_go = free_lr && lt && (lt_rep == MIXER_FIRST_TICKS || (lt_rep > MIXER_REPEAT_TICKS && (lt_rep % 3U) == 0U));
                bool rt_go = free_lr && rt && (rt_rep == MIXER_FIRST_TICKS || (rt_rep > MIXER_REPEAT_TICKS && (rt_rep % 3U) == 0U));

                if (up_go) {
                    s_app.mixer_cursor = (uint8_t)((s_app.mixer_cursor + karaoke_mixer_view()->count - 1U) % karaoke_mixer_view()->count);
                }
                if (dn_go) {
                    s_app.mixer_cursor = (uint8_t)((s_app.mixer_cursor + 1U) % karaoke_mixer_view()->count);
                }
                if (lt_go) {
                    karaoke_mixer_adjust(mixer_item(), -1);
                }
                if (rt_go) {
                    if (mixer_item() == MIX_ITEM_DONE) {
                        close_mixer();
                    } else {
                        karaoke_mixer_adjust(mixer_item(), +1);
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

                /* Next / Prev Song: UP / DOWN (obeys active SensMe mood filter if enabled) */
                if (up && !last_up) {
                    uint32_t total = karaoke_catalog_get_total_songs();
                    if (sensme_get_active_channel() != SENSME_CH_ALL) {
                        s_app.current_song_index = find_adjacent_mood_song(s_app.current_song_index, -1);
                        play_song(s_app.current_song_index);
                    } else if (s_app.current_song_index == KARAOKE_DOOM_SONG_INDEX) {
                        s_app.current_song_index = (total > 0) ? total - 1 : 0;
                        play_song(s_app.current_song_index);
                    } else if (s_app.current_song_index > 0) {
                        s_app.current_song_index--;
                        play_song(s_app.current_song_index);
                    }
                }
                if (dn && !last_dn) {
                    uint32_t total = karaoke_catalog_get_total_songs();
                    if (sensme_get_active_channel() != SENSME_CH_ALL) {
                        s_app.current_song_index = find_adjacent_mood_song(s_app.current_song_index, 1);
                        play_song(s_app.current_song_index);
                    } else if (s_app.current_song_index == KARAOKE_DOOM_SONG_INDEX) {
                        s_app.current_song_index = 0;
                        play_song(s_app.current_song_index);
                    } else if (s_app.current_song_index + 1 < total) {
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

#if KARAOKE_STRESS_SWITCH_MS > 0
        if ((uint32_t)(now - s_stress.last_ms) >= KARAOKE_STRESS_SWITCH_MS) {
            k_mutex_lock(&s_app.lock, K_FOREVER);
            s_app.last_input_ms = now;
            s_stress.switches++;
            if (!play_random_song()) {
                s_stress.failures++;
            }
            k_mutex_unlock(&s_app.lock);
            s_stress.last_ms = k_uptime_get_32();
        }
#endif

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
    led_matrix_init();

    /* 1. Initialize ST7789 IPS LCD (FSMC parallel bus) */
    karaoke_ui_init();

    /* 2. Initialize Yamaha FM Synthesizer & MIDI Sequencer */
    yamaha_fm_synth_init(AUDIO_SAMPLE_RATE);
#ifdef SYNTH_COST_BENCH
    {
        extern void yamaha_fm_cost_bench(void);
        yamaha_fm_cost_bench();
    }
#endif
    midi_karaoke_init(yamaha_fm_get_callbacks());

    /* 3. Initialize ES8388 Audio Codec & Hook Master Audio Callback */
    audio_set_pcm_callback(audio_master_render_callback);
    audio_hardware_init();
    audio_set_volume(80);

    /* 4. Initialize SD Card, Song Catalog & Wi-Fi Audio Receiver */
    sd_card_reader_init();
    karaoke_catalog_init();
    wifi_audio_receiver_init();

    /* 5. Initialize Persistent Audio Settings (survives reset & shutdown) */
    karaoke_settings_init();
    karaoke_mixer_init();
    sensme_init();

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
