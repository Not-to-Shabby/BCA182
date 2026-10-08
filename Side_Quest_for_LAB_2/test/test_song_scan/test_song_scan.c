/**
 * @file test_song_scan.c
 * @brief Host tests for the SD card scan helpers (file name checks, parsing, path list).
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "../../src/song_scan.c"

static uint32_t code;
static char singer[28];
static char title[40];

void setUp(void) {}
void tearDown(void) {}

static void parse(const char *name)
{
    memset(singer, 'x', sizeof(singer));
    memset(title, 'x', sizeof(title));
    song_name_parse(name, &code, singer, sizeof(singer), title, sizeof(title));
}

void test_midi_names_are_accepted_in_any_case(void)
{
    TEST_ASSERT_TRUE(song_name_is_midi("036527.mid"));
    TEST_ASSERT_TRUE(song_name_is_midi("SONG.MID"));
    TEST_ASSERT_TRUE(song_name_is_midi("Song.Midi"));
    TEST_ASSERT_TRUE(song_name_is_midi("027720 - Itchyworms - Beer.mid"));
}

void test_other_files_and_hidden_files_are_rejected(void)
{
    TEST_ASSERT_FALSE(song_name_is_midi("readme.txt"));
    TEST_ASSERT_FALSE(song_name_is_midi("song.mp3"));
    TEST_ASSERT_FALSE(song_name_is_midi("midi"));
    TEST_ASSERT_FALSE(song_name_is_midi("song.mid.bak"));
    TEST_ASSERT_FALSE(song_name_is_midi("._036527.mid"));
    TEST_ASSERT_FALSE(song_name_is_midi(".mid"));
    TEST_ASSERT_FALSE(song_name_is_midi(""));
    TEST_ASSERT_FALSE(song_name_is_midi(NULL));
}

void test_full_library_name_is_split_into_code_artist_title(void)
{
    parse("027720 - Itchyworms - Beer.mid");
    TEST_ASSERT_EQUAL_UINT32(27720, code);
    TEST_ASSERT_EQUAL_STRING("Itchyworms", singer);
    TEST_ASSERT_EQUAL_STRING("Beer", title);
}

void test_title_may_contain_the_separator(void)
{
    parse("012345 - Some Band - Song - Live Version.mid");
    TEST_ASSERT_EQUAL_UINT32(12345, code);
    TEST_ASSERT_EQUAL_STRING("Some Band", singer);
    TEST_ASSERT_EQUAL_STRING("Song - Live Version", title);
}

void test_code_and_title_without_artist(void)
{
    parse("000042 - Only A Title.mid");
    TEST_ASSERT_EQUAL_UINT32(42, code);
    TEST_ASSERT_EQUAL_STRING("", singer);
    TEST_ASSERT_EQUAL_STRING("Only A Title", title);
}

void test_bare_code_name_from_the_prepare_tool(void)
{
    parse("036527.mid");
    TEST_ASSERT_EQUAL_UINT32(36527, code);
    TEST_ASSERT_EQUAL_STRING("", singer);
    TEST_ASSERT_EQUAL_STRING("036527", title);
}

void test_plain_name_has_no_code(void)
{
    parse("My Song.MID");
    TEST_ASSERT_EQUAL_UINT32(0, code);
    TEST_ASSERT_EQUAL_STRING("", singer);
    TEST_ASSERT_EQUAL_STRING("My Song", title);
}

void test_digits_not_followed_by_separator_are_not_a_code(void)
{
    parse("2Pac - Changes.mid");
    TEST_ASSERT_EQUAL_UINT32(0, code);
    TEST_ASSERT_EQUAL_STRING("2Pac - Changes", title);

    parse("1234567890.mid");
    TEST_ASSERT_EQUAL_UINT32(0, code);
    TEST_ASSERT_EQUAL_STRING("1234567890", title);
}

void test_empty_title_falls_back_to_the_file_name(void)
{
    parse("027720 - Artist - .mid");
    TEST_ASSERT_EQUAL_UINT32(27720, code);
    TEST_ASSERT_EQUAL_STRING("027720 - Artist - ", title);
}

void test_long_text_is_cut_to_the_buffers(void)
{
    parse("000001 - ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCDEF - ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCDEFGHIJKLMNOP.mid");
    TEST_ASSERT_EQUAL_UINT32(1, code);
    TEST_ASSERT_EQUAL_UINT(27, strlen(singer));
    TEST_ASSERT_EQUAL_UINT(39, strlen(title));
    TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNOPQRSTUVWXYZ0", singer);
}

void test_table_stores_and_returns_paths_in_order(void)
{
    static song_table_t table;
    song_table_clear(&table);
    TEST_ASSERT_TRUE(song_table_add(&table, "midi/036/036527.mid"));
    TEST_ASSERT_TRUE(song_table_add(&table, "midi/074/074531.mid"));
    TEST_ASSERT_TRUE(song_table_add(&table, "loose.mid"));
    TEST_ASSERT_EQUAL_UINT16(3, table.count);
    TEST_ASSERT_EQUAL_STRING("midi/036/036527.mid", song_table_get(&table, 0));
    TEST_ASSERT_EQUAL_STRING("midi/074/074531.mid", song_table_get(&table, 1));
    TEST_ASSERT_EQUAL_STRING("loose.mid", song_table_get(&table, 2));
    TEST_ASSERT_NULL(song_table_get(&table, 3));
}

void test_table_stops_at_the_song_limit(void)
{
    static song_table_t table;
    song_table_clear(&table);
    for (int i = 0; i < SONG_TABLE_MAX_SONGS; i++) {
        TEST_ASSERT_TRUE(song_table_add(&table, "a.mid"));
    }
    TEST_ASSERT_FALSE(song_table_add(&table, "a.mid"));
    TEST_ASSERT_EQUAL_UINT16(SONG_TABLE_MAX_SONGS, table.count);
}

void test_table_stops_when_the_pool_is_full_and_stays_intact(void)
{
    static song_table_t table;
    char path[128];
    song_table_clear(&table);
    memset(path, 'p', sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    int added = 0;
    while (song_table_add(&table, path)) {
        added++;
    }
    TEST_ASSERT_EQUAL_INT(SONG_TABLE_POOL_BYTES / 128, added);
    TEST_ASSERT_LESS_THAN(SONG_TABLE_MAX_SONGS, added);
    TEST_ASSERT_EQUAL_STRING(path, song_table_get(&table, 0));
    TEST_ASSERT_EQUAL_STRING(path, song_table_get(&table, (uint16_t)(added - 1)));
    TEST_ASSERT_FALSE(song_table_add(&table, ""));
}

void test_clear_empties_the_table(void)
{
    static song_table_t table;
    song_table_clear(&table);
    TEST_ASSERT_TRUE(song_table_add(&table, "x.mid"));
    song_table_clear(&table);
    TEST_ASSERT_EQUAL_UINT16(0, table.count);
    TEST_ASSERT_NULL(song_table_get(&table, 0));
    TEST_ASSERT_TRUE(song_table_add(&table, "y.mid"));
    TEST_ASSERT_EQUAL_STRING("y.mid", song_table_get(&table, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_midi_names_are_accepted_in_any_case);
    RUN_TEST(test_other_files_and_hidden_files_are_rejected);
    RUN_TEST(test_full_library_name_is_split_into_code_artist_title);
    RUN_TEST(test_title_may_contain_the_separator);
    RUN_TEST(test_code_and_title_without_artist);
    RUN_TEST(test_bare_code_name_from_the_prepare_tool);
    RUN_TEST(test_plain_name_has_no_code);
    RUN_TEST(test_digits_not_followed_by_separator_are_not_a_code);
    RUN_TEST(test_empty_title_falls_back_to_the_file_name);
    RUN_TEST(test_long_text_is_cut_to_the_buffers);
    RUN_TEST(test_table_stores_and_returns_paths_in_order);
    RUN_TEST(test_table_stops_at_the_song_limit);
    RUN_TEST(test_table_stops_when_the_pool_is_full_and_stays_intact);
    RUN_TEST(test_clear_empties_the_table);
    return UNITY_END();
}
