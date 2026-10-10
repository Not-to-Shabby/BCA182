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
#include "../../src/synth_dsp.c"
#include "../../src/yamaha_fm_synth.c"

#define RATE        44100
#define BLOCK       256         /* 128 stereo frames, the block the audio thread asks for */
#define MAX_FRAMES  (RATE * 4)

static int16_t g_pcm[MAX_FRAMES * 2];
static size_t g_frames;

void setUp(void)
{
    yamaha_fm_synth_init(RATE);
    yamaha_fm_set_effects_level(0);     /* these tests measure the dry voices; effects have their own */
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
    for (int i = 0; i < 90; i++) {
        play((uint8_t)(i % 8), (uint8_t)(i * 2), (uint8_t)(20 + i), 127);
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
    for (int i = 0; i < 1200; i++) {
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

void test_melody_gain_scales_only_the_chosen_channel(void)
{
    play(0, 73, 72, 100);
    play(1, 73, 72, 100);
    yamaha_fm_set_melody_channel(0);
    yamaha_fm_set_melody_gain(200);
    render_ms(400);
    double lead = rms_window(250, 100);

    yamaha_fm_synth_init(RATE);
    g_frames = 0;
    play(0, 73, 72, 100);
    yamaha_fm_set_melody_channel(0);
    yamaha_fm_set_melody_gain(100);
    render_ms(400);
    double plain = rms_window(250, 100);

    TEST_ASSERT_TRUE(lead > plain * 1.5);
}

void test_melody_gain_does_nothing_without_a_melody_channel(void)
{
    play(0, 73, 72, 100);
    yamaha_fm_set_melody_channel(-1);
    yamaha_fm_set_melody_gain(250);
    render_ms(400);
    double boosted = rms_window(250, 100);

    yamaha_fm_synth_init(RATE);
    g_frames = 0;
    play(0, 73, 72, 100);
    yamaha_fm_set_melody_gain(100);
    render_ms(400);
    double plain = rms_window(250, 100);

    TEST_ASSERT_FLOAT_WITHIN((float)plain * 0.02f, (float)plain, (float)boosted);
}

void test_melody_gain_is_clamped_and_drum_channel_is_refused(void)
{
    yamaha_fm_set_melody_gain(1);
    TEST_ASSERT_EQUAL_UINT8(20, yamaha_fm_get_melody_gain());
    yamaha_fm_set_melody_gain(255);
    TEST_ASSERT_EQUAL_UINT8(250, yamaha_fm_get_melody_gain());
    yamaha_fm_set_melody_channel(FM_DRUM_CHANNEL);
    TEST_ASSERT_EQUAL_INT8(-1, s_melody_channel);
    yamaha_fm_set_melody_channel(16);
    TEST_ASSERT_EQUAL_INT8(-1, s_melody_channel);
    yamaha_fm_set_melody_channel(3);
    TEST_ASSERT_EQUAL_INT8(3, s_melody_channel);
}

/* ---- timestamped events, sends, flush ---- */

void test_a_stamped_event_sounds_at_its_frame(void)
{
    /* The anchor maps song time 0 to "now + lead"; an event at song time 100 ms must then start
     * 100 ms after that, to within one render chunk. */
    yamaha_fm_song_time_anchor(0);
    yamaha_fm_song_time_event(100000);
    yamaha_fm_note_on(9, 37, 110);
    render_ms(400);

    size_t first = 0;
    for (size_t i = 0; i < g_frames; i++) {
        if (abs(g_pcm[i * 2]) > 300) {
            first = i;
            break;
        }
    }
    size_t want = (size_t)RATE * MIDI_EVENT_LEAD_MS / 1000U + (size_t)RATE / 10U;
    TEST_ASSERT_TRUE(first + 3U * SUB_FRAMES >= want);
    TEST_ASSERT_TRUE(first <= want + 3U * SUB_FRAMES);
}

void test_events_keep_their_order_when_stamps_differ(void)
{
    yamaha_fm_program_change(0, 16);                     /* organ: sounds until released */
    yamaha_fm_song_time_anchor(0);
    yamaha_fm_song_time_event(50000);
    yamaha_fm_note_on(0, 60, 100);
    yamaha_fm_song_time_event(250000);
    yamaha_fm_note_off(0, 60, 0);
    render_ms(150);
    TEST_ASSERT_TRUE(rms_window(110, 30) > 300.0);       /* sounding between the two stamps */
    render_ms(500);
    TEST_ASSERT_TRUE(rms_window(600, 40) < 30.0);        /* released after the second one */
}

void test_a_song_change_drops_events_still_waiting_in_the_ring(void)
{
    yamaha_fm_song_time_anchor(0);
    yamaha_fm_song_time_event(500000);
    yamaha_fm_note_on(0, 60, 100);
    yamaha_fm_synth_reset();
    render_ms(900);
    TEST_ASSERT_TRUE(rms_window(600, 100) < 5.0);
}

void test_events_pushed_after_a_reset_survive_it(void)
{
    yamaha_fm_synth_reset();
    play(0, 16, 60, 100);
    render_ms(200);
    TEST_ASSERT_TRUE(rms_window(100, 50) > 300.0);
}

void test_an_unstamped_event_is_applied_at_once(void)
{
    play(0, 16, 60, 100);
    render_ms(100);
    TEST_ASSERT_TRUE(rms_window(20, 40) > 300.0);
}

void test_repeating_the_same_controller_value_does_not_fill_the_ring(void)
{
    yamaha_fm_control_change(0, 11, 100);
    yamaha_fm_control_change(0, 7, 90);
    yamaha_fm_pitch_bend(0, 8192);
    render_ms(30);
    yamaha_fm_reset_event_stats();
    for (int i = 0; i < 1000; i++) {
        yamaha_fm_control_change(0, 11, 100);
        yamaha_fm_control_change(0, 7, 90);
        yamaha_fm_pitch_bend(0, 8192);
    }
    yamaha_fm_event_stats_t st;
    yamaha_fm_get_event_stats(&st);
    TEST_ASSERT_TRUE(st.ring_peak <= 3U);
    TEST_ASSERT_EQUAL_UINT32(0, st.dropped_events);
}

void test_a_controller_reset_lets_the_same_value_through_again(void)
{
    yamaha_fm_control_change(0, 11, 20);
    yamaha_fm_control_change(0, 121, 0);
    yamaha_fm_control_change(0, 11, 20);
    render_ms(30);
    TEST_ASSERT_EQUAL_UINT8(20, s_channels[0].expression);
}

void test_reverb_send_leaves_a_tail_and_no_send_leaves_none(void)
{
    yamaha_fm_set_effects_level(100);
    yamaha_fm_control_change(0, 91, 127);
    play(0, 16, 60, 100);
    render_ms(300);
    yamaha_fm_note_off(0, 60, 0);
    render_ms(900);
    double wet = rms_window(700, 100);

    setUp();
    yamaha_fm_set_effects_level(100);
    yamaha_fm_control_change(0, 91, 0);
    play(0, 16, 60, 100);
    render_ms(300);
    yamaha_fm_note_off(0, 60, 0);
    render_ms(900);
    double dry = rms_window(700, 100);

    TEST_ASSERT_TRUE(wet > 40.0);
    TEST_ASSERT_TRUE(dry < 3.0);
}

void test_effects_level_zero_removes_the_tail(void)
{
    yamaha_fm_set_effects_level(0);
    yamaha_fm_control_change(0, 91, 127);
    play(0, 16, 60, 100);
    render_ms(300);
    yamaha_fm_note_off(0, 60, 0);
    render_ms(900);
    TEST_ASSERT_TRUE(rms_window(700, 100) < 3.0);
}

void test_chorus_send_changes_the_sound_of_a_held_note(void)
{
    yamaha_fm_set_effects_level(100);
    yamaha_fm_control_change(0, 93, 127);
    play(0, 48, 60, 100);
    render_ms(1500);
    double sum = 0.0;
    size_t a = RATE;
    for (size_t i = 0; i < 20000; i++) {
        double d = (double)g_pcm[(a + i) * 2] - (double)g_pcm[(a + i) * 2 + 1];
        sum += d * d;
    }
    TEST_ASSERT_TRUE(sqrt(sum / 20000.0) > 30.0);     /* the sides differ; a centered dry note gives 0 */
}

void test_the_master_stage_keeps_a_full_mix_under_the_ceiling(void)
{
    for (int i = 0; i < 70; i++) {
        play((uint8_t)(i % 8), (uint8_t)(i * 2), (uint8_t)(30 + i), 127);
    }
    render_ms(2500);
    int peak = 0;
    for (size_t i = 0; i < g_frames * 2; i++) {
        int a = abs(g_pcm[i]);
        peak = a > peak ? a : peak;
    }
    TEST_ASSERT_TRUE(peak <= (int)(DSP_CEILING * 32767.0f) + 1);
    TEST_ASSERT_TRUE(peak > 8000);
}

/* ---- third operator ---- */

static double side_to_mid_db(unsigned from_ms, unsigned len_ms)
{
    size_t a = (size_t)from_ms * RATE / 1000U;
    size_t n = (size_t)len_ms * RATE / 1000U;
    double m = 0.0, d = 0.0;

    for (size_t i = 0; i < n; i++) {
        double l = g_pcm[(a + i) * 2], r = g_pcm[(a + i) * 2 + 1];
        m += ((l + r) / 2.0) * ((l + r) / 2.0);
        d += ((l - r) / 2.0) * ((l - r) / 2.0);
    }
    return 10.0 * log10((d + 1e-9) / (m + 1e-9));
}

/* High-frequency energy of the left channel in a window: the second difference rises at
 * 12 dB per octave, so the loud fundamental of the note hardly counts and the partials do. */
static double highs(unsigned from_ms, unsigned len_ms)
{
    size_t a = (size_t)from_ms * RATE / 1000U;
    size_t n = (size_t)len_ms * RATE / 1000U;
    double e = 0.0;

    for (size_t i = 2; i < n; i++) {
        double d = (double)g_pcm[(a + i) * 2] - 2.0 * (double)g_pcm[(a + i - 1) * 2] +
                   (double)g_pcm[(a + i - 2) * 2];
        e += d * d;
    }
    return e;
}

void test_strings_with_a_third_operator_spread_across_the_stereo_field(void)
{
    yamaha_fm_set_third_operator(false);
    play(0, 48, 60, 100);
    render_ms(1500);
    double two_op = side_to_mid_db(600, 800);

    setUp();
    yamaha_fm_set_third_operator(true);
    play(0, 48, 60, 100);
    render_ms(1500);
    double three_op = side_to_mid_db(600, 800);

    TEST_ASSERT_TRUE(two_op < -20.0);
    TEST_ASSERT_TRUE(three_op > two_op + 12.0);
}

void test_the_third_operator_keeps_the_level_of_the_two_operator_voice(void)
{
    static const uint8_t progs[] = {48, 40, 52, 89, 62, 42, 0};

    for (unsigned k = 0; k < sizeof(progs); k++) {
        setUp();
        yamaha_fm_set_third_operator(false);
        play(0, progs[k], 57, 100);
        render_ms(1400);
        double a = rms_window(500, 600);

        setUp();
        yamaha_fm_set_third_operator(true);
        play(0, progs[k], 57, 100);
        render_ms(1400);
        double b = rms_window(500, 600);

        char msg[48];
        snprintf(msg, sizeof(msg), "program %u changes level", progs[k]);
        TEST_ASSERT_TRUE_MESSAGE(b > a * 0.7 && b < a * 1.35, msg);
    }
}

void test_the_piano_third_operator_adds_a_bright_attack_that_fades(void)
{
    yamaha_fm_set_third_operator(false);
    play(0, 0, 60, 110);
    render_ms(1200);
    double plain_attack = highs(0, 40);
    double plain_late = highs(600, 400);

    setUp();
    yamaha_fm_set_third_operator(true);
    play(0, 0, 60, 110);
    render_ms(1200);
    double tine_attack = highs(0, 40);
    double tine_late = highs(600, 400);

    TEST_ASSERT_TRUE(tine_attack > plain_attack * 2.0);          /* the tine: about 2.5x in the first 40 ms */
    TEST_ASSERT_TRUE(tine_late < plain_late * 1.05);             /* and gone by the time the note has settled */
}

void test_notes_that_start_while_busy_stay_two_operator(void)
{
    uint32_t started, skipped;

    yamaha_fm_set_third_operator(true);
    for (int i = 0; i < THIRD_OP_MAX_VOICES + 6; i++) {
        play(1, 16, (uint8_t)(30 + i), 80);                  /* organ: never a third operator */
    }
    render_ms(40);
    yamaha_fm_get_third_operator_stats(&started, &skipped);
    uint32_t before_started = started;

    for (int i = 0; i < 4; i++) {
        play(0, 48, (uint8_t)(60 + i), 100);                 /* strings while the player is busy */
    }
    render_ms(40);
    yamaha_fm_get_third_operator_stats(&started, &skipped);
    TEST_ASSERT_EQUAL_UINT32(before_started, started);
    TEST_ASSERT_TRUE(skipped >= 4U);
}

void test_the_third_operator_is_used_when_the_player_is_quiet(void)
{
    uint32_t started, skipped;

    yamaha_fm_set_third_operator(true);
    play(0, 48, 60, 100);
    play(0, 48, 64, 100);
    render_ms(40);
    yamaha_fm_get_third_operator_stats(&started, &skipped);
    TEST_ASSERT_EQUAL_UINT32(2, started);
    TEST_ASSERT_EQUAL_UINT32(0, skipped);
}

void test_switching_the_third_operator_off_restores_the_two_operator_sound_exactly(void)
{
    yamaha_fm_set_third_operator(false);
    play(0, 48, 60, 100);
    render_ms(600);
    static int16_t ref[44100 * 2];
    memcpy(ref, g_pcm, (size_t)600 * RATE / 1000U * 4U);
    size_t keep = (size_t)600 * RATE / 1000U;

    setUp();
    yamaha_fm_set_third_operator(false);
    play(0, 48, 60, 100);
    render_ms(600);
    TEST_ASSERT_EQUAL_INT16_ARRAY(ref, g_pcm, keep * 2U);
    yamaha_fm_set_third_operator(true);
}

void test_ensemble_voices_stay_in_tune_at_both_ends_of_the_keyboard(void)
{
    static const uint8_t notes[] = {24, 48, 72, 84};   /* pitch_hz reads up to about 1.4 kHz */

    for (unsigned k = 0; k < sizeof(notes); k++) {
        setUp();
        yamaha_fm_set_third_operator(true);
        play(0, 48, notes[k], 100);
        render_ms(1500);
        double hz = pitch_hz(400);
        double want = 440.0 * pow(2.0, ((double)notes[k] - 69.0) / 12.0);
        TEST_ASSERT_FLOAT_WITHIN((float)(want * 0.02), (float)want, (float)hz);
    }
}

void test_bass_and_brass_patches_engage_four_operator_mode(void)
{
    uint32_t started = 0, skipped = 0;

    yamaha_fm_set_third_operator(true);
    play(0, 32, 40, 100);   /* Acoustic Bass */
    play(1, 33, 43, 100);   /* Finger Bass */
    play(2, 56, 60, 100);   /* Trumpet */
    play(3, 61, 64, 100);   /* Brass Section */
    render_ms(50);
    yamaha_fm_get_third_operator_stats(&started, &skipped);
    TEST_ASSERT_EQUAL_UINT32(4, started);
    TEST_ASSERT_EQUAL_UINT32(0, skipped);
}

void test_electronic_808_kit_has_longer_sub_bass_tail_than_standard_kick(void)
{
    yamaha_fm_program_change(9, 0);     /* Standard Kit */
    yamaha_fm_note_on(9, 36, 110);
    render_ms(600);
    double std_tail = rms_window(250, 200);

    setUp();
    yamaha_fm_program_change(9, 25);    /* TR-808 Electronic Kit */
    yamaha_fm_note_on(9, 36, 110);
    render_ms(600);
    double tr808_tail = rms_window(250, 200);

    TEST_ASSERT_TRUE(tr808_tail > std_tail * 2.0);
}

void test_accompaniment_ducks_when_melody_channel_is_singing(void)
{
    /* 1. Play accompaniment alone on channel 1 */
    yamaha_fm_set_melody_channel(0);
    play(1, 48, 55, 100);       /* Strings accompaniment */
    render_ms(400);
    double solo_acc = rms_window(200, 150);

    /* 2. Play accompaniment with lead melody singing on channel 0 */
    setUp();
    yamaha_fm_set_melody_channel(0);
    play(1, 48, 55, 100);       /* Strings accompaniment */
    play(0, 71, 72, 100);       /* Clarinet lead melody */
    render_ms(400);

    /* When melody is muted, accompaniment should have been attenuated by ~2.5 dB */
    setUp();
    yamaha_fm_set_melody_channel(0);
    play(1, 48, 55, 100);
    play(0, 71, 72, 100);
    render_ms(300);
    yamaha_fm_control_change(0, 7, 0);  /* mute melody channel */
    render_ms(50);
    double ducked_acc = rms_window(310, 30);

    TEST_ASSERT_TRUE(ducked_acc < solo_acc * 0.88);
}

void test_opl3_waveforms_enrich_clarinet_and_oboe_harmonics(void)
{
    play(0, 73, 60, 100);   /* Flute: Sine */
    render_ms(300);
    double flute = brightness(100, 100);

    setUp();
    play(0, 71, 60, 100);   /* Clarinet: Half-Sine */
    render_ms(300);
    double clar = brightness(100, 100);

    setUp();
    play(0, 68, 60, 100);   /* Oboe: Absolute-Sine */
    render_ms(300);
    double oboe = brightness(100, 100);

    TEST_ASSERT_TRUE(clar > flute * 1.15);
    TEST_ASSERT_TRUE(oboe > clar * 1.5);
}

void test_all_eight_opl3_waveforms_generate_distinct_shapes(void)
{
    TEST_ASSERT_EQUAL_UINT32(8, NUM_OPL3_WAVES);

    /* 0: Standard Sine has both positive and negative peaks */
    int min0 = 32767, max0 = -32768;
    for (int i = 0; i < SINE_SIZE; i++) {
        if (s_waves[WAVE_SINE][i] < min0) min0 = s_waves[WAVE_SINE][i];
        if (s_waves[WAVE_SINE][i] > max0) max0 = s_waves[WAVE_SINE][i];
    }
    TEST_ASSERT_TRUE(min0 < -30000 && max0 > 30000);

    /* 1: Half-Sine has no negative values */
    for (int i = 0; i < SINE_SIZE; i++) {
        TEST_ASSERT_TRUE(s_waves[WAVE_HALF][i] >= 0);
    }

    /* 2: Absolute-Sine has no negative values */
    for (int i = 0; i < SINE_SIZE; i++) {
        TEST_ASSERT_TRUE(s_waves[WAVE_ABS][i] >= 0);
    }

    /* 3: Quarter-Sine is zero in quadrants 1 and 3 */
    TEST_ASSERT_EQUAL_INT16(0, s_waves[WAVE_QUARTER][SINE_SIZE * 3 / 8]);
    TEST_ASSERT_EQUAL_INT16(0, s_waves[WAVE_QUARTER][SINE_SIZE * 7 / 8]);

    /* 4: Alternating-Sine is zero in the second half */
    for (int i = SINE_SIZE / 2; i < SINE_SIZE; i++) {
        TEST_ASSERT_EQUAL_INT16(0, s_waves[WAVE_ALT][i]);
    }

    /* 5: Camel-Sine is zero in the second half and non-negative in the first */
    for (int i = 0; i < SINE_SIZE / 2; i++) {
        TEST_ASSERT_TRUE(s_waves[WAVE_CAMEL][i] >= 0);
    }
    for (int i = SINE_SIZE / 2; i < SINE_SIZE; i++) {
        TEST_ASSERT_EQUAL_INT16(0, s_waves[WAVE_CAMEL][i]);
    }

    /* 6: Square wave has constant amplitude steps */
    TEST_ASSERT_EQUAL_INT16(28000, s_waves[WAVE_SQUARE][100]);
    TEST_ASSERT_EQUAL_INT16(-28000, s_waves[WAVE_SQUARE][SINE_SIZE / 2 + 100]);

    /* 7: Log-Saw has positive ramp in first half and negative in second */
    TEST_ASSERT_TRUE(s_waves[WAVE_LOG_SAW][50] > 0);
    TEST_ASSERT_TRUE(s_waves[WAVE_LOG_SAW][SINE_SIZE / 2 + 50] < 0);
}

void test_six_operator_dx7_mode_engages_when_voice_count_under_eighteen(void)
{
    yamaha_fm_set_third_operator(true);
    play(0, 48, 60, 100);   /* Strings in quiet playback */
    render_ms(30);

    /* Look up the voice allocated for channel 0, note 60 */
    bool found_6op = false;
    for (int i = 0; i < FM_MAX_VOICES; i++) {
        if (s_voices[i].active && s_voices[i].channel == 0 && s_voices[i].note == 60) {
            found_6op = (s_voices[i].ops == 6);
            break;
        }
    }
    TEST_ASSERT_TRUE(found_6op);
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
    RUN_TEST(test_melody_gain_scales_only_the_chosen_channel);
    RUN_TEST(test_melody_gain_does_nothing_without_a_melody_channel);
    RUN_TEST(test_melody_gain_is_clamped_and_drum_channel_is_refused);
    RUN_TEST(test_a_stamped_event_sounds_at_its_frame);
    RUN_TEST(test_events_keep_their_order_when_stamps_differ);
    RUN_TEST(test_a_song_change_drops_events_still_waiting_in_the_ring);
    RUN_TEST(test_events_pushed_after_a_reset_survive_it);
    RUN_TEST(test_an_unstamped_event_is_applied_at_once);
    RUN_TEST(test_repeating_the_same_controller_value_does_not_fill_the_ring);
    RUN_TEST(test_a_controller_reset_lets_the_same_value_through_again);
    RUN_TEST(test_reverb_send_leaves_a_tail_and_no_send_leaves_none);
    RUN_TEST(test_effects_level_zero_removes_the_tail);
    RUN_TEST(test_chorus_send_changes_the_sound_of_a_held_note);
    RUN_TEST(test_the_master_stage_keeps_a_full_mix_under_the_ceiling);
    RUN_TEST(test_strings_with_a_third_operator_spread_across_the_stereo_field);
    RUN_TEST(test_the_third_operator_keeps_the_level_of_the_two_operator_voice);
    RUN_TEST(test_the_piano_third_operator_adds_a_bright_attack_that_fades);
    RUN_TEST(test_notes_that_start_while_busy_stay_two_operator);
    RUN_TEST(test_the_third_operator_is_used_when_the_player_is_quiet);
    RUN_TEST(test_switching_the_third_operator_off_restores_the_two_operator_sound_exactly);
    RUN_TEST(test_ensemble_voices_stay_in_tune_at_both_ends_of_the_keyboard);
    RUN_TEST(test_bass_and_brass_patches_engage_four_operator_mode);
    RUN_TEST(test_electronic_808_kit_has_longer_sub_bass_tail_than_standard_kick);
    RUN_TEST(test_accompaniment_ducks_when_melody_channel_is_singing);
    RUN_TEST(test_opl3_waveforms_enrich_clarinet_and_oboe_harmonics);
    RUN_TEST(test_all_eight_opl3_waveforms_generate_distinct_shapes);
    RUN_TEST(test_six_operator_dx7_mode_engages_when_voice_count_under_eighteen);
    return UNITY_END();
}
