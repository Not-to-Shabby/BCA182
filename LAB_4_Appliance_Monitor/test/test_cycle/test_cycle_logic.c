/**
 * @file test_cycle_logic.c
 * @brief Host-native Unity tests for the Laboratory 4 decision chain:
 *        vibration features, baseline rules, trained forest, hysteresis state
 *        machine (incl. DONE derivation), and the service-mode transitions.
 *
 * The same C sources that run on the board are compiled here directly, so a
 * passing suite proves the decision logic, not a mock.
 */

#include <unity.h>
#include <math.h>
#include <string.h>

#include "../../src/vibration_features.c"
#include "../../src/cycle_model.c"
#include "../../src/cycle_logic.c"
#include "../../src/service_mode.c"

/* Build a window whose accel magnitude is exactly  mean + A*sin(2*pi*f*t)  by
 * putting the signal on ax alone with a DC offset larger than the amplitude.
 * push() reports true only when the ring turns full, i.e. on the last sample. */
static void push_sine(float mean, float amplitude, float freq_hz)
{
    vib_features_reset();
    for (uint16_t i = 0; i < VIB_WINDOW_SAMPLES; i++) {
        imu_sample_t s;
        s.ax = mean + amplitude * sinf(2.0f * 3.14159265f * freq_hz * (float)i / 100.0f);
        s.ay = 0.0f;
        s.az = 0.0f;
        s.gx = 0.0f;
        s.gy = 0.0f;
        s.gz = 0.0f;
        TEST_ASSERT_EQUAL(i == VIB_WINDOW_SAMPLES - 1U, vib_features_push(&s));
    }
}

/* Hand-built feature vectors inside the trainer's synthetic distributions */
static const float IDLE_FEATS[VIB_NUM_FEATURES] = {
    0.004f, 0.010f, 2.2f, 0.003f, 0.003f, 0.003f, 0.30f,
    0.33f, 0.33f, 0.33f, 25.0f, 0.002f, 0.010f, 0.05f
};
static const float WASH_FEATS[VIB_NUM_FEATURES] = {
    0.150f, 0.500f, 2.2f, 0.100f, 0.100f, 0.100f, 0.04f,
    0.80f, 0.15f, 0.02f, 2.0f, 0.050f, 0.300f, 0.90f
};
static const float SPIN_FEATS[VIB_NUM_FEATURES] = {
    0.300f, 0.900f, 2.1f, 0.200f, 0.200f, 0.200f, 0.24f,
    0.02f, 0.10f, 0.85f, 15.0f, 0.400f, 2.000f, 0.95f
};

void setUp(void)
{
    vib_features_reset();
    cycle_logic_reset();
    service_mode_init();
}

void tearDown(void)
{
}

/* ---------------------------------------------------------------- features */
void test_sine_at_12hz_yields_exact_features(void)
{
    push_sine(0.30f, 0.20f, 12.0f);

    float f[VIB_NUM_FEATURES];
    TEST_ASSERT_TRUE(vib_features_extract(f));

    TEST_ASSERT_FLOAT_WITHIN(0.004f, 0.20f / sqrtf(2.0f), f[FEAT_ACC_RMS]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.40f, f[FEAT_ACC_P2P]);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, sqrtf(2.0f), f[FEAT_ACC_CREST]);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.24f, f[FEAT_ZCR]);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 12.0f, f[FEAT_DOM_FREQ]);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.0f, f[FEAT_BAND_HIGH]);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.0f, f[FEAT_PEAK_CONC]);
}

void test_sine_at_2hz_lands_in_low_band(void)
{
    push_sine(0.25f, 0.15f, 2.0f);

    float f[VIB_NUM_FEATURES];
    TEST_ASSERT_TRUE(vib_features_extract(f));

    TEST_ASSERT_FLOAT_WITHIN(0.5f, 2.0f, f[FEAT_DOM_FREQ]);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.0f, f[FEAT_BAND_LOW]);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, f[FEAT_BAND_HIGH]);
}

void test_constant_signal_is_quiet(void)
{
    push_sine(0.010f, 0.0f, 10.0f);

    float f[VIB_NUM_FEATURES];
    TEST_ASSERT_TRUE(vib_features_extract(f));

    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, f[FEAT_ACC_RMS]);
    TEST_ASSERT_EQUAL_INT(CYCLE_IDLE, cycle_logic_baseline_classify(f));
}

