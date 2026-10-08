/**
 * @file test_synth.c
 * @brief Host tests that render the real yamaha_fm_synth.c and check how it sounds:
 *        pitch, decay, release, pedal, bend, expression, drums and overload behaviour.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include <unity.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/yamaha_fm_synth.c"

#define RATE        44100
#define BLOCK       256         /* 128 stereo frames, the block the audio thread asks for */
#define MAX_FRAMES  (RATE * 4)

static int16_t g_pcm[MAX_FRAMES * 2];
static size_t g_frames;

void setUp(void)
{
    yamaha_fm_synth_init(RATE);
    g_frames = 0;
    memset(g_pcm, 0, sizeof(g_pcm));
}

void tearDown(void) {}

static void render_ms(unsigned ms)
{
    size_t want = (size_t)ms * RATE / 1000U;
    size_t end = g_frames + want;

    TEST_ASSERT_TRUE(end <= MAX_FRAMES);
    while (g_frames < end) {
        size_t n = end - g_frames;
        if (n > BLOCK / 2) {
            n = BLOCK / 2;
        }
        yamaha_fm_synth_render(g_pcm + g_frames * 2, n * 2);
        g_frames += n;
    }
}

static double rms_window(unsigned from_ms, unsigned len_ms)
{
    size_t a = (size_t)from_ms * RATE / 1000U;
    size_t n = (size_t)len_ms * RATE / 1000U;
    double sum = 0.0;

    TEST_ASSERT_TRUE(a + n <= g_frames);
    for (size_t i = 0; i < n; i++) {
        double v = g_pcm[(a + i) * 2];
        sum += v * v;
    }
    return sqrt(sum / (double)n);
}

/* Energy of the sample-to-sample change relative to the level: larger means brighter. */
static double brightness(unsigned from_ms, unsigned len_ms)
{
    size_t a = (size_t)from_ms * RATE / 1000U;
    size_t n = (size_t)len_ms * RATE / 1000U;
    double e = 0.0, d = 0.0;

    for (size_t i = 1; i < n; i++) {
        double x = g_pcm[(a + i) * 2];
        double y = g_pcm[(a + i - 1) * 2];
        e += x * x;
        d += (x - y) * (x - y);
    }
    return e > 0.0 ? sqrt(d / e) : 0.0;
}

/* Fundamental frequency from the autocorrelation of the left channel. */
static double pitch_hz(unsigned from_ms)
{
    size_t a = (size_t)from_ms * RATE / 1000U;
    const size_t n = 6000, min_lag = 30, max_lag = 1500;
    static double corr[1501];
    double best = 0.0;

    TEST_ASSERT_TRUE(a + n + max_lag <= g_frames);
    for (size_t lag = min_lag; lag <= max_lag; lag++) {
        double s = 0.0;
        for (size_t i = 0; i < n; i++) {
            s += (double)g_pcm[(a + i) * 2] * (double)g_pcm[(a + i + lag) * 2];
        }
        corr[lag] = s;
        if (s > best) {
            best = s;
        }
    }
    for (size_t lag = min_lag + 1; lag < max_lag; lag++) {
        if (corr[lag] >= 0.9 * best && corr[lag] >= corr[lag - 1] && corr[lag] >= corr[lag + 1]) {
            double d = corr[lag - 1] - 2.0 * corr[lag] + corr[lag + 1];
            double off = d != 0.0 ? 0.5 * (corr[lag - 1] - corr[lag + 1]) / d : 0.0;
            return (double)RATE / ((double)lag + off);
        }
    }
    return 0.0;
}

static void play(uint8_t ch, uint8_t program, uint8_t note, uint8_t vel)
{
    yamaha_fm_program_change(ch, program);
    yamaha_fm_note_on(ch, note, vel);
}

void test_organ_a4_is_in_tune(void)
{
    play(0, 16, 69, 100);
    render_ms(600);
    double hz = pitch_hz(150);
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 440.0f, (float)hz);
}

void test_notes_across_the_keyboard_are_in_tune(void)
{
    static const uint8_t notes[] = {36, 48, 60, 72, 84};

    for (unsigned i = 0; i < sizeof(notes); i++) {
        setUp();
        play(0, 16, notes[i], 100);
        render_ms(600);
        double want = 440.0 * pow(2.0, ((double)notes[i] - 69.0) / 12.0);
        double got = pitch_hz(150);
        TEST_ASSERT_FLOAT_WITHIN((float)(want * 0.01), (float)want, (float)got);
    }
}

