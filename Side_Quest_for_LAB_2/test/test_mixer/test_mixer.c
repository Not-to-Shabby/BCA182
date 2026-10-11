/**
 * @file test_mixer.c
 * @brief Host tests for the mixer controller: default mix, per-song mix files, and what reaches
 *        the synthesizer. The storage is an in-memory fake.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include <unity.h>
#include <stdio.h>
#include <string.h>

#include "../../src/karaoke_mix.c"

/* ---- a stand-in for the synthesizer that records what the mixer sends ---- */
#define SYNTH_SECT_COUNT 15
static uint8_t g_sect[SYNTH_SECT_COUNT];
static uint8_t g_chan[16];
static uint16_t g_mute, g_solo;
static uint8_t g_rev, g_cho;
static int8_t g_bass, g_treble;

void yamaha_fm_set_section_gain(uint8_t s, uint8_t p) { g_sect[s] = p; }
void yamaha_fm_set_channel_gain(uint8_t c, uint8_t p) { g_chan[c] = p; }
void yamaha_fm_set_channel_mute(uint8_t c, bool m) { g_mute = m ? (uint16_t)(g_mute | (1U << c)) : (uint16_t)(g_mute & ~(1U << c)); }
void yamaha_fm_set_channel_solo(uint8_t c, bool m) { g_solo = m ? (uint16_t)(g_solo | (1U << c)) : (uint16_t)(g_solo & ~(1U << c)); }
void yamaha_fm_set_reverb_level(uint8_t p) { g_rev = p; }
void yamaha_fm_set_chorus_level(uint8_t p) { g_cho = p; }
void yamaha_fm_set_tone(int8_t b, int8_t t) { g_bass = b; g_treble = t; }

/* the real header is not used on the host: the controller only needs the declarations above */
#define YAMAHA_FM_SYNTH_H_
#define SYNTH_MIX_STUB 1
#include "../../include/karaoke_mixer.h"

/* ---- in-memory storage ---- */
#define MAX_FILES 8
static struct { char path[48]; char data[512]; size_t len; bool used; } g_files[MAX_FILES];
static int g_writes, g_deletes;
static bool g_card_ok;

static int find(const char *path)
{
    for (int i = 0; i < MAX_FILES; i++) {
        if (g_files[i].used && strcmp(g_files[i].path, path) == 0) return i;
    }
    return -1;
}

bool mix_store_read(const char *path, char *buf, size_t size, size_t *len)
{
    int i = g_card_ok ? find(path) : -1;
    *len = 0;
    if (i < 0) return false;
    *len = g_files[i].len < size ? g_files[i].len : size;
    memcpy(buf, g_files[i].data, *len);
    return true;
}

bool mix_store_write(const char *path, const char *buf, size_t len)
{
    if (!g_card_ok || len >= sizeof(g_files[0].data)) return false;
    int i = find(path);
    for (int k = 0; i < 0 && k < MAX_FILES; k++) {
        if (!g_files[k].used) { i = k; g_files[k].used = true; strcpy(g_files[k].path, path); }
    }
    if (i < 0) return false;
    memcpy(g_files[i].data, buf, len);
    g_files[i].len = len;
    g_writes++;
    return true;
}

void mix_store_delete(const char *path)
{
    int i = find(path);
    if (i >= 0) { g_files[i].used = false; g_deletes++; }
}

#include "../../src/karaoke_mixer.c"

void setUp(void)
{
    memset(g_files, 0, sizeof(g_files));
    memset(g_sect, 0, sizeof(g_sect));
    memset(g_chan, 0, sizeof(g_chan));
    g_mute = g_solo = 0;
    g_rev = g_cho = 0;
    g_bass = g_treble = 99;
    g_writes = g_deletes = 0;
    g_card_ok = true;
}

void tearDown(void) {}

void test_without_any_file_everything_starts_flat(void)
{
    karaoke_mixer_init();
    karaoke_mixer_song_started(13238);
    for (int i = 0; i < SYNTH_SECT_COUNT; i++) TEST_ASSERT_EQUAL_UINT8(100, g_sect[i]);
    for (int i = 0; i < 16; i++) TEST_ASSERT_EQUAL_UINT8(100, g_chan[i]);
    TEST_ASSERT_EQUAL_UINT8(100, g_rev);
    TEST_ASSERT_EQUAL_INT8(0, g_bass);
    TEST_ASSERT_FALSE(karaoke_mixer_song_has_own_mix());
}

