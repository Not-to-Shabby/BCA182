/**
 * @file test_midi_fm.c
 * @brief Automated host-based unit tests for MIDI Sequencer & FM Synthesis Logic.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include <unity.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* Minimal Mock Data for Testing */
static const uint8_t SAMPLE_SMF_FORMAT0[] = {
    'M', 'T', 'h', 'd', 0x00, 0x00, 0x00, 0x06,
    0x00, 0x00, /* Format 0 */
    0x00, 0x01, /* 1 Track */
    0x00, 0x60, /* 96 PPQN */
    'M', 'T', 'r', 'k', 0x00, 0x00, 0x00, 0x22,
    0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20, /* Tempo = 500,000 us (120 BPM) */
    0x00, 0xFF, 0x05, 0x05, 'H', 'e', 'l', 'l', 'o', /* Lyric: "Hello" */
    0x10, 0x90, 0x3C, 0x64, /* Note On: Middle C, Vel 100 at tick 16 */
    0x20, 0x80, 0x3C, 0x00, /* Note Off: Middle C at tick 48 */
    0x00, 0xFF, 0x05, 0x01, '\n', /* Lyric: Newline */
    0x00, 0xFF, 0x2F, 0x00  /* End of Track */
};

/* Test 1: Verify SMF Header validation */
void test_smf_header_validation(void)
{
    TEST_ASSERT_EQUAL_UINT8('M', SAMPLE_SMF_FORMAT0[0]);
    TEST_ASSERT_EQUAL_UINT8('T', SAMPLE_SMF_FORMAT0[1]);
    TEST_ASSERT_EQUAL_UINT8('h', SAMPLE_SMF_FORMAT0[2]);
    TEST_ASSERT_EQUAL_UINT8('d', SAMPLE_SMF_FORMAT0[3]);

    uint16_t fmt = (SAMPLE_SMF_FORMAT0[8] << 8) | SAMPLE_SMF_FORMAT0[9];
    uint16_t tracks = (SAMPLE_SMF_FORMAT0[10] << 8) | SAMPLE_SMF_FORMAT0[11];
    uint16_t ppqn = (SAMPLE_SMF_FORMAT0[12] << 8) | SAMPLE_SMF_FORMAT0[13];

    TEST_ASSERT_EQUAL_UINT16(0, fmt);
    TEST_ASSERT_EQUAL_UINT16(1, tracks);
    TEST_ASSERT_EQUAL_UINT16(96, ppqn);
}

/* Test 2: Verify Variable Length Quantity (VLQ) Decoding */
static uint32_t decode_vlq(const uint8_t *bytes, size_t *pos)
{
    uint32_t val = 0;
    while (1) {
        uint8_t b = bytes[(*pos)++];
        val = (val << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return val;
}

void test_vlq_decoding(void)
{
    const uint8_t seq1[] = {0x00};
    size_t pos = 0;
    TEST_ASSERT_EQUAL_UINT32(0, decode_vlq(seq1, &pos));

    const uint8_t seq2[] = {0x7F};
    pos = 0;
    TEST_ASSERT_EQUAL_UINT32(127, decode_vlq(seq2, &pos));

    const uint8_t seq3[] = {0x81, 0x00};
    pos = 0;
    TEST_ASSERT_EQUAL_UINT32(128, decode_vlq(seq3, &pos));

    const uint8_t seq4[] = {0xC0, 0x00};
    pos = 0;
    TEST_ASSERT_EQUAL_UINT32(8192, decode_vlq(seq4, &pos));
}

/* Test 3: Verify Tempo & Microseconds per Tick Calculation */
void test_tempo_and_bpm_calculation(void)
{
    uint32_t tempo_us = 500000; /* 120 BPM */
    uint16_t ppqn = 120;

    uint32_t bpm = 60000000UL / tempo_us;
    TEST_ASSERT_EQUAL_UINT32(120, bpm);

    uint32_t us_per_tick = tempo_us / ppqn;
    TEST_ASSERT_EQUAL_UINT32(4166, us_per_tick);
}

/* Test 4: Verify FM Synthesizer 2-Operator Math */
void test_fm_synthesis_operator_math(void)
{
    int16_t sine_table[256];
    for (int i = 0; i < 256; i++) {
        sine_table[i] = (int16_t)(sin(2.0 * 3.14159265 * i / 256.0) * 32767.0);
    }

    /* Modulator phase & sample */
    uint32_t phase_mod = 0;
    int16_t mod_sample = sine_table[(phase_mod >> 24) & 255];
    TEST_ASSERT_EQUAL_INT16(0, mod_sample);

    /* Carrier with modulation */
    int32_t mod_index = 2800;
    int32_t deviation = ((int32_t)mod_sample * mod_index) >> 1;
    uint32_t phase_car = (64U << 24); /* 90 degrees = peak */
    uint32_t modulated_phase = phase_car + (uint32_t)deviation;
    int16_t car_sample = sine_table[(modulated_phase >> 24) & 255];

    /* At 90 degrees sine is at peak (~32767) */
    TEST_ASSERT_INT_WITHIN(500, 32767, car_sample);
}

/* Test 5: Verify Soft Clipping Limiter */
void test_audio_soft_clipping(void)
{
    int32_t over_val = 45000;
    int32_t under_val = -50000;
    int32_t normal_val = 12000;

    if (over_val > 32767) over_val = 32767;
    if (under_val < -32768) under_val = -32768;

    TEST_ASSERT_EQUAL_INT32(32767, over_val);
    TEST_ASSERT_EQUAL_INT32(-32768, under_val);
    TEST_ASSERT_EQUAL_INT32(12000, normal_val);
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_smf_header_validation);
    RUN_TEST(test_vlq_decoding);
    RUN_TEST(test_tempo_and_bpm_calculation);
    RUN_TEST(test_fm_synthesis_operator_math);
    RUN_TEST(test_audio_soft_clipping);
    return UNITY_END();
}
