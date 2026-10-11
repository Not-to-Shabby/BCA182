/**
 * @file test_mix.c
 * @brief Host tests for the mixer data model: row ranges, stepping, mute/solo cycle and the
 *        text form saved on the SD card.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include <unity.h>
#include <string.h>
#include "../../src/karaoke_mix.c"

void setUp(void) {}
void tearDown(void) {}

void test_defaults_leave_every_row_unchanged(void)
{
    karaoke_mix_t m;
    karaoke_mix_defaults(&m);
    for (uint8_t i = 0; i < MIX_ITEM_COUNT; i++) {
        TEST_ASSERT_FALSE(karaoke_mix_item_changed(&m, i));
    }
    TEST_ASSERT_EQUAL_INT(100, karaoke_mix_item_value(&m, 0));
    TEST_ASSERT_EQUAL_INT(0, karaoke_mix_item_value(&m, MIX_ITEM_BASS));
}

void test_row_numbers_match_the_screen_layout(void)
{
    TEST_ASSERT_EQUAL_UINT(15, MIX_ITEM_REVERB);
    TEST_ASSERT_EQUAL_UINT(19, MIX_ITEM_CHANNEL0);
    TEST_ASSERT_EQUAL_UINT(35, MIX_ITEM_RESET);
    TEST_ASSERT_EQUAL_UINT(37, MIX_ITEM_DONE);
    TEST_ASSERT_EQUAL_UINT(38, MIX_ITEM_COUNT);
    TEST_ASSERT_EQUAL(MIX_KIND_SECTION, karaoke_mix_item_kind(14));
    TEST_ASSERT_EQUAL(MIX_KIND_EFFECT, karaoke_mix_item_kind(MIX_ITEM_CHORUS));
    TEST_ASSERT_EQUAL(MIX_KIND_TONE, karaoke_mix_item_kind(MIX_ITEM_TREBLE));
    TEST_ASSERT_EQUAL(MIX_KIND_CHANNEL, karaoke_mix_item_kind(MIX_ITEM_CHANNEL0 + 15));
    TEST_ASSERT_EQUAL(MIX_KIND_ACTION, karaoke_mix_item_kind(MIX_ITEM_RESET));
    TEST_ASSERT_EQUAL(MIX_KIND_ACTION, karaoke_mix_item_kind(MIX_ITEM_SAVE_ALL));
    TEST_ASSERT_EQUAL(MIX_KIND_ACTION, karaoke_mix_item_kind(MIX_ITEM_DONE));
}

void test_every_row_has_a_name(void)
{
    char scratch[16];
    for (uint8_t i = 0; i < MIX_ITEM_COUNT; i++) {
        TEST_ASSERT_TRUE(strlen(karaoke_mix_item_name(i, scratch, sizeof(scratch))) > 0U);
    }
    TEST_ASSERT_EQUAL_STRING("Ch 1", karaoke_mix_item_name(MIX_ITEM_CHANNEL0, scratch, sizeof(scratch)));
    TEST_ASSERT_EQUAL_STRING("Ch 16", karaoke_mix_item_name(MIX_ITEM_CHANNEL0 + 15, scratch, sizeof(scratch)));
    TEST_ASSERT_EQUAL_STRING("Done", karaoke_mix_item_name(MIX_ITEM_DONE, scratch, sizeof(scratch)));
}

void test_adjusting_steps_by_five_percent_and_clamps(void)
{
    karaoke_mix_t m;
    karaoke_mix_defaults(&m);
    karaoke_mix_item_adjust(&m, 3, +1);
    TEST_ASSERT_EQUAL_UINT8(105, m.section[3]);
    karaoke_mix_item_adjust(&m, 3, -1);
    karaoke_mix_item_adjust(&m, 3, -1);
    TEST_ASSERT_EQUAL_UINT8(95, m.section[3]);
    for (int i = 0; i < 100; i++) karaoke_mix_item_adjust(&m, 3, +1);
    TEST_ASSERT_EQUAL_UINT8(200, m.section[3]);
    for (int i = 0; i < 100; i++) karaoke_mix_item_adjust(&m, 3, -1);
    TEST_ASSERT_EQUAL_UINT8(0, m.section[3]);
}

void test_adjusting_lands_on_multiples_of_five_even_from_an_odd_value(void)
{
    karaoke_mix_t m;
    karaoke_mix_defaults(&m);
    m.section[0] = 103;
    karaoke_mix_item_adjust(&m, 0, +1);
    TEST_ASSERT_EQUAL_UINT8(105, m.section[0]);
    m.section[0] = 103;
    karaoke_mix_item_adjust(&m, 0, -1);
    TEST_ASSERT_EQUAL_UINT8(100, m.section[0]);
}

void test_tone_rows_move_one_db_and_stop_at_six(void)
{
    karaoke_mix_t m;
    karaoke_mix_defaults(&m);
    karaoke_mix_item_adjust(&m, MIX_ITEM_BASS, +1);
    TEST_ASSERT_EQUAL_INT8(1, m.bass);
    for (int i = 0; i < 20; i++) karaoke_mix_item_adjust(&m, MIX_ITEM_TREBLE, -1);
    TEST_ASSERT_EQUAL_INT8(-6, m.treble);
    for (int i = 0; i < 20; i++) karaoke_mix_item_adjust(&m, MIX_ITEM_BASS, +1);
    TEST_ASSERT_EQUAL_INT8(6, m.bass);
    TEST_ASSERT_EQUAL_INT(-6, karaoke_mix_item_min(MIX_ITEM_TREBLE));
    TEST_ASSERT_EQUAL_INT(200, karaoke_mix_item_max(MIX_ITEM_REVERB));
}

void test_an_action_row_cannot_be_adjusted(void)
{
    karaoke_mix_t m, d;
    karaoke_mix_defaults(&m);
    karaoke_mix_defaults(&d);
    karaoke_mix_item_adjust(&m, MIX_ITEM_RESET, +1);
    karaoke_mix_item_adjust(&m, MIX_ITEM_SAVE_ALL, -1);
    karaoke_mix_item_adjust(&m, MIX_ITEM_DONE, +1);
    karaoke_mix_item_click(&m, MIX_ITEM_DONE);
    TEST_ASSERT_TRUE(karaoke_mix_equal(&m, &d));
}

void test_clicking_a_channel_cycles_normal_muted_soloed(void)
{
    karaoke_mix_t m;
    uint8_t item = MIX_ITEM_CHANNEL0 + 4;
    karaoke_mix_defaults(&m);
    TEST_ASSERT_EQUAL_INT(0, karaoke_mix_channel_state(&m, 4));
    karaoke_mix_item_click(&m, item);
    TEST_ASSERT_EQUAL_INT(1, karaoke_mix_channel_state(&m, 4));
    TEST_ASSERT_TRUE(karaoke_mix_item_changed(&m, item));
    karaoke_mix_item_click(&m, item);
    TEST_ASSERT_EQUAL_INT(2, karaoke_mix_channel_state(&m, 4));
    TEST_ASSERT_EQUAL_UINT16(0, m.mute);
    karaoke_mix_item_click(&m, item);
    TEST_ASSERT_EQUAL_INT(0, karaoke_mix_channel_state(&m, 4));
    TEST_ASSERT_FALSE(karaoke_mix_item_changed(&m, item));
}

void test_clicking_a_section_or_effect_row_resets_it(void)
{
    karaoke_mix_t m;
    karaoke_mix_defaults(&m);
    m.section[2] = 40;
    karaoke_mix_item_click(&m, 2);
    TEST_ASSERT_EQUAL_UINT8(100, m.section[2]);
    m.bass = 4;
    karaoke_mix_item_click(&m, MIX_ITEM_BASS);
    TEST_ASSERT_EQUAL_INT8(0, m.bass);
    m.reverb = 0;
    karaoke_mix_item_click(&m, MIX_ITEM_REVERB);
    TEST_ASSERT_EQUAL_UINT8(100, m.reverb);
}

void test_resetting_a_channel_clears_its_gain_mute_and_solo(void)
{
    karaoke_mix_t m;
    uint8_t item = MIX_ITEM_CHANNEL0 + 1;
    karaoke_mix_defaults(&m);
    m.channel[1] = 30;
    m.mute = 0x0002;
    m.solo = 0x0002;
    karaoke_mix_item_reset(&m, item);
    TEST_ASSERT_EQUAL_UINT8(100, m.channel[1]);
    TEST_ASSERT_EQUAL_UINT16(0, m.mute);
    TEST_ASSERT_EQUAL_UINT16(0, m.solo);
}

void test_a_mix_survives_a_save_and_load(void)
{
    karaoke_mix_t a, b;
    char text[400];
    karaoke_mix_defaults(&a);
    a.section[0] = 35;
    a.section[14] = 175;
    a.channel[3] = 60;
    a.channel[15] = 200;
    a.mute = 0x0120;
    a.solo = 0x0004;
    a.reverb = 0;
    a.chorus = 150;
    a.bass = -3;
    a.treble = 6;

    size_t n = karaoke_mix_format(&a, text, sizeof(text));
    TEST_ASSERT_TRUE(n > 0U);
    TEST_ASSERT_EQUAL_UINT(strlen(text), n);

    karaoke_mix_defaults(&b);
    TEST_ASSERT_TRUE(karaoke_mix_parse(&b, text));
    TEST_ASSERT_TRUE(karaoke_mix_equal(&a, &b));
}

void test_formatting_into_a_buffer_that_is_too_small_gives_zero(void)
{
    karaoke_mix_t m;
    char text[20];
    karaoke_mix_defaults(&m);
    TEST_ASSERT_EQUAL_UINT(0, karaoke_mix_format(&m, text, sizeof(text)));
}

void test_a_damaged_or_hand_edited_file_is_clamped_not_trusted(void)
{
    karaoke_mix_t m;
    karaoke_mix_defaults(&m);
    TEST_ASSERT_TRUE(karaoke_mix_parse(&m,
        "# edited\nSEC=999,-4,50,abc\nCHN=300\nREV=400\nCHO=-9\nBASS=99\nTREBLE=-99\nMUTE=70000\nNONSENSE=1\nSEC\n"));
    TEST_ASSERT_EQUAL_UINT8(200, m.section[0]);
    TEST_ASSERT_EQUAL_UINT8(0, m.section[1]);            /* -4 is clamped up to 0 */
    TEST_ASSERT_EQUAL_UINT8(50, m.section[2]);
    TEST_ASSERT_EQUAL_UINT8(100, m.section[3]);          /* parsing stopped at "abc": later entries untouched */
    TEST_ASSERT_EQUAL_UINT8(200, m.channel[0]);
    TEST_ASSERT_EQUAL_UINT8(200, m.reverb);
    TEST_ASSERT_EQUAL_UINT8(0, m.chorus);
    TEST_ASSERT_EQUAL_INT8(6, m.bass);
    TEST_ASSERT_EQUAL_INT8(-6, m.treble);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, m.mute);
}

