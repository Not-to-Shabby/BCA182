/**
 * @file test_features.c
 * @brief Host-native Unity suite for the on-device 63-feature extractor.
 *
 * Parity contract: src/audio_features.c must reproduce the PC trainer front
 * end (ml_model/extract_features.py) feature-for-feature.  The golden vector
 * in feature_golden.h was produced by the REAL Python pipeline
 * (gen_feature_golden.py -> extract_window on a documented synthetic int16
 * window), so this suite proves PC <-> C numerical parity, not C == C.
 *
 * src/audio_features.c is #included directly (LAB_4 pattern, so
 * build_src_filter stays untouched): the shipped extractor - not a mock -
 * is what runs.  cry_model.h is included to prove the extracted features
 * feed the frozen emlearn forest without adaptation.
 */

#include <unity.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/audio_features.c" /* LAB_4 pattern: compile the real thing */
#include "cry_model.h"
#include "feature_golden.h"

/* Static buffers: 32 KB each, kept out of the test stack. */
static int16_t s_zero[GOLDEN_WINDOW_LEN];
static int16_t s_alt[GOLDEN_WINDOW_LEN];
static int16_t s_quiet[GOLDEN_WINDOW_LEN];
static int16_t s_loud[GOLDEN_WINDOW_LEN];

void setUp(void)
{
    audio_features_init(); /* idempotent; contract: init before compute */
}

void tearDown(void)
{
}

/* ------------------------------------------------------------- helpers ---- */

static int16_t quantize_half_away(double v)
{
    double r = (v >= 0.0) ? floor(v + 0.5) : ceil(v - 0.5);
    if (r > 32767.0) {
        r = 32767.0;
    }
    if (r < -32768.0) {
        r = -32768.0;
    }
    return (int16_t)r;
}

/* Pure 440 Hz sine at a given full-scale amplitude, int16-quantized. */
static void fill_sine(int16_t *buf, double amplitude)
{
    uint32_t n;
    for (n = 0u; n < GOLDEN_WINDOW_LEN; n++) {
        buf[n] = quantize_half_away(
            amplitude * sin(2.0 * 3.14159265358979323846 * 440.0 *
                            (double)n / 16000.0) * 32768.0);
    }
}

static void assert_all_finite(const float *feat, const char *what)
{
    uint32_t i;
    for (i = 0u; i < AUDIO_FEAT_COUNT; i++) {
        if (!isfinite((double)feat[i])) {
            char msg[64];
            snprintf(msg, sizeof(msg), "%s: feature %u not finite",
                     what, (unsigned)i);
            TEST_FAIL_MESSAGE(msg);
        }
    }
}

/* --------------------------------------------------- 1. golden PC parity -- */

void test_golden_window_parity(void)
{
    float feat[GOLDEN_FEATURE_COUNT];
    double max_err = 0.0;
    uint32_t worst = 0u;
    uint32_t i;

    audio_features_compute(golden_window, GOLDEN_WINDOW_LEN, feat);

    /* pass 1: measure (no asserts) so the summary always prints */
    for (i = 0u; i < GOLDEN_FEATURE_COUNT; i++) {
        const double expected = (double)golden_features[i];
        const double err = fabs((double)feat[i] - expected);
        if (err > max_err) {
            max_err = err;
            worst = i;
        }
    }
    printf("test_features: golden parity max |err| = %.3e at %s\n",
           max_err, GOLDEN_FEATURE_NAMES[worst]);

    /* pass 2: assert |C - PC| <= max(1e-3, 1e-3*|PC|) per feature, i.e.
     * 1e-3 absolute or 1e-3 relative, whichever is larger (float32 FFT vs
     * numpy float64 FFT rounding budget). */
    for (i = 0u; i < GOLDEN_FEATURE_COUNT; i++) {
        const double expected = (double)golden_features[i];
        const double tol = 1e-3 * ((fabs(expected) > 1.0) ? fabs(expected)
                                                          : 1.0);
        const double err = fabs((double)feat[i] - expected);
        if (err > tol) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "feature %u (%s): expected %.7g, got %.7g, "
                     "err %.3e > tol %.3e",
                     (unsigned)i, GOLDEN_FEATURE_NAMES[i],
                     expected, (double)feat[i], err, tol);
            TEST_FAIL_MESSAGE(msg);
        }
    }
    TEST_PASS();
}

