/**
 * ==============================================================================
 * PlatformIO Unity Test Suite: TinyML Activity Recognition & BLE Protocol
 * ==============================================================================
 */

#include <unity.h>
#include <string.h>
#include <math.h>
#include "activity_model.h"
#include "feature_extractor.h"
#include "ble_protocol.h"

void setUp(void)
{
    /* Set up test environment before each test case */
}

void tearDown(void)
{
    /* Clean up test environment after each test case */
}

/* -------------------------------------------------------------------------- */
/* Test 1: Feature Extractor Window Buffering                                 */
/* -------------------------------------------------------------------------- */
void test_window_buffering(void)
{
    feature_window_t win;
    feature_extractor_init(&win);

    imu_sample_t sample = { 0.1f, -0.8f, 0.2f, 0.01f, -0.02f, 0.05f };
    
    for (int i = 0; i < ACTIVITY_WINDOW_SAMPLES - 1; i++) {
        bool ready = feature_extractor_push(&win, &sample, i * 200);
        TEST_ASSERT_FALSE(ready);
    }

    bool ready = feature_extractor_push(&win, &sample, (ACTIVITY_WINDOW_SAMPLES - 1) * 200);
    TEST_ASSERT_TRUE(ready);
    TEST_ASSERT_EQUAL_UINT8(ACTIVITY_WINDOW_SAMPLES, win.count);
}

/* -------------------------------------------------------------------------- */
/* Test 2: Statistical Feature Computation Accuracy                           */
/* -------------------------------------------------------------------------- */
void test_statistical_computation(void)
{
    feature_window_t win;
    feature_extractor_init(&win);

    imu_sample_t s_const = { 1.0f, 2.0f, 3.0f, 0.1f, 0.2f, 0.3f };
    for (int i = 0; i < ACTIVITY_WINDOW_SAMPLES; i++) {
        feature_extractor_push(&win, &s_const, i * 200);
    }

    float features[ACTIVITY_NUM_FEATURES];
    feature_extractor_compute(&win, features);

    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, features[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f, features[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 3.0f, features[2]);

    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, features[6]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, features[7]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, features[8]);

    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, features[12]);

    float expected_mag = sqrtf(14.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, expected_mag, features[18]);
}

/* -------------------------------------------------------------------------- */
/* Test 3: Activity Classification - Ground-Truth Walking Profile             */
/* -------------------------------------------------------------------------- */
void test_classification_walking(void)
{
    feature_window_t win;
    feature_extractor_init(&win);

    static const imu_sample_t s_real_walking[15] = {
        { 0.2650f, -0.7814f, -0.0076f, -0.0590f,  0.0325f, -2.9296f },
        { 0.6722f, -1.1233f, -0.2344f, -0.1757f,  0.0208f,  0.1269f },
        { 0.4399f, -1.4817f,  0.0722f, -0.9105f,  0.1063f, -2.4367f },
        { 0.3031f, -0.8125f,  0.0888f,  0.1199f, -0.4099f, -2.9336f },
        { 0.4814f, -0.9312f,  0.0359f,  0.0527f,  0.4379f,  2.4922f },
        { 0.4044f, -0.8056f, -0.0956f,  0.6925f, -0.2179f,  2.5750f },
        { 0.6320f, -1.1290f, -0.2982f,  0.0548f, -0.1896f,  0.4473f },
        { 0.6670f, -1.3503f, -0.0880f, -0.8094f, -0.7938f, -1.4348f },
        { 0.2704f, -0.8633f,  0.1293f, -0.4173f, -0.1904f, -2.6759f },
        { 0.4690f, -1.0740f,  0.0219f,  0.0388f,  1.1491f,  1.6982f },
        { 0.2985f, -0.7172f, -0.0693f,  0.2326f,  0.4321f,  2.1009f },
        { 0.6364f, -1.0452f, -0.2400f,  0.1163f, -0.1033f,  1.0822f },
        { 0.5683f, -1.2486f, -0.1310f, -0.4556f, -0.5281f, -1.2407f },
        { 0.2911f, -0.7748f,  0.0163f, -0.2345f, -0.0148f, -2.5884f },
        { 0.4477f, -1.1574f, -0.0172f, -0.1081f,  0.4016f,  0.6700f }
    };

    for (int i = 0; i < ACTIVITY_WINDOW_SAMPLES; i++) {
        feature_extractor_push(&win, &s_real_walking[i], i * 200);
    }

    float features[ACTIVITY_NUM_FEATURES];
    feature_extractor_compute(&win, features);

    uint8_t confidence = 0;
    activity_type_t act = tinyml_predict_activity(features, &confidence);

    TEST_ASSERT_EQUAL_INT(ACTIVITY_WALKING, act);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT8(80, confidence);
}