void test_changes_reach_the_synth_immediately(void)
{
    karaoke_mixer_init();
    karaoke_mixer_song_started(1);
    karaoke_mixer_adjust(3, -1);                                     /* bass section */
    karaoke_mixer_adjust(MIX_ITEM_REVERB, -1);
    karaoke_mixer_adjust(MIX_ITEM_TREBLE, +1);
    karaoke_mixer_click(MIX_ITEM_CHANNEL0 + 2);                      /* mute channel 3 */
    TEST_ASSERT_EQUAL_UINT8(95, g_sect[3]);
    TEST_ASSERT_EQUAL_UINT8(95, g_rev);
    TEST_ASSERT_EQUAL_INT8(1, g_treble);
    TEST_ASSERT_EQUAL_UINT16(0x0004, g_mute);
    karaoke_mixer_click(MIX_ITEM_CHANNEL0 + 2);                      /* solo it */
    TEST_ASSERT_EQUAL_UINT16(0, g_mute);
    TEST_ASSERT_EQUAL_UINT16(0x0004, g_solo);
    TEST_ASSERT_TRUE(karaoke_mixer_song_has_own_mix());
}

void test_a_changed_mix_is_saved_for_its_song_only(void)
{
    karaoke_mixer_init();
    karaoke_mixer_song_started(13238);
    karaoke_mixer_adjust(0, +1);
    karaoke_mixer_leave();
    TEST_ASSERT_TRUE(find("SD:/mix/013238.cfg") >= 0);
    TEST_ASSERT_EQUAL_INT(1, g_writes);

    karaoke_mixer_song_started(500);                                  /* another song is flat */
    TEST_ASSERT_EQUAL_UINT8(100, g_sect[0]);
    karaoke_mixer_song_started(13238);                                /* coming back restores it */
    TEST_ASSERT_EQUAL_UINT8(105, g_sect[0]);
    TEST_ASSERT_TRUE(karaoke_mixer_song_has_own_mix());
}

void test_putting_a_song_back_to_the_default_removes_its_file(void)
{
    karaoke_mixer_init();
    karaoke_mixer_song_started(7);
    karaoke_mixer_adjust(0, +1);
    karaoke_mixer_leave();
    TEST_ASSERT_TRUE(find("SD:/mix/000007.cfg") >= 0);

    karaoke_mixer_song_started(7);
    karaoke_mixer_click(MIX_ITEM_RESET);
    karaoke_mixer_leave();
    TEST_ASSERT_EQUAL_INT(-1, find("SD:/mix/000007.cfg"));
    TEST_ASSERT_EQUAL_INT(1, g_deletes);
    TEST_ASSERT_EQUAL_UINT8(100, g_sect[0]);
}

void test_leaving_an_untouched_song_writes_and_deletes_nothing(void)
{
    karaoke_mixer_init();
    karaoke_mixer_song_started(9);
    karaoke_mixer_leave();
    TEST_ASSERT_EQUAL_INT(0, g_writes);
    TEST_ASSERT_EQUAL_INT(0, g_deletes);
}

void test_set_as_default_applies_to_songs_without_their_own_mix(void)
{
    karaoke_mixer_init();
    karaoke_mixer_song_started(1);
    karaoke_mixer_adjust(MIX_ITEM_BASS, +1);
    karaoke_mixer_adjust(MIX_ITEM_BASS, +1);
    karaoke_mixer_click(MIX_ITEM_SAVE_ALL);
    TEST_ASSERT_TRUE(find(MIXER_DEFAULT_PATH) >= 0);
    TEST_ASSERT_FALSE(karaoke_mixer_song_has_own_mix());              /* the song now equals the default */
    karaoke_mixer_leave();
    TEST_ASSERT_EQUAL_INT(-1, find("SD:/mix/000001.cfg"));

    karaoke_mixer_song_started(2);
    TEST_ASSERT_EQUAL_INT8(2, g_bass);

    karaoke_mixer_init();                                             /* and it survives a restart */
    karaoke_mixer_song_started(3);
    TEST_ASSERT_EQUAL_INT8(2, g_bass);
}

void test_a_song_mix_wins_over_the_default(void)
{
    karaoke_mixer_init();
    karaoke_mixer_song_started(1);
    karaoke_mixer_adjust(MIX_ITEM_BASS, +1);
    karaoke_mixer_click(MIX_ITEM_SAVE_ALL);                           /* default bass +1 */
    karaoke_mixer_song_started(2);
    karaoke_mixer_adjust(MIX_ITEM_BASS, -1);
    karaoke_mixer_adjust(MIX_ITEM_BASS, -1);                          /* this song: -1 */
    karaoke_mixer_leave();
    karaoke_mixer_song_started(3);
    TEST_ASSERT_EQUAL_INT8(1, g_bass);
    karaoke_mixer_song_started(2);
    TEST_ASSERT_EQUAL_INT8(-1, g_bass);
}