/* ------------------------------------------------------ 2. determinism ---- */

void test_compute_is_deterministic(void)
{
    float a[AUDIO_FEAT_COUNT];
    float b[AUDIO_FEAT_COUNT];

    audio_features_compute(golden_window, GOLDEN_WINDOW_LEN, a);
    audio_features_compute(golden_window, GOLDEN_WINDOW_LEN, b);
    TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof(a));

    memset(s_zero, 0, sizeof(s_zero));
    audio_features_compute(s_zero, GOLDEN_WINDOW_LEN, a);
    audio_features_compute(s_zero, GOLDEN_WINDOW_LEN, b);
    TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof(a));
}

/* ------------------------------------------------- 3. silence sanity ------
 * numpy floors: mel band energies at 1e-10, frame energy at 1e-12, so an
 * all-zero window must give finite features everywhere.  With constant
 * frames every MFCC std collapses to ~0, mfcc00_mean = sqrt(40)*log(1e-10)
 * (row 0 of the orthonormal DCT sums to sqrt(40), rows 1..12 sum to 0),
 * log_energy_mean = log(1e-12) and ZCR = 0 (signbit(+0.0) is false).
 * Feature-set v2: the delta sequences of constant frames are identically 0
 * (means and stds), and the spectral-shape guards define centroid /
 * rolloff / flatness as 0 for zero-power frames -- so every feature except
 * the two logarithmic means must be exactly ~0. */

void test_silence_window_features_finite(void)
{
    float feat[AUDIO_FEAT_COUNT];
    uint32_t i;

    memset(s_zero, 0, sizeof(s_zero));
    audio_features_compute(s_zero, GOLDEN_WINDOW_LEN, feat);
    assert_all_finite(feat, "silence");

    TEST_ASSERT_FLOAT_WITHIN(1e-3f, (float)log(1e-12), feat[26]);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, (float)(sqrt(40.0) * log(1e-10)), feat[0]);
    for (i = 0u; i < AUDIO_FEAT_COUNT; i++) {
        if ((i == 0u) || (i == 26u)) {
            continue; /* the two logarithmic means asserted above */
        }
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, feat[i]);
    }
}

/* ---------------------------------------------------- 4. ZCR definition --- */

void test_alternating_window_zcr_is_one(void)
{
    float feat[AUDIO_FEAT_COUNT];
    uint32_t i;

    /* +1 / -1 at 16 kHz: every adjacent pair inside every frame flips sign.
     * The hann[0] == 0 tap turns sample 0 into +0.0 (signbit false) in both
     * numpy and C (IEEE signed-zero product rule), so all 511 pairs count. */
    for (i = 0u; i < GOLDEN_WINDOW_LEN; i++) {
        s_alt[i] = (int16_t)(((i & 1u) != 0u) ? -1 : 1);
    }
    audio_features_compute(s_alt, GOLDEN_WINDOW_LEN, feat);
    assert_all_finite(feat, "alternating");
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, feat[28]);
}

/* ------------------------------------------------ 5. log-energy physics --- */

void test_louder_sine_has_higher_log_energy(void)
{
    float quiet[AUDIO_FEAT_COUNT];
    float loud[AUDIO_FEAT_COUNT];

    fill_sine(s_quiet, 0.05);
    fill_sine(s_loud, 0.25);
    audio_features_compute(s_quiet, GOLDEN_WINDOW_LEN, quiet);
    audio_features_compute(s_loud, GOLDEN_WINDOW_LEN, loud);
    assert_all_finite(quiet, "quiet sine");
    assert_all_finite(loud, "loud sine");

    /* 5x amplitude -> 25x energy -> log-energy gap ~ ln(25) = 3.22. */
    TEST_ASSERT_TRUE(loud[26] > quiet[26] + 1.0f);
}