void test_partial_window_refuses_extraction_and_overlaps(void)
{
    imu_sample_t s = { 0 };
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_FALSE(vib_features_push(&s)); /* not full yet */
    }
    float f[VIB_NUM_FEATURES];
    TEST_ASSERT_FALSE(vib_features_extract(f));

    for (int i = 0; i < 99; i++) {
        TEST_ASSERT_FALSE(vib_features_push(&s));
    }
    TEST_ASSERT_TRUE(vib_features_push(&s)); /* the 200th sample fills the ring */
    TEST_ASSERT_TRUE(vib_features_extract(f)); /* full now */

    /* 50 % overlap: pushing 100 more still leaves a full window */
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_TRUE(vib_features_push(&s)); /* ring stays full */
    }
    TEST_ASSERT_TRUE(vib_features_extract(f));
}

/* ---------------------------------------------------------------- baseline */
void test_baseline_boundaries(void)
{
    float f[VIB_NUM_FEATURES];
    memcpy(f, WASH_FEATS, sizeof(f));

    memcpy(f, IDLE_FEATS, sizeof(f));
    f[FEAT_DOM_FREQ] = 15.0f; /* even with spin-like frequency, quiet = IDLE */
    TEST_ASSERT_EQUAL_INT(CYCLE_IDLE, cycle_logic_baseline_classify(f));

    memcpy(f, WASH_FEATS, sizeof(f));
    f[FEAT_DOM_FREQ] = 7.9f; /* below spin frequency -> WASH despite loud rms */
    TEST_ASSERT_EQUAL_INT(CYCLE_WASH, cycle_logic_baseline_classify(f));

    memcpy(f, SPIN_FEATS, sizeof(f));
    TEST_ASSERT_EQUAL_INT(CYCLE_SPIN, cycle_logic_baseline_classify(f));

    memcpy(f, WASH_FEATS, sizeof(f));
    TEST_ASSERT_EQUAL_INT(CYCLE_WASH, cycle_logic_baseline_classify(f));
}

/* ------------------------------------------------------------------- model */
void test_forest_classifies_the_three_signatures(void)
{
    TEST_ASSERT_EQUAL_INT(CYCLE_IDLE, cycle_model_predict(IDLE_FEATS));
    TEST_ASSERT_EQUAL_INT(CYCLE_WASH, cycle_model_predict(WASH_FEATS));
    TEST_ASSERT_EQUAL_INT(CYCLE_SPIN, cycle_model_predict(SPIN_FEATS));
}

void test_forest_rejects_null_safely(void)
{
    TEST_ASSERT_EQUAL_INT(CYCLE_IDLE, cycle_model_predict(NULL));
}

/* ------------------------------------------- state machine: hysteresis+DONE */
void run_window(const float *feats, uint32_t now_ms, cycle_decision_t *out)
{
    cycle_logic_process(feats, now_ms, out);
}

void test_two_windows_required_to_change_state(void)
{
    cycle_decision_t d;
    run_window(WASH_FEATS, 0, &d);
    TEST_ASSERT_EQUAL_INT(CYCLE_STATE_IDLE, d.state); /* single window: hold */
    run_window(WASH_FEATS, 1000, &d);
    TEST_ASSERT_EQUAL_INT(CYCLE_STATE_WASH, d.state); /* confirmed */

    /* One SPIN window is not enough to leave WASH */
    run_window(SPIN_FEATS, 2000, &d);
    TEST_ASSERT_EQUAL_INT(CYCLE_STATE_WASH, d.state);
    run_window(WASH_FEATS, 3000, &d);
    TEST_ASSERT_EQUAL_INT(CYCLE_STATE_WASH, d.state);
}

