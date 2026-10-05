/**
 * @file test_player_logic.c
 * @brief Automated host-native unit test suite for the Personal MP3 Player decision logic.
 *
 * Course: BCA182 Embedded Systems Programming
 * Laboratory Activity 2: Personal MP3 Player
 * Target: Host PC MinGW Native via PlatformIO Unity
 *
 * Test Categories:
 * 1. Binary Button Song Selection Decoding (8 tests)
 * 2. 5-Second Confirmation Window Timeout Logic (4 tests)
 * 3. Player Operational State Transitions (5 tests)
 * 4. Potentiometer Volume Normalization & Bounds (5 tests)
 * 5. Song Catalog Lookup & Boundary Wrap-Around (3 tests)
 * Total: 25 Test Cases
 */

#include <unity.h>
#include "player_logic.h"

void setUp(void)
{
    /* Set up resources before each test if needed */
}

void tearDown(void)
{
    /* Clean up resources after each test if needed */
}

/* ========================================================================== */
/* Category 1: Binary Button Song Selection Decoding (8 tests)                */
/* Formula: index = (b2 << 0) | (b3 << 1) | (b4 << 2)                         */
/* ========================================================================== */

void test_decode_all_buttons_released_is_song_0(void)
{
    /* 000b = 0: Fur Elise (Beethoven) */
    TEST_ASSERT_EQUAL_UINT8(0, decode_binary_song_index(false, false, false));
}

void test_decode_button2_only_is_song_1(void)
{
    /* 001b = 1: Canon in D (Pachelbel) */
    TEST_ASSERT_EQUAL_UINT8(1, decode_binary_song_index(true, false, false));
}

void test_decode_button3_only_is_song_2(void)
{
    /* 010b = 2: Minuet in G major (Bach) */
    TEST_ASSERT_EQUAL_UINT8(2, decode_binary_song_index(false, true, false));
}

void test_decode_button2_and_button3_is_song_3(void)
{
    /* 011b = 3: Turkish March (Mozart) */
    TEST_ASSERT_EQUAL_UINT8(3, decode_binary_song_index(true, true, false));
}

void test_decode_button4_only_is_song_4(void)
{
    /* 100b = 4: Nocturne in E flat (Chopin) */
    TEST_ASSERT_EQUAL_UINT8(4, decode_binary_song_index(false, false, true));
}

void test_decode_button4_and_button2_is_song_5(void)
{
    /* 101b = 5: Waltz No. 2 (Shostakovich) */
    TEST_ASSERT_EQUAL_UINT8(5, decode_binary_song_index(true, false, true));
}

void test_decode_button4_and_button3_is_song_6(void)
{
    /* 110b = 6: Nocturne in C sharp (Chopin) */
    TEST_ASSERT_EQUAL_UINT8(6, decode_binary_song_index(false, true, true));
}

void test_decode_all_buttons_pressed_is_song_7(void)
{
    /* 111b = 7: Symphony No. 40 (Mozart) */
    TEST_ASSERT_EQUAL_UINT8(7, decode_binary_song_index(true, true, true));
}

/* ========================================================================== */
/* Category 2: 5-Second Confirmation Window Timeout Logic (4 tests)           */
/* ========================================================================== */

void test_confirmation_within_5_seconds_does_not_timeout(void)
{
    /* 4999 ms elapsed with 5000 ms timeout -> false (still active) */
    TEST_ASSERT_FALSE(check_confirmation_timeout(10000, 14999, 5000));
}

void test_confirmation_at_exact_5_seconds_expires(void)
{
    /* Exactly 5000 ms elapsed -> true (expired) */
    TEST_ASSERT_TRUE(check_confirmation_timeout(10000, 15000, 5000));
}

void test_confirmation_past_5_seconds_expires(void)
{
    /* 5001 ms elapsed -> true (expired) */
    TEST_ASSERT_TRUE(check_confirmation_timeout(10000, 15001, 5000));
}

void test_confirmation_timeout_handles_32bit_rollover(void)
{
    /* System tick rolls over past 0xFFFFFFFF */
    uint32_t start_time = 0xFFFFFF00U;
    uint32_t current_time = 0x00001388U; /* 5000 ms after rollover */
    TEST_ASSERT_TRUE(check_confirmation_timeout(start_time, current_time, 5000));
}

/* ========================================================================== */
/* Category 3: Player Operational State Transitions (5 tests)                 */
/* ========================================================================== */

void test_toggle_play_pause_from_stopped_starts_playing(void)
{
    TEST_ASSERT_EQUAL(PLAYER_STATE_PLAYING, toggle_play_pause(PLAYER_STATE_STOPPED));
}

void test_toggle_play_pause_from_playing_pauses(void)
{
    TEST_ASSERT_EQUAL(PLAYER_STATE_PAUSED, toggle_play_pause(PLAYER_STATE_PLAYING));
}

void test_toggle_play_pause_from_paused_resumes_playing(void)
{
    TEST_ASSERT_EQUAL(PLAYER_STATE_PLAYING, toggle_play_pause(PLAYER_STATE_PAUSED));
}

void test_toggle_play_pause_while_confirming_remains_confirming(void)
{
    TEST_ASSERT_EQUAL(PLAYER_STATE_CONFIRMING, toggle_play_pause(PLAYER_STATE_CONFIRMING));
}