/* ------------------------------- 6. v2 deltas track spectral motion ------
 * Feature-set v2's reason to exist: a window whose spectrum MOVES must
 * stand out in the delta features even when its per-window summary (v1
 * block) looks like a steady tone's.  A 500 Hz tone completes exactly
 * 16 cycles per 512-sample frame (8 per hop), so all 61 frames are
 * identical, the MFCC/log-energy delta sequences are identically zero and
 * every delta std collapses to ~0 (variance clamp).  A linear 400->900 Hz
 * sweep sweeps the spectral peak across mel bands frame by frame, which
 * must surface as a large MFCC-delta std (indices 42..54). */

void test_delta_features_track_spectral_motion(void)
{
    float steady[AUDIO_FEAT_COUNT];
    float swept[AUDIO_FEAT_COUNT];
    float max_steady = 0.0f;
    float max_swept = 0.0f;
    double phase = 0.0;
    uint32_t n;
    uint32_t i;

    for (n = 0u; n < GOLDEN_WINDOW_LEN; n++) {
        const double t = (double)n / 16000.0;
        s_quiet[n] = quantize_half_away(
            0.2 * sin(2.0 * 3.14159265358979323846 * 500.0 * t) * 32768.0);
        phase += 2.0 * 3.14159265358979323846 *
                 (400.0 + 500.0 * t) / 16000.0;
        s_loud[n] = quantize_half_away(0.2 * sin(phase) * 32768.0);
    }
    audio_features_compute(s_quiet, GOLDEN_WINDOW_LEN, steady);
    audio_features_compute(s_loud, GOLDEN_WINDOW_LEN, swept);
    assert_all_finite(steady, "steady tone");
    assert_all_finite(swept, "swept tone");

    for (i = 42u; i < 55u; i++) { /* mfcc00_delta_std .. mfcc12_delta_std */
        if (steady[i] > max_steady) {
            max_steady = steady[i];
        }
        if (swept[i] > max_swept) {
            max_swept = swept[i];
        }
    }
    printf("test_features: mfcc-delta std max: steady %.4g, swept %.4g\n",
           (double)max_steady, (double)max_swept);
    TEST_ASSERT_TRUE(max_steady < 0.01f);
    TEST_ASSERT_TRUE(max_swept > 0.10f);
}

/* ------------------------------------- 7. model feeds on the features ----- */

void test_model_predict_on_golden_features(void)
{
    float feat[AUDIO_FEAT_COUNT];
    int cls_a;
    int cls_b;
    uint8_t conf_a;
    uint8_t conf_b;
    cry_class_t voted_a;
    cry_class_t voted_b;

    audio_features_compute(golden_window, GOLDEN_WINDOW_LEN, feat);

    cls_a = cry_model_predict(feat);
    TEST_ASSERT_TRUE((cls_a == (int)CRY_PRED_NONCRY) ||
                     (cls_a == (int)CRY_PRED_CRY));

    conf_a = 0u;
    voted_a = cry_model_predict_confidence(feat, &conf_a);
    TEST_ASSERT_TRUE(conf_a <= 100u);
    TEST_ASSERT_TRUE((voted_a == CRY_PRED_NONCRY) ||
                     (voted_a == CRY_PRED_CRY));

    /* deterministic: identical inputs -> identical classification */
    cls_b = cry_model_predict(feat);
    conf_b = 0u;
    voted_b = cry_model_predict_confidence(feat, &conf_b);
    TEST_ASSERT_EQUAL_INT(cls_a, cls_b);
    TEST_ASSERT_EQUAL_INT((int)voted_a, (int)voted_b);
    TEST_ASSERT_EQUAL_INT(conf_a, conf_b);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_golden_window_parity);
    RUN_TEST(test_compute_is_deterministic);
    RUN_TEST(test_silence_window_features_finite);
    RUN_TEST(test_alternating_window_zcr_is_one);
    RUN_TEST(test_louder_sine_has_higher_log_energy);
    RUN_TEST(test_delta_features_track_spectral_motion);
    RUN_TEST(test_model_predict_on_golden_features);
    return UNITY_END();
}
