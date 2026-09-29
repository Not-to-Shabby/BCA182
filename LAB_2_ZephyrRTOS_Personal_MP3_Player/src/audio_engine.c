/**
 * @file audio_engine.c
 * @brief Real-time audio engine and musical note synthesizer for the Personal MP3 Player.
 *        Implements hardware TIM3 PWM note synthesis on PB0 (onboard buzzer) and PB1 (expansion pin)
 *        with immediate shadow prescaler reload, glitch-free ARR modulation, and Zephyr k_timer
 *        ticker for drift-free note progression across 8 classical repertoire songs.
 *
 * Course: BCA182 Embedded Systems Programming
 * Laboratory Activity 2: Personal MP3 Player
 */

#include "audio_engine.h"
#include "audio_codec_es8388.h"
#include "app_config.h"
#include "threads.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <stm32f4xx.h>

/* -------------------------------------------------------------------------- */
/* Musical Note Period Definitions (T in milliseconds, f = 1000 / T Hz)       */
/* -------------------------------------------------------------------------- */
#define So__1      5.102f  /* G3: ~196 Hz  */
#define Si__1      4.059f  /* B3: ~246 Hz  */

#define Do         3.822f  /* C4: 261.6 Hz */
#define DoD        3.608f  /* C#4: 277.2 Hz*/
#define Re         3.405f  /* D4: 293.7 Hz */
#define ReD        3.214f  /* D#4: 311.1 Hz*/
#define Mi         3.033f  /* E4: 329.7 Hz */
#define Fa         2.863f  /* F4: 349.2 Hz */
#define FaD        2.702f  /* F#4: 370.0 Hz*/
#define So         2.551f  /* G4: 392.0 Hz */
#define SoD        2.407f  /* G#4: 415.3 Hz*/
#define La         2.272f  /* A4: 440.0 Hz */
#define LaD        2.145f  /* A#4: 466.2 Hz*/
#define Si         2.024f  /* B4: 494.0 Hz */

#define Do_2       1.911f  /* C5: 523.3 Hz */
#define DoD_2      1.803f  /* C#5: 554.4 Hz*/
#define Re_2       1.702f  /* D5: 587.3 Hz */
#define ReD_2      1.607f  /* D#5: 622.3 Hz*/
#define Mi_2       1.516f  /* E5: 659.3 Hz */
#define Fa_2       1.431f  /* F5: 698.8 Hz */
#define FaD_2      1.351f  /* F#5: 740.0 Hz*/
#define So_2       1.275f  /* G5: 784.0 Hz */
#define SoD_2      1.204f  /* G#5: 830.6 Hz*/
#define La_2       1.136f  /* A5: 880.0 Hz */
#define LaD_2      1.073f  /* A#5: 932.3 Hz*/
#define Si_2       1.012f  /* B5: 987.8 Hz */

#define Do_3       0.955f  /* C6: 1047 Hz  */
#define DoD_3      0.901f  /* C#6: 1109 Hz */
#define Re_3       0.853f  /* D6: 1172 Hz  */
#define No         0.0f    /* Musical Rest (Silence) */

/* Beat duration multipliers */
#define b0         1.000f  /* Whole note    */
#define b1         0.500f  /* Half note     */
#define b2         0.250f  /* Quarter note  */
#define b3         0.125f  /* Eighth note   */
#define b4         0.075f  /* Sixteenth note*/

/* -------------------------------------------------------------------------- */
/* Song Note and Beat Data Arrays (8 Classical Compositions)                  */
/* -------------------------------------------------------------------------- */
#include "song_data.inc"

/* Song Catalog Table */
static const musical_piece_t s_catalog[8] = {
    {0, "Fur Elise -",      "Beethoven",      s_fur_elise_note,               s_fur_elise_beat,               0.18f, 72},
    {1, "Canon In D -",     "Pachelbel",      s_canon_in_d_note,              s_canon_in_d_beat,              0.41f, 88},
    {2, "Minuet in G",      "major - Bach",   s_minuet_in_g_major_note,       s_minuet_in_g_major_beat,       0.13f, 90},
    {3, "Turkish March -",  "Mozart",         s_turkish_march_note,           s_turkish_march_beat,           0.15f, 176},
    {4, "Nocturne in E",    "flat - Chopin",  s_nocturne_in_e_flat_note,      s_nocturne_in_e_flat_beat,      0.31f, 116},
    {5, "Waltz No. 2 -",    "Shostakovich",   s_waltz_no2_note,               s_waltz_no2_beat,               0.10f, 135},
    {6, "Nocturne in C",    "sharp - Chopin", s_nocturne_in_c_sharp_minor_note, s_nocturne_in_c_sharp_minor_beat, 0.13f, 64},
    {7, "Symphony No. 40",  "- Mozart",       s_symphony_no40_note,           s_symphony_no40_beat,           0.10f, 168}
};

