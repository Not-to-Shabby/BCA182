/**
 * @file test_random_play.c
 * @brief Host tests for the random-song choice and the idle rule.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include <unity.h>
#include <stdint.h>
#include <stdlib.h>
#include "../../src/random_play.c"

void setUp(void) {}
void tearDown(void) {}

void test_an_empty_history_returns_the_random_number_modulo_the_catalog(void)
{
    random_play_t st;
    random_play_reset(&st);
    TEST_ASSERT_EQUAL_UINT32(7, random_play_pick(&st, 100, 7));
    TEST_ASSERT_EQUAL_UINT32(7, random_play_pick(&st, 100, 107));
    TEST_ASSERT_EQUAL_UINT32(99, random_play_pick(&st, 100, 0xFFFFFFFFU - (0xFFFFFFFFU % 100U) + 99U - 100U));
}

void test_an_empty_catalog_gives_index_zero(void)
{
    random_play_t st;
    random_play_reset(&st);
    TEST_ASSERT_EQUAL_UINT32(0, random_play_pick(&st, 0, 12345));
}

void test_the_song_just_played_is_not_picked_again(void)
{
    random_play_t st;
    random_play_reset(&st);
    random_play_note(&st, 42);
    TEST_ASSERT_NOT_EQUAL(42, random_play_pick(&st, 100, 42));
    TEST_ASSERT_EQUAL_UINT32(43, random_play_pick(&st, 100, 42));
}

void test_the_skip_wraps_around_the_end_of_the_catalog(void)
{
    random_play_t st;
    random_play_reset(&st);
    random_play_note(&st, 99);
    TEST_ASSERT_EQUAL_UINT32(0, random_play_pick(&st, 100, 99));
}

void test_the_last_several_songs_are_all_avoided(void)
{
    random_play_t st;
    random_play_reset(&st);
    for (uint32_t i = 10; i < 10 + RANDOM_PLAY_HISTORY; i++) {
        random_play_note(&st, i);
    }
    /* A start inside the recent run lands on the first song after it. */
    TEST_ASSERT_EQUAL_UINT32(10 + RANDOM_PLAY_HISTORY, random_play_pick(&st, 1000, 10));
    TEST_ASSERT_EQUAL_UINT32(10 + RANDOM_PLAY_HISTORY, random_play_pick(&st, 1000, 14));
    /* A start outside it is kept. */
    TEST_ASSERT_EQUAL_UINT32(9, random_play_pick(&st, 1000, 9));
}

void test_old_songs_are_forgotten_once_the_history_is_full(void)
{
    random_play_t st;
    random_play_reset(&st);
    for (uint32_t i = 0; i < RANDOM_PLAY_HISTORY + 1U; i++) {
        random_play_note(&st, 100 + i);
    }
    TEST_ASSERT_EQUAL_UINT32(100, random_play_pick(&st, 1000, 100));    /* the oldest fell out */
    TEST_ASSERT_NOT_EQUAL(101, random_play_pick(&st, 1000, 101));
}

void test_playing_the_same_song_twice_uses_one_slot(void)
{
    random_play_t st;
    random_play_reset(&st);
    random_play_note(&st, 5);
    random_play_note(&st, 5);
    random_play_note(&st, 5);
    TEST_ASSERT_EQUAL_UINT8(1, st.count);
}

void test_a_one_song_catalog_always_returns_that_song(void)
{
    random_play_t st;
    random_play_reset(&st);
    random_play_note(&st, 0);
    for (uint32_t r = 0; r < 20; r++) {
        TEST_ASSERT_EQUAL_UINT32(0, random_play_pick(&st, 1, r));
    }
}

void test_a_small_catalog_still_gets_a_different_song_every_time(void)
{
    /* Three songs: only the last total - 1 = 2 can be skipped, so the pick is never the
     * one just played and never loops. */
    random_play_t st;
    random_play_reset(&st);
    uint32_t last = 0;
    random_play_note(&st, last);
    for (int i = 0; i < 200; i++) {
        uint32_t next = random_play_pick(&st, 3, (uint32_t)rand());
        TEST_ASSERT_TRUE(next < 3);
        TEST_ASSERT_NOT_EQUAL(last, next);
        random_play_note(&st, next);
        last = next;
    }
}