void test_full_cycle_produces_exactly_one_done_alert(void)
{
    cycle_decision_t d;
    uint32_t t = 0;

    /* WASH for 30 s */
    for (int i = 0; i < 30; i++, t += 1000) {
        run_window(WASH_FEATS, t, &d);
    }
    TEST_ASSERT_EQUAL_INT(CYCLE_STATE_WASH, d.state);

    /* SPIN for 12 s */
    for (int i = 0; i < 12; i++, t += 1000) {
        run_window(SPIN_FEATS, t, &d);
    }
    TEST_ASSERT_EQUAL_INT(CYCLE_STATE_SPIN, d.state);

    /* Machine stops: two quiet windows confirm IDLE -> DONE alert (spin tub) */
    run_window(IDLE_FEATS, t, &d);          t += 1000;
    TEST_ASSERT_FALSE(d.alert_active);      /* hysteresis holds SPIN first */
    run_window(IDLE_FEATS, t, &d);          t += 1000;
    TEST_ASSERT_EQUAL_INT(CYCLE_STATE_DONE, d.state);
    TEST_ASSERT_TRUE(d.alert_active);
    TEST_ASSERT_EQUAL_INT(CYCLE_REASON_SPIN, d.alert_reason);

    /* Alert latches until acknowledged; further windows keep it */
    run_window(IDLE_FEATS, t, &d);
    TEST_ASSERT_TRUE(d.alert_active);

    TEST_ASSERT_TRUE(cycle_logic_acknowledge());
    TEST_ASSERT_EQUAL_INT(CYCLE_STATE_IDLE, cycle_logic_state());
    TEST_ASSERT_FALSE(cycle_logic_is_alerting());
    TEST_ASSERT_FALSE(cycle_logic_acknowledge()); /* nothing left to ack */
}

void test_short_spin_does_not_raise_done(void)
{
    cycle_decision_t d;
    uint32_t t = 0;
    /* WASH then a 2 s SPIN burst then quiet: no DONE (spin < 8 s) */
    for (int i = 0; i < 10; i++, t += 1000) {
        run_window(WASH_FEATS, t, &d);
    }
    for (int i = 0; i < 2; i++, t += 1000) {
        run_window(SPIN_FEATS, t, &d);
    }
    for (int i = 0; i < 3; i++, t += 1000) {
        run_window(IDLE_FEATS, t, &d);
    }
    TEST_ASSERT_EQUAL_INT(CYCLE_STATE_IDLE, d.state);
    TEST_ASSERT_FALSE(d.alert_active);
}

/* Twin-tub semantics: the wash tub stopping is its own DONE event */
void test_wash_tub_stop_raises_wash_done(void)
{
    cycle_decision_t d;
    uint32_t t = 0;
    for (int i = 0; i < 30; i++, t += 1000) {   /* wash tub runs 30 s */
        run_window(WASH_FEATS, t, &d);
    }
    TEST_ASSERT_EQUAL_INT(CYCLE_STATE_WASH, d.state);

    run_window(IDLE_FEATS, t, &d); t += 1000;   /* hysteresis holds WASH */
    TEST_ASSERT_FALSE(d.alert_active);
    run_window(IDLE_FEATS, t, &d); t += 1000;   /* confirmed quiet */
    TEST_ASSERT_EQUAL_INT(CYCLE_STATE_DONE, d.state);
    TEST_ASSERT_TRUE(d.alert_active);
    TEST_ASSERT_EQUAL_INT(CYCLE_REASON_WASH, d.alert_reason);
}

void test_short_wash_burst_does_not_raise_done(void)
{
    cycle_decision_t d;
    uint32_t t = 0;
    for (int i = 0; i < 5; i++, t += 1000) {    /* 5 s wash: under the guard */
        run_window(WASH_FEATS, t, &d);
    }
    TEST_ASSERT_EQUAL_INT(CYCLE_STATE_WASH, d.state);
    for (int i = 0; i < 3; i++, t += 1000) {
        run_window(IDLE_FEATS, t, &d);
    }
    TEST_ASSERT_EQUAL_INT(CYCLE_STATE_IDLE, d.state);
    TEST_ASSERT_FALSE(d.alert_active);
    TEST_ASSERT_EQUAL_INT(CYCLE_REASON_NONE, d.alert_reason);
}

