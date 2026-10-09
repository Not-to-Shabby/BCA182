/**
 * @file test_melody.c
 * @brief Host tests for lead-melody detection: the channel that follows the lyric syllables.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include <unity.h>
#include <stdint.h>
#include <string.h>
#include "../../src/midi_karaoke_parser.c"

#define PPQN 96
#define SYLLABLES 12

static uint8_t g_smf[8192];
static uint32_t g_len;

static void put(uint8_t b) { g_smf[g_len++] = b; }

static void put_vlq(uint32_t v)
{
    uint8_t tmp[4];
    int n = 0;
    tmp[n++] = (uint8_t)(v & 0x7FU);
    while ((v >>= 7) != 0U) {
        tmp[n++] = (uint8_t)((v & 0x7FU) | 0x80U);
    }
    while (n > 0) {
        put(tmp[--n]);
    }
}

static void begin_file(uint16_t tracks)
{
    g_len = 0;
    memcpy(g_smf, "MThd\0\0\0\6\0\1", 10);
    g_len = 10;
    put((uint8_t)(tracks >> 8)); put((uint8_t)tracks);
    put(0); put(PPQN);
}

static uint32_t begin_track(void)
{
    memcpy(g_smf + g_len, "MTrk\0\0\0\0", 8);
    g_len += 8;
    return g_len;
}

static void end_track(uint32_t start)
{
    put(0); put(0xFF); put(0x2F); put(0);
    uint32_t n = g_len - start;
    g_smf[start - 4] = (uint8_t)(n >> 24); g_smf[start - 3] = (uint8_t)(n >> 16);
    g_smf[start - 2] = (uint8_t)(n >> 8);  g_smf[start - 1] = (uint8_t)n;
}

/* Lyrics on a track of their own, one syllable per beat. */
static void lyric_track(void)
{
    uint32_t t = begin_track();
    for (int i = 0; i < SYLLABLES; i++) {
        put_vlq(i == 0 ? 0 : PPQN);
        put(0xFF); put(0x05); put(2); put('l'); put('a');
    }
    end_track(t);
}

/* A channel that plays a note on every beat (hits every syllable), or only every 'step' beats. */
static void note_track(uint8_t ch, int notes, uint32_t spacing)
{
    uint32_t t = begin_track();
    for (int i = 0; i < notes; i++) {
        put_vlq(i == 0 ? 0 : spacing);
        put((uint8_t)(0x90 | ch)); put(60); put(90);
        put_vlq(10);
        put((uint8_t)(0x80 | ch)); put(60); put(0);
    }
    end_track(t);
}

void setUp(void) {}
void tearDown(void) {}

void test_channel_following_the_syllables_is_the_lead(void)
{
    begin_file(3);
    lyric_track();
    note_track(0, SYLLABLES, PPQN - 10U);    /* on the beat */
    note_track(4, SYLLABLES * 8, 12);        /* busy, mostly off the syllables */
    TEST_ASSERT_TRUE(midi_karaoke_load_memory(g_smf, g_len));
    TEST_ASSERT_EQUAL_INT8(0, midi_karaoke_get_melody_channel());
}

void test_busy_channel_overlapping_by_chance_does_not_win(void)
{
    begin_file(3);
    lyric_track();
    note_track(6, SYLLABLES * 8, 12);        /* hits every syllable among many other notes */
    note_track(2, SYLLABLES, PPQN - 10U);    /* sparse and exact */
    TEST_ASSERT_TRUE(midi_karaoke_load_memory(g_smf, g_len));
    TEST_ASSERT_EQUAL_INT8(2, midi_karaoke_get_melody_channel());
}

void test_chords_do_not_beat_a_single_line(void)
{
    begin_file(3);
    lyric_track();
    {
        uint32_t t = begin_track();
        for (int i = 0; i < SYLLABLES; i++) {
            put_vlq(i == 0 ? 0 : PPQN - 10U);
            for (uint8_t k = 0; k < 3; k++) {
                if (k > 0) put_vlq(0);
                put(0x94); put((uint8_t)(48 + k * 4)); put(90);
            }
            put_vlq(10);
            put(0x84); put(48); put(0);
        }
        end_track(t);
    }
    note_track(0, SYLLABLES, PPQN - 10U);
    TEST_ASSERT_TRUE(midi_karaoke_load_memory(g_smf, g_len));
    TEST_ASSERT_EQUAL_INT8(0, midi_karaoke_get_melody_channel());
}

void test_song_without_lyrics_has_no_melody_channel(void)
{
    begin_file(1);
    note_track(0, SYLLABLES, PPQN - 10U);
    TEST_ASSERT_TRUE(midi_karaoke_load_memory(g_smf, g_len));
    TEST_ASSERT_EQUAL_INT8(-1, midi_karaoke_get_melody_channel());
}

void test_drum_channel_is_never_the_lead(void)
{
    begin_file(2);
    lyric_track();
    note_track(9, SYLLABLES, PPQN - 10U);
    TEST_ASSERT_TRUE(midi_karaoke_load_memory(g_smf, g_len));
    TEST_ASSERT_EQUAL_INT8(-1, midi_karaoke_get_melody_channel());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_channel_following_the_syllables_is_the_lead);
    RUN_TEST(test_busy_channel_overlapping_by_chance_does_not_win);
    RUN_TEST(test_chords_do_not_beat_a_single_line);
    RUN_TEST(test_song_without_lyrics_has_no_melody_channel);
    RUN_TEST(test_drum_channel_is_never_the_lead);
    return UNITY_END();
}