void test_a_catalog_of_two_alternates(void)
{
    random_play_t st;
    random_play_reset(&st);
    random_play_note(&st, 1);
    TEST_ASSERT_EQUAL_UINT32(0, random_play_pick(&st, 2, 1));
    random_play_note(&st, 0);
    TEST_ASSERT_EQUAL_UINT32(1, random_play_pick(&st, 2, 0));
}

void test_every_song_in_a_big_catalog_can_be_reached(void)
{
    random_play_t st;
    random_play_reset(&st);
    static uint8_t seen[64];
    srand(1234);
    for (int i = 0; i < 5000; i++) {
        uint32_t next = random_play_pick(&st, 64, (uint32_t)rand() ^ ((uint32_t)rand() << 15));
        TEST_ASSERT_TRUE(next < 64);
        seen[next] = 1;
        random_play_note(&st, next);
    }
    for (int i = 0; i < 64; i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, seen[i], "a song was never chosen");
    }
}

void test_no_song_comes_back_within_the_history_window(void)
{
    random_play_t st;
    random_play_reset(&st);
    uint32_t played[2000];
    srand(99);
    for (int i = 0; i < 2000; i++) {
        uint32_t next = random_play_pick(&st, 200, (uint32_t)rand() ^ ((uint32_t)rand() << 15));
        for (int k = 1; k <= RANDOM_PLAY_HISTORY && k <= i; k++) {
            TEST_ASSERT_NOT_EQUAL(played[i - k], next);
        }
        played[i] = next;
        random_play_note(&st, next);
    }
}

void test_a_hand_picked_song_is_remembered_too(void)
{
    random_play_t st;
    random_play_reset(&st);
    random_play_note(&st, 500);
    TEST_ASSERT_NOT_EQUAL(500, random_play_pick(&st, 1000, 500));
}

void test_idle_rule_waits_for_both_the_song_to_end_and_fifteen_seconds(void)
{
    TEST_ASSERT_FALSE(random_play_idle_due(100000, 90000, true));              /* 10 s: too soon */
    TEST_ASSERT_FALSE(random_play_idle_due(100000, 100000 - 14999U, true));    /* 14.999 s */
    TEST_ASSERT_TRUE(random_play_idle_due(100000, 100000 - 15000U, true));     /* exactly 15 s */
    TEST_ASSERT_TRUE(random_play_idle_due(100000, 0, true));
    TEST_ASSERT_FALSE(random_play_idle_due(100000, 0, false));                 /* still playing */
}

void test_idle_rule_survives_the_millisecond_counter_wrapping(void)
{
    uint32_t last = 0xFFFFFFFFU - 5000U;
    TEST_ASSERT_FALSE(random_play_idle_due(4000U, last, true));      /* 9 s across the wrap */
    TEST_ASSERT_TRUE(random_play_idle_due(10000U, last, true));      /* 15 s across the wrap */
}

void test_a_button_press_just_now_blocks_the_idle_pick(void)
{
    TEST_ASSERT_FALSE(random_play_idle_due(50000, 50000, true));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_an_empty_history_returns_the_random_number_modulo_the_catalog);
    RUN_TEST(test_an_empty_catalog_gives_index_zero);
    RUN_TEST(test_the_song_just_played_is_not_picked_again);
    RUN_TEST(test_the_skip_wraps_around_the_end_of_the_catalog);
    RUN_TEST(test_the_last_several_songs_are_all_avoided);
    RUN_TEST(test_old_songs_are_forgotten_once_the_history_is_full);
    RUN_TEST(test_playing_the_same_song_twice_uses_one_slot);
    RUN_TEST(test_a_one_song_catalog_always_returns_that_song);
    RUN_TEST(test_a_small_catalog_still_gets_a_different_song_every_time);
    RUN_TEST(test_a_catalog_of_two_alternates);
    RUN_TEST(test_every_song_in_a_big_catalog_can_be_reached);
    RUN_TEST(test_no_song_comes_back_within_the_history_window);
    RUN_TEST(test_a_hand_picked_song_is_remembered_too);
    RUN_TEST(test_idle_rule_waits_for_both_the_song_to_end_and_fifteen_seconds);
    RUN_TEST(test_idle_rule_survives_the_millisecond_counter_wrapping);
    RUN_TEST(test_a_button_press_just_now_blocks_the_idle_pick);
    return UNITY_END();
}
