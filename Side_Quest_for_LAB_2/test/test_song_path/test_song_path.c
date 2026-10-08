/**
 * @file test_song_path.c
 * @brief Host tests for the SD card MIDI path rule shared with prepare_karaoke_sd.py.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "../../src/song_path.c"

void setUp(void) {}
void tearDown(void) {}

void test_sharded_path_uses_thousands_folder(void)
{
    char path[64];
    TEST_ASSERT_TRUE(song_midi_path(path, sizeof(path), 36527, "036527.mid", false));
    TEST_ASSERT_EQUAL_STRING("SD:/midi/036/036527.mid", path);
}

void test_small_code_goes_to_folder_000(void)
{
    char path[64];
    TEST_ASSERT_TRUE(song_midi_path(path, sizeof(path), 1, "000001.mid", false));
    TEST_ASSERT_EQUAL_STRING("SD:/midi/000/000001.mid", path);
}

void test_folder_boundary_999_and_1000(void)
{
    char path[64];
    TEST_ASSERT_TRUE(song_midi_path(path, sizeof(path), 999, "000999.mid", false));
    TEST_ASSERT_EQUAL_STRING("SD:/midi/000/000999.mid", path);
    TEST_ASSERT_TRUE(song_midi_path(path, sizeof(path), 1000, "001000.mid", false));
    TEST_ASSERT_EQUAL_STRING("SD:/midi/001/001000.mid", path);
}

void test_highest_known_code(void)
{
    char path[64];
    TEST_ASSERT_TRUE(song_midi_path(path, sizeof(path), 91718, "091718.mid", false));
    TEST_ASSERT_EQUAL_STRING("SD:/midi/091/091718.mid", path);
}

void test_flat_path_has_no_subfolder(void)
{
    char path[64];
    TEST_ASSERT_TRUE(song_midi_path(path, sizeof(path), 36527, "036527.mid", true));
    TEST_ASSERT_EQUAL_STRING("SD:/midi/036527.mid", path);
}

void test_too_small_buffer_is_rejected(void)
{
    char path[16];
    TEST_ASSERT_FALSE(song_midi_path(path, sizeof(path), 36527, "036527.mid", false));
    TEST_ASSERT_FALSE(song_midi_path(path, sizeof(path), 36527, "036527.mid", true));
}

void test_buffer_exactly_fitting_is_accepted(void)
{
    char path[24];
    TEST_ASSERT_TRUE(song_midi_path(path, sizeof(path), 36527, "036527.mid", false));
    TEST_ASSERT_EQUAL_UINT(23, strlen(path));
    char tight[23];
    TEST_ASSERT_FALSE(song_midi_path(tight, sizeof(tight), 36527, "036527.mid", false));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sharded_path_uses_thousands_folder);
    RUN_TEST(test_small_code_goes_to_folder_000);
    RUN_TEST(test_folder_boundary_999_and_1000);
    RUN_TEST(test_highest_known_code);
    RUN_TEST(test_flat_path_has_no_subfolder);
    RUN_TEST(test_too_small_buffer_is_rejected);
    RUN_TEST(test_buffer_exactly_fitting_is_accepted);
    return UNITY_END();
}