void test_wash_then_spin_session_raises_both_alerts_in_turn(void)
{
    cycle_decision_t d;
    uint32_t t = 0;

    for (int i = 0; i < 20; i++, t += 1000) {   /* wash tub */
        run_window(WASH_FEATS, t, &d);
    }
    for (int i = 0; i < 2; i++, t += 1000) {    /* wash stops */
        run_window(IDLE_FEATS, t, &d);
    }
    TEST_ASSERT_EQUAL_INT(CYCLE_REASON_WASH, d.alert_reason);
    TEST_ASSERT_TRUE(cycle_logic_acknowledge());

    for (int i = 0; i < 2; i++, t += 1000) {    /* leave IDLE (still hysteresis) */
        run_window(IDLE_FEATS, t, &d);
    }
    for (int i = 0; i < 15; i++, t += 1000) {   /* user starts spin tub */
        run_window(SPIN_FEATS, t, &d);
    }
    TEST_ASSERT_EQUAL_INT(CYCLE_STATE_SPIN, d.state);
    for (int i = 0; i < 2; i++, t += 1000) {    /* spin stops */
        run_window(IDLE_FEATS, t, &d);
    }
    TEST_ASSERT_EQUAL_INT(CYCLE_STATE_DONE, d.state);
    TEST_ASSERT_EQUAL_INT(CYCLE_REASON_SPIN, d.alert_reason);
}

void test_unacknowledged_done_times_out_to_idle(void)
{
    cycle_decision_t d;
    uint32_t t = 0;
    for (int i = 0; i < 12; i++, t += 1000) {
        run_window(SPIN_FEATS, t, &d);
    }
    for (int i = 0; i < 2; i++, t += 1000) {
        run_window(IDLE_FEATS, t, &d);
    }
    TEST_ASSERT_EQUAL_INT(CYCLE_STATE_DONE, d.state);

    /* DONE falls back to IDLE after 5 minutes without acknowledgment */
    run_window(IDLE_FEATS, t + 300000, &d);
    TEST_ASSERT_EQUAL_INT(CYCLE_STATE_IDLE, d.state);
    TEST_ASSERT_FALSE(d.alert_active);
}

/* ---------------------------------------------------------- service modes */
void test_service_mode_transitions(void)
{
    TEST_ASSERT_EQUAL_INT(SVC_RUN, service_mode_get());

    service_mode_step(false, false, true);          /* long KEY1 in RUN -> DIAG */
    TEST_ASSERT_EQUAL_INT(SVC_DIAG, service_mode_get());

    service_mode_step(false, true, false);          /* short KEY1 -> COLLECT */
    TEST_ASSERT_EQUAL_INT(SVC_COLLECT, service_mode_get());

    service_mode_step(true, false, false);          /* KEY0 cycles label */
    TEST_ASSERT_EQUAL_INT(1, service_mode_collect_label());
    service_mode_step(true, false, false);
    TEST_ASSERT_EQUAL_INT(2, service_mode_collect_label());
    service_mode_step(true, false, false);
    TEST_ASSERT_EQUAL_INT(0, service_mode_collect_label());

    service_mode_step(false, false, true);          /* long KEY1 -> BENCH */
    TEST_ASSERT_EQUAL_INT(SVC_BENCH, service_mode_get());
    TEST_ASSERT_TRUE(service_mode_is_collecting() == false);

    service_mode_step(false, true, false);          /* short KEY1 -> RUN */
    TEST_ASSERT_EQUAL_INT(SVC_RUN, service_mode_get());
}

void test_service_mode_ack_only_clears_real_alerts(void)
{
    service_mode_step(true, false, false); /* KEY0 with no alert: harmless */
    TEST_ASSERT_EQUAL_INT(SVC_RUN, service_mode_get());
    TEST_ASSERT_FALSE(cycle_logic_is_alerting());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sine_at_12hz_yields_exact_features);
    RUN_TEST(test_sine_at_2hz_lands_in_low_band);
    RUN_TEST(test_constant_signal_is_quiet);
    RUN_TEST(test_partial_window_refuses_extraction_and_overlaps);
    RUN_TEST(test_baseline_boundaries);
    RUN_TEST(test_forest_classifies_the_three_signatures);
    RUN_TEST(test_forest_rejects_null_safely);
    RUN_TEST(test_two_windows_required_to_change_state);
    RUN_TEST(test_full_cycle_produces_exactly_one_done_alert);
    RUN_TEST(test_short_spin_does_not_raise_done);
    RUN_TEST(test_wash_tub_stop_raises_wash_done);
    RUN_TEST(test_short_wash_burst_does_not_raise_done);
    RUN_TEST(test_wash_then_spin_session_raises_both_alerts_in_turn);
    RUN_TEST(test_unacknowledged_done_times_out_to_idle);
    RUN_TEST(test_service_mode_transitions);
    RUN_TEST(test_service_mode_ack_only_clears_real_alerts);
    return UNITY_END();
}