void test_piano_dies_away_while_the_key_is_held(void)
{
    play(0, 0, 60, 100);
    render_ms(2500);
    double early = rms_window(100, 100);
    double late = rms_window(2000, 100);
    TEST_ASSERT_TRUE(early > 100.0);
    TEST_ASSERT_TRUE(late < early * 0.5);
}

void test_organ_holds_its_level(void)
{
    play(0, 16, 60, 100);
    render_ms(2500);
    double early = rms_window(200, 100);
    double late = rms_window(2000, 100);
    TEST_ASSERT_TRUE(late > early * 0.85);
    TEST_ASSERT_TRUE(late < early * 1.15);
}

void test_piano_gets_duller_as_it_decays(void)
{
    play(0, 0, 60, 110);
    render_ms(2000);
    TEST_ASSERT_TRUE(brightness(20, 80) > brightness(1400, 80) * 1.25);
}

void test_harder_notes_are_louder_and_brighter(void)
{
    play(0, 0, 60, 120);
    render_ms(400);
    double loud = rms_window(30, 150), loud_b = brightness(30, 150);

    setUp();
    play(0, 0, 60, 30);
    render_ms(400);
    double soft = rms_window(30, 150), soft_b = brightness(30, 150);

    TEST_ASSERT_TRUE(loud > soft * 3.0);
    TEST_ASSERT_TRUE(loud_b > soft_b);
}

void test_released_note_fades_out(void)
{
    play(0, 16, 60, 100);
    render_ms(500);
    double held = rms_window(300, 100);
    yamaha_fm_note_off(0, 60, 0);
    render_ms(900);
    TEST_ASSERT_TRUE(held > 100.0);
    TEST_ASSERT_TRUE(rms_window(1100, 100) < held * 0.02);
    TEST_ASSERT_EQUAL_UINT8(0, yamaha_fm_get_active_voice_count());
}

void test_sustain_pedal_holds_a_released_note_until_it_is_lifted(void)
{
    yamaha_fm_control_change(0, 64, 127);
    play(0, 16, 60, 100);
    render_ms(300);
    yamaha_fm_note_off(0, 60, 0);
    render_ms(700);
    double held = rms_window(800, 100);
    TEST_ASSERT_TRUE(held > 500.0);

    yamaha_fm_control_change(0, 64, 0);
    render_ms(800);
    TEST_ASSERT_TRUE(rms_window(1700, 100) < held * 0.02);
}

void test_pedal_does_not_hold_a_key_that_is_still_down(void)
{
    yamaha_fm_control_change(0, 64, 127);
    play(0, 16, 60, 100);
    render_ms(300);
    yamaha_fm_control_change(0, 64, 0);
    render_ms(500);
    TEST_ASSERT_TRUE(rms_window(600, 100) > 500.0);
}

void test_pitch_bend_up_by_the_default_two_semitones(void)
{
    play(0, 16, 69, 100);
    render_ms(500);
    double base = pitch_hz(100);
    yamaha_fm_pitch_bend(0, 16383);
    render_ms(500);
    double bent = pitch_hz(700);
    TEST_ASSERT_FLOAT_WITHIN(4.0f, 440.0f, (float)base);
    TEST_ASSERT_FLOAT_WITHIN(0.02f * 494.0f, 493.9f, (float)bent);
}

void test_pitch_bend_range_follows_rpn_zero(void)
{
    yamaha_fm_control_change(0, 101, 0);
    yamaha_fm_control_change(0, 100, 0);
    yamaha_fm_control_change(0, 6, 12);
    play(0, 16, 69, 100);
    render_ms(300);
    yamaha_fm_pitch_bend(0, 16383);
    render_ms(500);
    TEST_ASSERT_FLOAT_WITHIN(0.02f * 880.0f, 880.0f, (float)pitch_hz(500));
}

void test_expression_zero_silences_a_channel(void)
{
    play(0, 16, 60, 100);
    render_ms(300);
    yamaha_fm_control_change(0, 11, 0);
    render_ms(300);
    TEST_ASSERT_TRUE(rms_window(450, 100) < 5.0);
}

void test_channel_volume_scales_the_level(void)
{
    yamaha_fm_control_change(0, 7, 127);
    play(0, 16, 60, 100);
    render_ms(500);
    double full = rms_window(300, 100);

    setUp();
    yamaha_fm_control_change(0, 7, 64);
    play(0, 16, 60, 100);
    render_ms(500);
    double half = rms_window(300, 100);

    TEST_ASSERT_TRUE(half < full * 0.45);
    TEST_ASSERT_TRUE(half > full * 0.15);
}

