/**
 * @file test_synth_dsp.c
 * @brief Host tests for the reverb, chorus, compressor and look-ahead limiter.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include <unity.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/synth_dsp.c"

#define RATE 44100

void setUp(void)
{
    dsp_init((float)RATE);
}

void tearDown(void) {}

/* Pushes a mono signal through the master stage in 32-frame blocks; both channels carry it. */
static void master_run(const int32_t *in, int16_t *out_l, int16_t *out_r, size_t frames, uint32_t q8)
{
    int32_t l[DSP_BLOCK_FRAMES];
    int16_t o[DSP_BLOCK_FRAMES * 2];

    for (size_t at = 0; at < frames; at += DSP_BLOCK_FRAMES) {
        size_t n = frames - at < DSP_BLOCK_FRAMES ? frames - at : DSP_BLOCK_FRAMES;
        memcpy(l, in + at, n * sizeof(int32_t));
        dsp_master_process(l, l, o, n, q8);
        for (size_t i = 0; i < n; i++) {
            out_l[at + i] = o[i * 2U];
            out_r[at + i] = o[i * 2U + 1U];
        }
    }
}

static double rms_i16(const int16_t *x, size_t n)
{
    double s = 0.0;

    for (size_t i = 0; i < n; i++) {
        s += (double)x[i] * (double)x[i];
    }
    return sqrt(s / (double)n);
}

static double db(double ratio)
{
    return 20.0 * log10(ratio);
}

static void fill_sine(int32_t *x, size_t n, double hz, double peak)
{
    for (size_t i = 0; i < n; i++) {
        x[i] = (int32_t)lrint(peak * sin(2.0 * 3.14159265358979 * hz * (double)i / RATE));
    }
}

/* ------------------------------------------------------------------------ */
/* Limiter                                                                  */
/* ------------------------------------------------------------------------ */
void test_a_quiet_signal_passes_through_delayed_and_unchanged(void)
{
    static int32_t in[8000];
    static int16_t ol[8000], orr[8000];

    dsp_set_compressor(false);
    fill_sine(in, 8000, 440.0, 6000.0);
    master_run(in, ol, orr, 8000, 256);

    for (size_t i = DSP_LIMITER_DELAY; i < 8000; i++) {
        int d = (int)ol[i] - (int)in[i - DSP_LIMITER_DELAY];
        TEST_ASSERT_TRUE_MESSAGE(d >= -1 && d <= 1, "signal changed or the delay is not DSP_LIMITER_DELAY");
    }
    TEST_ASSERT_EQUAL_UINT32(0, dsp_get_limiter_samples());
}

void test_the_output_never_goes_over_the_ceiling(void)
{
    static int32_t in[44100];
    static int16_t ol[44100], orr[44100];
    const int limit = (int)(DSP_CEILING * 32767.0f) + 1;

    dsp_set_compressor(false);
    srand(5);
    for (size_t i = 0; i < 44100; i++) {
        double burst = ((i / 3000U) % 3U == 0U) ? 6.0 : 0.6;     /* repeated loud bursts */
        in[i] = (int32_t)(burst * 30000.0 * sin(0.05 * (double)i)) + (rand() % 20000) - 10000;
    }
    master_run(in, ol, orr, 44100, 256);

    int peak = 0;
    for (size_t i = 0; i < 44100; i++) {
        int a = abs(ol[i]);
        peak = a > peak ? a : peak;
    }
    TEST_ASSERT_TRUE_MESSAGE(peak <= limit, "the limiter let a peak through");
    TEST_ASSERT_TRUE(peak > limit - 600);       /* and it did not just turn everything down */
    TEST_ASSERT_TRUE(dsp_get_limiter_samples() > 0U);
}

void test_a_single_huge_spike_is_caught_before_it_arrives(void)
{
    static int32_t in[4000];
    static int16_t ol[4000], orr[4000];
    const int limit = (int)(DSP_CEILING * 32767.0f) + 1;

    dsp_set_compressor(false);
    in[1000] = 400000;
    master_run(in, ol, orr, 4000, 256);

    TEST_ASSERT_TRUE(abs(ol[1000 + DSP_LIMITER_DELAY]) <= limit);
    TEST_ASSERT_TRUE(abs(ol[1000 + DSP_LIMITER_DELAY]) > limit / 2);
}