/* -------------------------------------------------------------------------- */
/* Hardware Low-Level Timer TIM3 Audio Generator (PB0: CH3, PB1: CH4)         */
/* -------------------------------------------------------------------------- */
static void hw_pwm_init(void)
{
    /* 1. Enable AHB1 clock for GPIOB and APB1 clock for TIM3 */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;

    /* 2. Configure PB0 (TIM3_CH3 - Onboard Buzzer) as Alternate Function 2 */
    GPIOB->MODER = (GPIOB->MODER & ~(3U << (0 * 2))) | (2U << (0 * 2));
    GPIOB->OTYPER &= ~(1U << 0);
    GPIOB->OSPEEDR |= (3U << (0 * 2));
    GPIOB->PUPDR &= ~(3U << (0 * 2));
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~(0xFU << (0 * 4))) | (2U << (0 * 4)); /* AF2 */

    /* Configure PB1 (TIM3_CH4 - Expansion Header Pin) as Alternate Function 2 */
    GPIOB->MODER = (GPIOB->MODER & ~(3U << (1 * 2))) | (2U << (1 * 2));
    GPIOB->OTYPER &= ~(1U << 1);
    GPIOB->OSPEEDR |= (3U << (1 * 2));
    GPIOB->PUPDR &= ~(3U << (1 * 2));
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~(0xFU << (1 * 4))) | (2U << (1 * 4)); /* AF2 */

    /* 3. Configure TIM3 Timebase:
     *    APB1 timer clock is 84 MHz (168 MHz SYSCLK / 4 APB1 * 2 timer mult).
     *    Prescaler = 83 -> counter clock = 84 MHz / (83 + 1) = 1.0 MHz (1 us per count).
     */
    TIM3->PSC = 83;
    TIM3->ARR = 1000;
    TIM3->CCR3 = 0; /* Silent initially */
    TIM3->CCR4 = 0; /* Silent initially */

    /* PWM Mode 1 on Channel 3 and Channel 4 */
    TIM3->CCMR2 = (TIM3->CCMR2 & ~((7U << 4) | (7U << 12))) |
                  (6U << 4) | TIM_CCMR2_OC3PE |
                  (6U << 12) | TIM_CCMR2_OC4PE;

    /* Enable output on Channel 3 and Channel 4 */
    TIM3->CCER |= (TIM_CCER_CC3E | TIM_CCER_CC4E);

    /* CRITICAL: Force prescaler and ARR to load into shadow registers immediately */
    TIM3->EGR = TIM_EGR_UG;
    TIM3->SR &= ~TIM_SR_UIF;

    /* Enable counter WITHOUT auto-reload preload to allow instantaneous frequency modulation */
    TIM3->CR1 = TIM_CR1_CEN;
}

static void hw_set_tone(float note_period_ms, uint8_t volume_percent)
{
    if (note_period_ms <= 0.001f || volume_percent == 0) {
        TIM3->CCR3 = 0;
        TIM3->CCR4 = 0;
        return;
    }

    /* Convert note period from ms to microseconds (1 us per counter tick at 1 MHz) */
    uint32_t period_us = (uint32_t)(note_period_ms * 1000.0f + 0.5f);
    if (period_us < 50) {
        period_us = 50; /* 20 kHz max limit */
    }
    if (period_us > 20000) {
        period_us = 20000; /* 50 Hz min limit */
    }

    TIM3->ARR = period_us - 1;

    /* Volume attenuation:
     * 50% duty cycle (ARR / 2) produces the maximum fundamental audio acoustic volume.
     * At 100% volume -> duty = ARR / 2.
     * At volume_percent -> duty = (ARR * volume_percent) / 200.
     */
    uint32_t pulse = ((period_us - 1) * (uint32_t)volume_percent) / 200U;
    if (pulse == 0 && volume_percent > 0) {
        pulse = 1;
    }
    TIM3->CCR3 = pulse;
    TIM3->CCR4 = pulse;

    /* Reset counter if it exceeded the new period to prevent 65535 rollover glitch */
    if (TIM3->CNT >= (period_us - 1)) {
        TIM3->CNT = 0;
    }
}

static void hw_stop_tone(void)
{
    TIM3->CCR3 = 0;
    TIM3->CCR4 = 0;
}

/* -------------------------------------------------------------------------- */
/* Real-Time Music Player Engine State & Ticker Callback                      */
/* -------------------------------------------------------------------------- */
static struct k_timer s_audio_timer;
static uint8_t  s_active_song = 0;
static uint16_t s_note_idx = 0;
static bool     s_in_gap_phase = false;
static bool     s_is_playing = false;

static void audio_timer_handler(struct k_timer *timer_id);