void test_pan_places_a_note_left_or_right(void)
{
    yamaha_fm_control_change(0, 10, 0);
    play(0, 16, 60, 100);
    render_ms(400);
    double left = rms_window(200, 100);
    double right = 0.0;
    for (size_t i = 0; i < 4410; i++) {
        double v = g_pcm[(8820 + i) * 2 + 1];
        right += v * v;
    }
    right = sqrt(right / 4410.0);
    TEST_ASSERT_TRUE(left > 500.0);
    TEST_ASSERT_TRUE(right < left * 0.02);
}

void test_kick_is_a_short_low_thump_that_ends_by_itself(void)
{
    yamaha_fm_note_on(9, 36, 110);
    render_ms(1500);
    TEST_ASSERT_TRUE(rms_window(0, 120) > 800.0);
    TEST_ASSERT_TRUE(rms_window(1200, 100) < 1.0);
    TEST_ASSERT_EQUAL_UINT8(0, yamaha_fm_get_active_voice_count());
}

void test_closed_hat_cuts_off_an_open_hat(void)
{
    yamaha_fm_note_on(9, 46, 100);
    render_ms(100);
    TEST_ASSERT_TRUE(rms_window(20, 60) > 300.0);
    yamaha_fm_note_on(9, 42, 100);
    render_ms(500);
    TEST_ASSERT_TRUE(rms_window(450, 100) < 20.0);
}

void test_drum_hits_are_noisy_and_tones_are_not(void)
{
    yamaha_fm_note_on(9, 42, 100);
    render_ms(200);
    double hat = brightness(5, 60);

    setUp();
    play(0, 16, 60, 100);
    render_ms(300);
    double organ = brightness(100, 60);

    TEST_ASSERT_TRUE(hat > organ * 3.0);
}

void test_polyphony_never_clips_or_crashes(void)
{
    for (int i = 0; i < 60; i++) {
        play((uint8_t)(i % 8), (uint8_t)(i * 2), (uint8_t)(36 + i), 127);
    }
    render_ms(1500);
    int peak = 0;
    for (size_t i = 0; i < g_frames * 2; i++) {
        int a = abs(g_pcm[i]);
        if (a > peak) {
            peak = a;
        }
    }
    TEST_ASSERT_TRUE(peak > 3000);
    TEST_ASSERT_TRUE(peak <= 32767);
    TEST_ASSERT_TRUE(yamaha_fm_get_active_voice_count() <= FM_MAX_VOICES);
    TEST_ASSERT_TRUE(yamaha_fm_get_steal_count() >= 20);
}

void test_stealing_prefers_dying_notes_over_held_ones(void)
{
    for (int i = 0; i < FM_MAX_VOICES; i++) {
        play(0, 16, (uint8_t)(40 + i), 100);
    }
    render_ms(100);
    yamaha_fm_note_off(0, 40, 0);
    render_ms(60);
    play(1, 16, 90, 100);
    render_ms(60);
    TEST_ASSERT_EQUAL_UINT32(1, yamaha_fm_get_steal_count());
    render_ms(300);
    for (int i = 1; i < FM_MAX_VOICES; i++) {
        bool alive = false;
        for (int v = 0; v < FM_MAX_VOICES; v++) {
            alive |= s_voices[v].active && s_voices[v].channel == 0 && s_voices[v].note == 40 + i;
        }
        TEST_ASSERT_TRUE_MESSAGE(alive, "a held note was stolen while a released one was available");
    }
}

void test_all_notes_off_goes_silent_and_frees_the_voices(void)
{
    for (int i = 0; i < 20; i++) {
        play(0, 16, (uint8_t)(40 + i), 100);
    }
    render_ms(300);
    TEST_ASSERT_TRUE(rms_window(250, 40) > 500.0);
    yamaha_fm_all_notes_off();
    render_ms(300);
    TEST_ASSERT_TRUE(rms_window(550, 40) < 5.0);
    TEST_ASSERT_EQUAL_UINT8(0, yamaha_fm_get_active_voice_count());
}

void test_controller_reset_clears_pedal_expression_and_bend(void)
{
    yamaha_fm_control_change(0, 64, 127);
    yamaha_fm_control_change(0, 11, 20);
    yamaha_fm_pitch_bend(0, 16383);
    yamaha_fm_control_change(0, 121, 0);
    TEST_ASSERT_FALSE(s_channels[0].pedal);
    TEST_ASSERT_EQUAL_UINT8(127, s_channels[0].expression);
    TEST_ASSERT_EQUAL_INT16(0, s_channels[0].bend);
}