void test_limiter_gain_recovers_after_a_peak(void)
{
    static int32_t in[44100];
    static int16_t ol[44100], orr[44100];

    dsp_set_compressor(false);
    fill_sine(in, 44100, 300.0, 5000.0);
    for (int i = 2000; i < 2200; i++) {
        in[i] = (i % 2 == 0) ? 120000 : -120000;
    }
    master_run(in, ol, orr, 44100, 256);

    double before = rms_i16(ol + 200, 1000);
    double just_after = rms_i16(ol + 2400, 1000);
    double much_later = rms_i16(ol + 40000, 2000);
    TEST_ASSERT_TRUE(just_after < before * 0.9);
    TEST_ASSERT_FLOAT_WITHIN((float)before * 0.03f, (float)before, (float)much_later);
}

void test_master_gain_scales_the_output(void)
{
    static int32_t in[6000];
    static int16_t ol[6000], orr[6000];

    dsp_set_compressor(false);
    fill_sine(in, 6000, 440.0, 4000.0);
    master_run(in, ol, orr, 6000, 512);                       /* x2 */
    double r = rms_i16(ol + 500, 4000) / (4000.0 / sqrt(2.0));
    TEST_ASSERT_FLOAT_WITHIN(0.03f, 2.0f, (float)r);
}

void test_silence_in_gives_silence_out(void)
{
    static int32_t in[4000];
    static int16_t ol[4000], orr[4000];

    master_run(in, ol, orr, 4000, 256);
    for (size_t i = 0; i < 4000; i++) {
        TEST_ASSERT_EQUAL_INT16(0, ol[i]);
        TEST_ASSERT_EQUAL_INT16(0, orr[i]);
    }
}

void test_blocks_shorter_than_32_frames_work(void)
{
    int32_t in[7];
    int16_t o[14];

    for (int i = 0; i < 7; i++) {
        in[i] = 1000 * (i + 1);
    }
    dsp_master_process(in, in, o, 7, 256);
    dsp_master_process(in, in, o, 1, 256);
    TEST_ASSERT_TRUE(1);
}

/* ------------------------------------------------------------------------ */
/* Compressor                                                               */
/* ------------------------------------------------------------------------ */
/* Level change through the master stage for a steady 1 kHz tone, after it has settled. */
static double settled_gain_db(double peak)
{
    static int32_t in[44100];
    static int16_t ol[44100], orr[44100];

    dsp_init((float)RATE);
    fill_sine(in, 44100, 1000.0, peak);
    master_run(in, ol, orr, 44100, 256);
    return db(rms_i16(ol + 30000, 8000) / (peak / sqrt(2.0)));
}

void test_a_quiet_signal_gets_only_the_makeup_gain(void)
{
    double g = settled_gain_db(300.0);        /* about -40 dBFS, far under the threshold */
    TEST_ASSERT_FLOAT_WITHIN(0.4f, DSP_COMP_MAKEUP_DB, (float)g);
}

void test_loud_signals_are_pulled_down_more_than_quiet_ones(void)
{
    double quiet = settled_gain_db(3000.0);
    double loud = settled_gain_db(18000.0);
    TEST_ASSERT_TRUE(loud < quiet - 3.0);
}

void test_the_compressor_follows_the_ratio_above_the_knee(void)
{
    /* Two levels above the knee: each extra dB in comes out 1/ratio dB louder. */
    double lo = 20.0 * log10(10000.0 / 32768.0);
    double hi = 20.0 * log10(20000.0 / 32768.0);
    double g_lo = settled_gain_db(10000.0);
    double g_hi = settled_gain_db(20000.0);
    double out_step = (hi + g_hi) - (lo + g_lo);
    double in_step = hi - lo;

    TEST_ASSERT_TRUE(lo > DSP_COMP_THRESH_DB + DSP_COMP_KNEE_DB * 0.5);
    TEST_ASSERT_FLOAT_WITHIN(0.35f, (float)(in_step / DSP_COMP_RATIO), (float)out_step);
}