void audio_engine_init(void)
{
    printk("[Audio] Initializing TIM3 PWM audio on PB0 (onboard) & PB1 (header)...\n");
    hw_pwm_init();

    printk("[Audio] Initializing 12-bit Analog DAC (PA4) and ES8388 (3.5mm Jack)...\n");
    audio_hardware_dac_init();

    /* Initialize Zephyr k_timer ticker for note scheduling */
    k_timer_init(&s_audio_timer, audio_timer_handler, NULL);
    printk("[Audio] Audio engine calibrated with exact pitch and tempo timebase.\n");
}

static void audio_timer_handler(struct k_timer *timer_id)
{
    ARG_UNUSED(timer_id);

    if (!s_is_playing) {
        hw_stop_tone();
        audio_hardware_dac_stop();
        return;
    }

    const musical_piece_t *piece = &s_catalog[s_active_song];
    if (s_note_idx >= piece->length) {
        /* Loop track from beginning */
        s_note_idx = 0;
    }

    float note = piece->notes[s_note_idx];
    float beat = piece->beats[s_note_idx];

    /* Calculate total note duration:
     * beat * tempo * 14000.0 ms yields authentic classical performance tempo:
     * - Fur Elise: ~315 ms per eighth note (Poco moto ~190 BPM eighths / 63 BPM dotted quarters)
     * - Turkish March: ~262 ms per note (Alla Turca ~114 BPM)
     * - Minuet in G: ~455 ms per note (Moderato ~132 BPM)
     * - Symphony No. 5: ~122 ms per eighth note (Allegro con brio)
     */
    uint32_t duration_ms = (uint32_t)(beat * piece->tempo * 14000.0f + 0.5f);
    if (duration_ms < 60) {
        duration_ms = 60;
    }

    if (!s_in_gap_phase) {
        /* Phase 1: Play Tone (80% of beat duration) */
        uint32_t play_ms = (duration_ms * 80U) / 100U;
        if (play_ms < 40) {
            play_ms = 40;
        }

        uint8_t vol;
        k_mutex_lock(&g_player_mutex, K_FOREVER);
        vol = g_player.volume_percent;
        k_mutex_unlock(&g_player_mutex);

        hw_set_tone(note, vol);
        audio_hardware_dac_set_tone(note, vol);
        s_in_gap_phase = true;
        k_timer_start(&s_audio_timer, K_MSEC(play_ms), K_NO_WAIT);
    } else {
        /* Phase 2: Articulation Gap (20% silence for clear note attack) */
        uint32_t gap_ms = duration_ms - ((duration_ms * 80U) / 100U);
        if (gap_ms < 15) {
            gap_ms = 15;
        }

        hw_stop_tone();
        audio_hardware_dac_stop();
        s_in_gap_phase = false;
        s_note_idx++; /* Advance to next note */
        k_timer_start(&s_audio_timer, K_MSEC(gap_ms), K_NO_WAIT);
    }
}

void audio_engine_start_song(uint8_t song_index)
{
    if (song_index >= 8) {
        song_index = 0;
    }
    s_active_song = song_index;
    s_note_idx = 0;
    s_in_gap_phase = false;
    s_is_playing = true;

    /* Start ticker immediately with no initial delay */
    k_timer_start(&s_audio_timer, K_NO_WAIT, K_NO_WAIT);
}

void audio_engine_pause(void)
{
    s_is_playing = false;
    hw_stop_tone();
    audio_hardware_dac_stop();
    k_timer_stop(&s_audio_timer);
}

void audio_engine_resume(void)
{
    if (!s_is_playing) {
        s_is_playing = true;
        s_in_gap_phase = false;
        k_timer_start(&s_audio_timer, K_NO_WAIT, K_NO_WAIT);
    }
}

void audio_engine_stop(void)
{
    s_is_playing = false;
    hw_stop_tone();
    audio_hardware_dac_stop();
    k_timer_stop(&s_audio_timer);
    s_note_idx = 0;
    s_in_gap_phase = false;
}

void audio_engine_set_volume(uint8_t volume_percent)
{
    if (s_is_playing && !s_in_gap_phase) {
        const musical_piece_t *piece = &s_catalog[s_active_song];
        if (s_note_idx < piece->length) {
            hw_set_tone(piece->notes[s_note_idx], volume_percent);
            audio_hardware_dac_set_tone(piece->notes[s_note_idx], volume_percent);
        }
    } else {
        audio_hardware_dac_set_volume(volume_percent);
    }
}

uint16_t audio_engine_get_note_index(void)
{
    return s_note_idx;
}

bool audio_engine_is_playing(void)
{
    return s_is_playing;
}

const musical_piece_t* audio_engine_get_piece(uint8_t song_index)
{
    if (song_index >= 8) {
        song_index = 0;
    }
    return &s_catalog[song_index];
}