void test_channel_notes_off_only_touches_that_channel(void)
{
    play(0, 16, 60, 100);
    play(1, 16, 64, 100);
    render_ms(200);
    yamaha_fm_control_change(0, 123, 0);
    render_ms(300);
    bool ch0 = false, ch1 = false;
    for (int v = 0; v < FM_MAX_VOICES; v++) {
        ch0 |= s_voices[v].active && s_voices[v].channel == 0;
        ch1 |= s_voices[v].active && s_voices[v].channel == 1;
    }
    TEST_ASSERT_FALSE(ch0);
    TEST_ASSERT_TRUE(ch1);
}

void test_overflowing_the_event_ring_drops_events_without_harm(void)
{
    for (int i = 0; i < 400; i++) {
        yamaha_fm_note_on(0, (uint8_t)(40 + (i % 40)), 100);
    }
    TEST_ASSERT_TRUE(yamaha_fm_get_dropped_events() > 0);
    render_ms(300);
    TEST_ASSERT_TRUE(yamaha_fm_get_active_voice_count() <= FM_MAX_VOICES);
}

void test_every_general_midi_program_makes_a_clean_sound(void)
{
    for (int prog = 0; prog < 128; prog++) {
        setUp();
        play(0, (uint8_t)prog, 60, 100);
        render_ms(400);
        double level = rms_window(60, 100);
        int peak = 0;
        for (size_t i = 0; i < g_frames * 2; i++) {
            int a = abs(g_pcm[i]);
            if (a > peak) {
                peak = a;
            }
        }
        char msg[48];
        snprintf(msg, sizeof(msg), "program %d is silent", prog);
        TEST_ASSERT_TRUE_MESSAGE(level > 30.0, msg);
        snprintf(msg, sizeof(msg), "program %d clips", prog);
        TEST_ASSERT_TRUE_MESSAGE(peak < 32000, msg);
    }
}

void test_extreme_notes_do_not_blow_up(void)
{
    static const uint8_t notes[] = {0, 1, 12, 108, 120, 127};

    for (unsigned i = 0; i < sizeof(notes); i++) {
        setUp();
        for (int prog = 0; prog < 128; prog += 9) {
            play(0, (uint8_t)prog, notes[i], 127);
        }
        render_ms(300);
        for (size_t s = 0; s < g_frames * 2; s++) {
            TEST_ASSERT_TRUE(abs(g_pcm[s]) <= 32767);
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_organ_a4_is_in_tune);
    RUN_TEST(test_notes_across_the_keyboard_are_in_tune);
    RUN_TEST(test_piano_dies_away_while_the_key_is_held);
    RUN_TEST(test_organ_holds_its_level);
    RUN_TEST(test_piano_gets_duller_as_it_decays);
    RUN_TEST(test_harder_notes_are_louder_and_brighter);
    RUN_TEST(test_released_note_fades_out);
    RUN_TEST(test_sustain_pedal_holds_a_released_note_until_it_is_lifted);
    RUN_TEST(test_pedal_does_not_hold_a_key_that_is_still_down);
    RUN_TEST(test_pitch_bend_up_by_the_default_two_semitones);
    RUN_TEST(test_pitch_bend_range_follows_rpn_zero);
    RUN_TEST(test_expression_zero_silences_a_channel);
    RUN_TEST(test_channel_volume_scales_the_level);
    RUN_TEST(test_pan_places_a_note_left_or_right);
    RUN_TEST(test_kick_is_a_short_low_thump_that_ends_by_itself);
    RUN_TEST(test_closed_hat_cuts_off_an_open_hat);
    RUN_TEST(test_drum_hits_are_noisy_and_tones_are_not);
    RUN_TEST(test_polyphony_never_clips_or_crashes);
    RUN_TEST(test_stealing_prefers_dying_notes_over_held_ones);
    RUN_TEST(test_all_notes_off_goes_silent_and_frees_the_voices);
    RUN_TEST(test_controller_reset_clears_pedal_expression_and_bend);
    RUN_TEST(test_channel_notes_off_only_touches_that_channel);
    RUN_TEST(test_overflowing_the_event_ring_drops_events_without_harm);
    RUN_TEST(test_every_general_midi_program_makes_a_clean_sound);
    RUN_TEST(test_extreme_notes_do_not_blow_up);
    return UNITY_END();
}