void test_compressor_off_leaves_the_level_alone(void)
{
    static int32_t in[44100];
    static int16_t ol[44100], orr[44100];

    dsp_set_compressor(false);
    fill_sine(in, 44100, 1000.0, 14000.0);
    master_run(in, ol, orr, 44100, 256);
    double g = db(rms_i16(ol + 30000, 8000) / (14000.0 / sqrt(2.0)));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 0.0f, (float)g);
}

void test_the_compressor_lets_go_after_a_loud_passage(void)
{
    static int32_t in[88200];
    static int16_t ol[88200], orr[88200];

    fill_sine(in, 88200, 1000.0, 2000.0);
    for (size_t i = 10000; i < 20000; i++) {
        in[i] = (int32_t)(26000.0 * sin(2.0 * 3.14159265358979 * 1000.0 * (double)i / RATE));
    }
    master_run(in, ol, orr, 88200, 256);

    double early = rms_i16(ol + 2000, 4000);         /* quiet, before the burst */
    double right_after = rms_i16(ol + 20500, 1500);  /* quiet again, compressor still down */
    double later = rms_i16(ol + 80000, 4000);
    TEST_ASSERT_TRUE(right_after < early * 0.95);
    TEST_ASSERT_FLOAT_WITHIN((float)early * 0.04f, (float)early, (float)later);
}

void test_reduction_meter_reports_and_resets(void)
{
    static int32_t in[20000];
    static int16_t ol[20000], orr[20000];

    fill_sine(in, 20000, 1000.0, 26000.0);
    master_run(in, ol, orr, 20000, 256);
    TEST_ASSERT_TRUE(dsp_take_reduction_db10() > 20);       /* more than 2 dB */
    TEST_ASSERT_EQUAL_INT(0, dsp_take_reduction_db10());
}

/* ------------------------------------------------------------------------ */
/* Reverb                                                                   */
/* ------------------------------------------------------------------------ */
static void fx_run(const int32_t *rev, const int32_t *cho, int32_t *l, int32_t *r, size_t frames)
{
    for (size_t at = 0; at < frames; at += DSP_BLOCK_FRAMES) {
        size_t n = frames - at < DSP_BLOCK_FRAMES ? frames - at : DSP_BLOCK_FRAMES;
        dsp_effects_process(rev + at, cho + at, l + at, r + at, n);
    }
}

static double rms_i32(const int32_t *x, size_t n)
{
    double s = 0.0;

    for (size_t i = 0; i < n; i++) {
        s += (double)x[i] * (double)x[i];
    }
    return sqrt(s / (double)n);
}

#define TAIL_FRAMES (RATE * 6)
static int32_t g_rev[TAIL_FRAMES], g_cho[TAIL_FRAMES], g_l[TAIL_FRAMES], g_r[TAIL_FRAMES];

static void impulse_response(int32_t level)
{
    memset(g_rev, 0, sizeof(g_rev));
    memset(g_cho, 0, sizeof(g_cho));
    memset(g_l, 0, sizeof(g_l));
    memset(g_r, 0, sizeof(g_r));
    g_rev[0] = level;
    fx_run(g_rev, g_cho, g_l, g_r, TAIL_FRAMES);
}

void test_reverb_rings_and_then_dies_away(void)
{
    impulse_response(30000);

    double w0 = rms_i32(g_l + 2000, 4000);
    double w1 = rms_i32(g_l + 22000, 4000);
    double w2 = rms_i32(g_l + 62000, 4000);
    TEST_ASSERT_TRUE(w0 > 20.0);
    TEST_ASSERT_TRUE(w1 < w0);
    TEST_ASSERT_TRUE(w1 > 0.5);           /* still ringing half a second in */
    TEST_ASSERT_TRUE(w2 < w1);
}