void test_reset_this_song_returns_to_the_default_not_to_flat(void)
{
    karaoke_mixer_init();
    karaoke_mixer_song_started(1);
    karaoke_mixer_adjust(0, -1);
    karaoke_mixer_click(MIX_ITEM_SAVE_ALL);                           /* default piano 95 */
    karaoke_mixer_adjust(0, -1);
    karaoke_mixer_adjust(0, -1);
    karaoke_mixer_click(MIX_ITEM_RESET);
    TEST_ASSERT_EQUAL_UINT8(95, g_sect[0]);
}

void test_a_missing_card_does_not_break_the_mixer(void)
{
    g_card_ok = false;
    karaoke_mixer_init();
    karaoke_mixer_song_started(1);
    karaoke_mixer_adjust(0, +1);
    karaoke_mixer_leave();
    karaoke_mixer_click(MIX_ITEM_SAVE_ALL);
    TEST_ASSERT_EQUAL_UINT8(105, g_sect[0]);                          /* still applied for this session */
    TEST_ASSERT_EQUAL_INT(0, g_writes);
}

void test_a_corrupt_song_file_falls_back_to_the_default(void)
{
    karaoke_mixer_init();
    mix_store_write("SD:/mix/000042.cfg", "garbage\nmore garbage\n", 21);
    karaoke_mixer_song_started(42);
    for (int i = 0; i < SYNTH_SECT_COUNT; i++) TEST_ASSERT_EQUAL_UINT8(100, g_sect[i]);
}

void test_leaving_twice_writes_only_once(void)
{
    karaoke_mixer_init();
    karaoke_mixer_song_started(5);
    karaoke_mixer_adjust(0, +1);
    karaoke_mixer_leave();
    karaoke_mixer_leave();
    karaoke_mixer_leave();
    TEST_ASSERT_EQUAL_INT(1, g_writes);
}

void test_set_as_default_keeps_per_song_channel_settings_out_of_other_songs(void)
{
    karaoke_mixer_init();
    karaoke_mixer_song_started(1);
    karaoke_mixer_adjust(0, -1);                                      /* a section change */
    karaoke_mixer_click(MIX_ITEM_CHANNEL0 + 3);                       /* mute channel 4 */
    karaoke_mixer_adjust(MIX_ITEM_CHANNEL0 + 5, -1);                  /* channel 6 at 95 % */
    karaoke_mixer_click(MIX_ITEM_SAVE_ALL);
    TEST_ASSERT_EQUAL_UINT8(95, karaoke_mixer_get_default()->section[0]);
    TEST_ASSERT_EQUAL_UINT16(0, karaoke_mixer_get_default()->mute);
    TEST_ASSERT_EQUAL_UINT8(100, karaoke_mixer_get_default()->channel[5]);
    TEST_ASSERT_TRUE(karaoke_mixer_song_has_own_mix());               /* this song keeps its channel settings */
    karaoke_mixer_leave();
    karaoke_mixer_song_started(2);
    TEST_ASSERT_EQUAL_UINT8(95, g_sect[0]);
    TEST_ASSERT_EQUAL_UINT16(0, g_mute);
    karaoke_mixer_song_started(1);
    TEST_ASSERT_EQUAL_UINT16(0x0008, g_mute);
    TEST_ASSERT_EQUAL_UINT8(95, g_chan[5]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_without_any_file_everything_starts_flat);
    RUN_TEST(test_changes_reach_the_synth_immediately);
    RUN_TEST(test_a_changed_mix_is_saved_for_its_song_only);
    RUN_TEST(test_putting_a_song_back_to_the_default_removes_its_file);
    RUN_TEST(test_leaving_an_untouched_song_writes_and_deletes_nothing);
    RUN_TEST(test_set_as_default_applies_to_songs_without_their_own_mix);
    RUN_TEST(test_a_song_mix_wins_over_the_default);
    RUN_TEST(test_reset_this_song_returns_to_the_default_not_to_flat);
    RUN_TEST(test_a_missing_card_does_not_break_the_mixer);
    RUN_TEST(test_a_corrupt_song_file_falls_back_to_the_default);
    RUN_TEST(test_leaving_twice_writes_only_once);
    RUN_TEST(test_set_as_default_keeps_per_song_channel_settings_out_of_other_songs);
    return UNITY_END();
}