void test_text_with_nothing_understood_is_reported_and_changes_nothing(void)
{
    karaoke_mix_t m, d;
    karaoke_mix_defaults(&m);
    karaoke_mix_defaults(&d);
    TEST_ASSERT_FALSE(karaoke_mix_parse(&m, "hello\nworld=1\n\n"));
    TEST_ASSERT_FALSE(karaoke_mix_parse(&m, ""));
    TEST_ASSERT_TRUE(karaoke_mix_equal(&m, &d));
}

void test_a_partial_file_keeps_the_rest_of_the_mix(void)
{
    karaoke_mix_t m;
    karaoke_mix_defaults(&m);
    m.bass = 2;
    TEST_ASSERT_TRUE(karaoke_mix_parse(&m, "REV=30\r\n"));
    TEST_ASSERT_EQUAL_UINT8(30, m.reverb);
    TEST_ASSERT_EQUAL_INT8(2, m.bass);
}

void test_song_mix_path(void)
{
    char path[32];
    TEST_ASSERT_TRUE(karaoke_mix_song_path(path, sizeof(path), 13238));
    TEST_ASSERT_EQUAL_STRING("SD:/mix/013238.cfg", path);
    TEST_ASSERT_TRUE(karaoke_mix_song_path(path, sizeof(path), 66666));
    TEST_ASSERT_EQUAL_STRING("SD:/mix/066666.cfg", path);
    TEST_ASSERT_FALSE(karaoke_mix_song_path(path, 8, 13238));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_leave_every_row_unchanged);
    RUN_TEST(test_row_numbers_match_the_screen_layout);
    RUN_TEST(test_every_row_has_a_name);
    RUN_TEST(test_adjusting_steps_by_five_percent_and_clamps);
    RUN_TEST(test_adjusting_lands_on_multiples_of_five_even_from_an_odd_value);
    RUN_TEST(test_tone_rows_move_one_db_and_stop_at_six);
    RUN_TEST(test_an_action_row_cannot_be_adjusted);
    RUN_TEST(test_clicking_a_channel_cycles_normal_muted_soloed);
    RUN_TEST(test_clicking_a_section_or_effect_row_resets_it);
    RUN_TEST(test_resetting_a_channel_clears_its_gain_mute_and_solo);
    RUN_TEST(test_a_mix_survives_a_save_and_load);
    RUN_TEST(test_formatting_into_a_buffer_that_is_too_small_gives_zero);
    RUN_TEST(test_a_damaged_or_hand_edited_file_is_clamped_not_trusted);
    RUN_TEST(test_text_with_nothing_understood_is_reported_and_changes_nothing);
    RUN_TEST(test_a_partial_file_keeps_the_rest_of_the_mix);
    RUN_TEST(test_song_mix_path);
    return UNITY_END();
}