void test_reverb_decay_time_is_close_to_the_setting(void)
{
    impulse_response(32000);

    /* Backward-integrated energy decay: time for the tail energy to fall by 30 dB, doubled. */
    static double tail[TAIL_FRAMES];
    double acc = 0.0;
    for (long i = TAIL_FRAMES - 1; i >= 0; i--) {
        acc += (double)g_l[i] * (double)g_l[i];
        tail[i] = acc;
    }
    double total = tail[0];
    long t30 = -1;
    for (long i = 0; i < TAIL_FRAMES; i++) {
        if (tail[i] < total * 0.001) {
            t30 = i;
            break;
        }
    }
    TEST_ASSERT_TRUE(t30 > 0);
    double t60 = 2.0 * (double)t30 / RATE;
    TEST_ASSERT_TRUE_MESSAGE(t60 > DSP_REVERB_T60_S * 0.35 && t60 < DSP_REVERB_T60_S * 1.3,
                             "decay time is far from DSP_REVERB_T60_S");
}

void test_reverb_tail_ends_in_exact_silence(void)
{
    impulse_response(32000);
    for (size_t i = TAIL_FRAMES - 4000; i < TAIL_FRAMES; i++) {
        TEST_ASSERT_EQUAL_INT32(0, g_l[i]);
        TEST_ASSERT_EQUAL_INT32(0, g_r[i]);
    }
}

void test_reverb_channels_are_different(void)
{
    impulse_response(30000);

    double ll = 0.0, rr = 0.0, lr = 0.0;
    for (size_t i = 3000; i < 40000; i++) {
        ll += (double)g_l[i] * g_l[i];
        rr += (double)g_r[i] * g_r[i];
        lr += (double)g_l[i] * g_r[i];
    }
    double corr = lr / sqrt(ll * rr);
    TEST_ASSERT_TRUE(ll > 0.0 && rr > 0.0);
    TEST_ASSERT_TRUE(fabs(corr) < 0.6);
}

void test_loud_continuous_input_does_not_blow_the_reverb_up(void)
{
    for (size_t i = 0; i < RATE * 2U; i++) {
        g_rev[i] = (i % 7U < 4U) ? 120000 : -120000;
        g_cho[i] = 0;
        g_l[i] = 0;
        g_r[i] = 0;
    }
    fx_run(g_rev, g_cho, g_l, g_r, RATE * 2U);

    memset(g_rev, 0, sizeof(g_rev));
    memset(g_l, 0, sizeof(g_l));
    memset(g_r, 0, sizeof(g_r));
    fx_run(g_rev, g_cho, g_l, g_r, TAIL_FRAMES);
    for (size_t i = TAIL_FRAMES - 2000; i < TAIL_FRAMES; i++) {
        TEST_ASSERT_EQUAL_INT32(0, g_l[i]);
    }
}

void test_effects_level_scales_the_return_and_zero_switches_it_off(void)
{
    dsp_set_effects_level(100);
    impulse_response(30000);
    double full = rms_i32(g_l + 2000, 6000);

    dsp_init((float)RATE);
    dsp_set_effects_level(50);
    impulse_response(30000);
    double half = rms_i32(g_l + 2000, 6000);
    TEST_ASSERT_FLOAT_WITHIN((float)full * 0.05f, (float)full * 0.5f, (float)half);

    dsp_init((float)RATE);
    dsp_set_effects_level(0);
    TEST_ASSERT_FALSE(dsp_effects_active());
    impulse_response(30000);
    for (size_t i = 0; i < 10000; i++) {
        TEST_ASSERT_EQUAL_INT32(0, g_l[i]);
    }
    dsp_set_effects_level(250);
    TEST_ASSERT_EQUAL_UINT8(200, dsp_get_effects_level());
}

void test_clear_removes_the_tail(void)
{
    impulse_response(30000);
    memset(g_rev, 0, sizeof(g_rev));
    dsp_init((float)RATE);
    impulse_response(0);
    for (size_t i = 0; i < 20000; i++) {
        TEST_ASSERT_EQUAL_INT32(0, g_l[i]);
    }
}