/* -------------------------------------------------------------------------- */
/* Test 4: Activity Classification - Ground-Truth Running Profile             */
/* -------------------------------------------------------------------------- */
void test_classification_running(void)
{
    feature_window_t win;
    feature_extractor_init(&win);

    static const imu_sample_t s_real_running[15] = {
        {  1.2842f,  0.0526f, -0.2362f, -1.9543f,  2.4977f,  1.3038f },
        {  0.3057f,  0.4504f, -0.1000f,  1.4730f, -0.5391f, -2.6030f },
        {  1.0601f, -0.9857f, -0.0732f,  1.4300f, -0.9917f, -2.3499f },
        { -0.1065f, -0.7203f,  0.2003f, -0.4013f,  0.4240f,  3.0591f },
        {  1.0069f,  0.3441f, -0.2760f, -1.6086f,  2.3222f,  1.4736f },
        {  0.0997f,  0.4844f, -0.0588f,  1.0809f, -0.6441f, -2.7383f },
        {  1.5466f, -1.1082f, -0.2509f,  0.8609f, -0.4994f, -1.6458f },
        {  0.1127f, -0.6124f, -0.0212f, -0.4119f,  0.0368f,  2.2783f },
        {  1.3669f,  0.1981f, -0.2247f, -1.9224f,  1.7346f,  1.8756f },
        {  0.1656f,  0.5072f, -0.0166f,  1.3623f, -0.5861f, -2.7333f },
        {  0.9711f, -0.9115f, -0.1347f,  1.5916f,  0.5062f, -1.6844f },
        {  0.3394f, -0.7747f,  0.0451f, -1.2522f, -0.4826f,  2.4021f },
        {  0.9977f,  0.4357f, -0.1830f, -1.1841f,  1.9555f,  1.1010f },
        {  0.3497f,  0.3292f, -0.1137f,  1.5133f,  0.1762f, -2.1542f },
        {  0.8550f, -0.7477f,  0.0615f,  1.9114f, -1.2625f, -1.7883f }
    };

    for (int i = 0; i < ACTIVITY_WINDOW_SAMPLES; i++) {
        feature_extractor_push(&win, &s_real_running[i], i * 180);
    }

    float features[ACTIVITY_NUM_FEATURES];
    feature_extractor_compute(&win, features);

    uint8_t confidence = 0;
    activity_type_t act = tinyml_predict_activity(features, &confidence);

    TEST_ASSERT_EQUAL_INT(ACTIVITY_RUNNING, act);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT8(80, confidence);
}

/* -------------------------------------------------------------------------- */
/* Test 5: Activity Classification - Stationary Profile (STD Gating)          */
/* -------------------------------------------------------------------------- */
void test_classification_stationary(void)
{
    feature_window_t win;
    feature_extractor_init(&win);

    /* Stationary has minimal variance (only MEMS thermal noise < 0.02g) */
    for (int i = 0; i < ACTIVITY_WINDOW_SAMPLES; i++) {
        float noise = ((float)(i % 5) - 2.0f) * 0.002f;
        imu_sample_t s = {
            0.02f + noise,
            -0.01f - noise,
            0.99f + noise,
            0.001f,
            -0.001f,
            0.001f
        };
        feature_extractor_push(&win, &s, i * 200);
    }

    float features[ACTIVITY_NUM_FEATURES];
    feature_extractor_compute(&win, features);

    uint8_t confidence = 0;
    activity_type_t act = tinyml_predict_activity(features, &confidence);

    TEST_ASSERT_EQUAL_INT(ACTIVITY_STATIONARY, act);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT8(80, confidence);
}

/* -------------------------------------------------------------------------- */
/* Test 6: Pedometer Cadence & Step Count Detection                           */
/* -------------------------------------------------------------------------- */
void test_pedometer_step_counting(void)
{
    feature_window_t win;
    feature_extractor_init(&win);

    /* Simulate 5 steps at 500 ms interval (120 spm) */
    uint32_t t = 1000;
    for (int step = 0; step < 5; step++) {
        imu_sample_t s_idle = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f };
        feature_extractor_push(&win, &s_idle, t);
        t += 200;

        imu_sample_t s_peak = { 0.8f, 0.8f, 1.2f, 0.0f, 0.0f, 0.0f };
        feature_extractor_push(&win, &s_peak, t);
        t += 300;
    }

    TEST_ASSERT_EQUAL_UINT32(5, win.total_steps);
    TEST_ASSERT_UINT8_WITHIN(15, 120, win.current_cadence_spm);
}

/* -------------------------------------------------------------------------- */
/* Test 6: BLE Protocol Serialization & Checksum Verification                 */
/* -------------------------------------------------------------------------- */
void test_ble_protocol_packaging(void)
{
    ble_activity_packet_t pkt;
    
    ble_protocol_pack(&pkt, ACTIVITY_RUNNING, 99, 168, 1250, 2.45f);

    TEST_ASSERT_EQUAL_HEX8(BLE_PACKET_MAGIC, pkt.magic);
    TEST_ASSERT_EQUAL_UINT8(ACTIVITY_RUNNING, pkt.activity);
    TEST_ASSERT_EQUAL_UINT8(99, pkt.confidence);
    TEST_ASSERT_EQUAL_UINT8(168, pkt.cadence_spm);
    TEST_ASSERT_EQUAL_UINT32(1250, pkt.step_count);
    TEST_ASSERT_EQUAL_UINT16(245, pkt.accel_mag_x100);
    
    uint8_t cks = ble_protocol_compute_checksum((const uint8_t *)&pkt, BLE_PACKET_LENGTH - 1);
    TEST_ASSERT_EQUAL_HEX8(cks, pkt.checksum);
}

/* -------------------------------------------------------------------------- */
/* Unity Main Entry Point                                                     */
/* -------------------------------------------------------------------------- */
int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_window_buffering);
    RUN_TEST(test_statistical_computation);
    RUN_TEST(test_classification_walking);
    RUN_TEST(test_classification_running);
    RUN_TEST(test_classification_stationary);
    RUN_TEST(test_pedometer_step_counting);
    RUN_TEST(test_ble_protocol_packaging);

    return UNITY_END();
}