void test_player_state_strings_are_valid(void)
{
    TEST_ASSERT_EQUAL_STRING("PLAYING", get_player_state_str(PLAYER_STATE_PLAYING));
    TEST_ASSERT_EQUAL_STRING("PAUSED", get_player_state_str(PLAYER_STATE_PAUSED));
    TEST_ASSERT_EQUAL_STRING("STOPPED", get_player_state_str(PLAYER_STATE_STOPPED));
    TEST_ASSERT_EQUAL_STRING("CONFIRMING", get_player_state_str(PLAYER_STATE_CONFIRMING));
}

/* ========================================================================== */
/* Category 4: Potentiometer Volume Normalization & Bounds (5 tests)          */
/* ========================================================================== */

void test_normalize_volume_at_min_is_zero_percent(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, normalize_adc_volume(0, 0, 4095));
}

void test_normalize_volume_below_min_clamps_to_zero(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, normalize_adc_volume(50, 100, 4095));
}

void test_normalize_volume_at_max_is_one_hundred_percent(void)
{
    TEST_ASSERT_EQUAL_UINT8(100, normalize_adc_volume(4095, 0, 4095));
}

void test_normalize_volume_above_max_clamps_to_one_hundred(void)
{
    TEST_ASSERT_EQUAL_UINT8(100, normalize_adc_volume(4200, 0, 4095));
}

void test_normalize_volume_midpoint_is_fifty_percent(void)
{
    /* 2048 of 4095 is ~50% */
    uint8_t vol = normalize_adc_volume(2048, 0, 4095);
    TEST_ASSERT_UINT8_WITHIN(1, 50, vol);
}

/* ========================================================================== */
/* Category 5: Song Catalog Lookup & Boundary Wrap-Around (3 tests)           */
/* ========================================================================== */

void test_get_song_info_returns_correct_title(void)
{
    const song_info_t *song = get_song_info(0);
    TEST_ASSERT_NOT_NULL(song);
    TEST_ASSERT_EQUAL_UINT8(0, song->id);
    TEST_ASSERT_EQUAL_STRING("Fur Elise -", song->name1);
    TEST_ASSERT_EQUAL_STRING("Beethoven", song->name2);
}

void test_get_song_info_returns_last_song_correctly(void)
{
    const song_info_t *song = get_song_info(7);
    TEST_ASSERT_NOT_NULL(song);
    TEST_ASSERT_EQUAL_UINT8(7, song->id);
    TEST_ASSERT_EQUAL_STRING("Symphony No. 40 ", song->name1);
    TEST_ASSERT_EQUAL_STRING("- Mozart", song->name2);
}

void test_get_song_info_out_of_bounds_wraps_to_song_0(void)
{
    const song_info_t *song = get_song_info(8);
    TEST_ASSERT_NOT_NULL(song);
    TEST_ASSERT_EQUAL_UINT8(0, song->id);
    TEST_ASSERT_EQUAL_STRING("Fur Elise -", song->name1);
}

/* ========================================================================== */
/* Unity Test Runner Entry Point                                              */
/* ========================================================================== */

int main(void)
{
    UNITY_BEGIN();

    /* Category 1: Binary Button Song Decoding */
    RUN_TEST(test_decode_all_buttons_released_is_song_0);
    RUN_TEST(test_decode_button2_only_is_song_1);
    RUN_TEST(test_decode_button3_only_is_song_2);
    RUN_TEST(test_decode_button2_and_button3_is_song_3);
    RUN_TEST(test_decode_button4_only_is_song_4);
    RUN_TEST(test_decode_button4_and_button2_is_song_5);
    RUN_TEST(test_decode_button4_and_button3_is_song_6);
    RUN_TEST(test_decode_all_buttons_pressed_is_song_7);

    /* Category 2: 5-Second Confirmation Window Timeout */
    RUN_TEST(test_confirmation_within_5_seconds_does_not_timeout);
    RUN_TEST(test_confirmation_at_exact_5_seconds_expires);
    RUN_TEST(test_confirmation_past_5_seconds_expires);
    RUN_TEST(test_confirmation_timeout_handles_32bit_rollover);

    /* Category 3: Player State Transitions */
    RUN_TEST(test_toggle_play_pause_from_stopped_starts_playing);
    RUN_TEST(test_toggle_play_pause_from_playing_pauses);
    RUN_TEST(test_toggle_play_pause_from_paused_resumes_playing);
    RUN_TEST(test_toggle_play_pause_while_confirming_remains_confirming);
    RUN_TEST(test_player_state_strings_are_valid);

    /* Category 4: Potentiometer Volume Normalization */
    RUN_TEST(test_normalize_volume_at_min_is_zero_percent);
    RUN_TEST(test_normalize_volume_below_min_clamps_to_zero);
    RUN_TEST(test_normalize_volume_at_max_is_one_hundred_percent);
    RUN_TEST(test_normalize_volume_above_max_clamps_to_one_hundred);
    RUN_TEST(test_normalize_volume_midpoint_is_fifty_percent);

    /* Category 5: Song Catalog Lookup & Boundary Wrap */
    RUN_TEST(test_get_song_info_returns_correct_title);
    RUN_TEST(test_get_song_info_returns_last_song_correctly);
    RUN_TEST(test_get_song_info_out_of_bounds_wraps_to_song_0);

    return UNITY_END();
}