/* ------------------------------------------------------------------------ */
/* Chorus                                                                   */
/* ------------------------------------------------------------------------ */
void test_chorus_adds_a_moving_copy_on_both_sides(void)
{
    memset(g_rev, 0, sizeof(g_rev));
    memset(g_l, 0, sizeof(g_l));
    memset(g_r, 0, sizeof(g_r));
    for (size_t i = 0; i < RATE * 3U; i++) {
        g_cho[i] = (int32_t)(10000.0 * sin(2.0 * 3.14159265358979 * 523.0 * (double)i / RATE));
    }
    fx_run(g_rev, g_cho, g_l, g_r, RATE * 3U);

    double lv = rms_i32(g_l + 20000, 40000);
    double rv = rms_i32(g_r + 20000, 40000);
    TEST_ASSERT_TRUE(lv > 500.0);
    TEST_ASSERT_TRUE(rv > 500.0);

    long diff = 0;
    for (size_t i = 20000; i < 60000; i++) {
        diff += (g_l[i] != g_r[i]) ? 1 : 0;
    }
    TEST_ASSERT_TRUE(diff > 30000);          /* the two sides are not the same signal */
}

void test_chorus_without_input_stays_silent(void)
{
    memset(g_rev, 0, sizeof(g_rev));
    memset(g_cho, 0, sizeof(g_cho));
    memset(g_l, 0, sizeof(g_l));
    memset(g_r, 0, sizeof(g_r));
    fx_run(g_rev, g_cho, g_l, g_r, RATE);
    for (size_t i = 0; i < RATE; i++) {
        TEST_ASSERT_EQUAL_INT32(0, g_l[i]);
    }
}

void test_downward_expander_gates_faint_tail_below_minus_68db(void)
{
    static int32_t in[8000];
    static int16_t ol[8000], orr[8000];

    fill_sine(in, 8000, 1000.0, 5.0);   /* ~ -76 dBFS faint quantization tail */
    master_run(in, ol, orr, 8000, 256);
    for (size_t i = 2000; i < 8000; i++) {
        TEST_ASSERT_EQUAL_INT16(0, ol[i]);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_quiet_signal_passes_through_delayed_and_unchanged);
    RUN_TEST(test_the_output_never_goes_over_the_ceiling);
    RUN_TEST(test_a_single_huge_spike_is_caught_before_it_arrives);
    RUN_TEST(test_limiter_gain_recovers_after_a_peak);
    RUN_TEST(test_master_gain_scales_the_output);
    RUN_TEST(test_silence_in_gives_silence_out);
    RUN_TEST(test_blocks_shorter_than_32_frames_work);
    RUN_TEST(test_a_quiet_signal_gets_only_the_makeup_gain);
    RUN_TEST(test_loud_signals_are_pulled_down_more_than_quiet_ones);
    RUN_TEST(test_the_compressor_follows_the_ratio_above_the_knee);
    RUN_TEST(test_compressor_off_leaves_the_level_alone);
    RUN_TEST(test_the_compressor_lets_go_after_a_loud_passage);
    RUN_TEST(test_reduction_meter_reports_and_resets);
    RUN_TEST(test_reverb_rings_and_then_dies_away);
    RUN_TEST(test_reverb_decay_time_is_close_to_the_setting);
    RUN_TEST(test_reverb_tail_ends_in_exact_silence);
    RUN_TEST(test_reverb_channels_are_different);
    RUN_TEST(test_loud_continuous_input_does_not_blow_the_reverb_up);
    RUN_TEST(test_effects_level_scales_the_return_and_zero_switches_it_off);
    RUN_TEST(test_clear_removes_the_tail);
    RUN_TEST(test_chorus_adds_a_moving_copy_on_both_sides);
    RUN_TEST(test_chorus_without_input_stays_silent);
    RUN_TEST(test_downward_expander_gates_faint_tail_below_minus_68db);
    return UNITY_END();
}
