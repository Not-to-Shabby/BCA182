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

void test_guide_vocal_notes_stored_on_the_lyric_track_are_found(void)
{
    begin_file(2);
    {
        uint32_t t = begin_track();       /* one track: a lyric and a note on every beat */
        for (int i = 0; i < SYLLABLES; i++) {
            put_vlq(i == 0 ? 0 : PPQN - 10U);
            put(0xFF); put(0x05); put(2); put('l'); put('a');
            put_vlq(0);
            put(0x90); put(64); put(90);
            put_vlq(10);
            put(0x80); put(64); put(0);
        }
        end_track(t);
    }
    note_track(5, SYLLABLES * 6, 20);     /* a busy accompaniment that overlaps some syllables */
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

/* ---- usage scan: what the song plays, for the mixer ---- */

static void ev_program(uint8_t ch, uint8_t prog)
{
    put_vlq(0); put((uint8_t)(0xC0 | ch)); put(prog);
}

static void ev_note(uint8_t ch, uint8_t key, uint8_t vel)
{
    put_vlq(0); put((uint8_t)(0x90 | ch)); put(key); put(vel);
    put_vlq(10); put((uint8_t)(0x80 | ch)); put(key); put(0);
}

/* Roland GS "use for rhythm part": F0 41 10 42 12 40 1p 15 map cs F7 */
static void ev_gs_rhythm(uint8_t part_nibble, uint8_t map)
{
    static const uint8_t head[] = { 0x41, 0x10, 0x42, 0x12, 0x40 };
    put_vlq(0); put(0xF0); put_vlq(10);
    for (size_t i = 0; i < sizeof(head); i++) put(head[i]);
    put((uint8_t)(0x10 | part_nibble)); put(0x15); put(map); put(0x00); put(0xF7);
}

void test_usage_lists_only_the_channels_and_sections_the_song_plays(void)
{
    begin_file(1);
    uint32_t t = begin_track();
    ev_program(0, 0);       /* piano */
    ev_program(2, 73);      /* flute */
    ev_program(5, 33);      /* finger bass */
    ev_program(7, 48);      /* strings: set but never played */
    ev_note(0, 60, 90);
    ev_note(2, 72, 90);
    ev_note(5, 36, 90);
    ev_note(9, 38, 100);    /* snare */
    ev_note(9, 42, 100);    /* hi-hat */
    end_track(t);
    TEST_ASSERT_TRUE(midi_karaoke_load_memory(g_smf, g_len));

    midi_usage_t u;
    midi_karaoke_get_usage(&u);
    TEST_ASSERT_TRUE(u.valid);
    TEST_ASSERT_EQUAL_HEX16((1U << 0) | (1U << 2) | (1U << 5) | (1U << 9), u.channels);
    TEST_ASSERT_EQUAL_UINT8(0, u.program[0]);
    TEST_ASSERT_EQUAL_UINT8(73, u.program[2]);
    TEST_ASSERT_EQUAL_UINT8(33, u.program[5]);
    TEST_ASSERT_EQUAL_UINT16(1, u.notes[0]);
    uint16_t want = (1U << SYNTH_SECT_PIANO) | (1U << SYNTH_SECT_REEDS) | (1U << SYNTH_SECT_BASS) |
                    (1U << SYNTH_SECT_SNARE) | (1U << SYNTH_SECT_HATS);
    TEST_ASSERT_EQUAL_HEX16(want, u.sections);
}

void test_usage_ignores_notes_with_zero_velocity_and_unused_programs(void)
{
    begin_file(1);
    uint32_t t = begin_track();
    ev_program(3, 56);
    ev_note(3, 60, 0);      /* a note-off in disguise */
    end_track(t);
    TEST_ASSERT_TRUE(midi_karaoke_load_memory(g_smf, g_len));

    midi_usage_t u;
    midi_karaoke_get_usage(&u);
    TEST_ASSERT_EQUAL_HEX16(0, u.channels);
    TEST_ASSERT_EQUAL_HEX16(0, u.sections);
}

void test_usage_uses_the_program_in_effect_when_each_note_starts(void)
{
    begin_file(1);
    uint32_t t = begin_track();
    ev_program(1, 0);
    ev_note(1, 60, 90);
    ev_program(1, 56);      /* the same channel changes to trumpet later */
    ev_note(1, 62, 90);
    end_track(t);
    TEST_ASSERT_TRUE(midi_karaoke_load_memory(g_smf, g_len));

    midi_usage_t u;
    midi_karaoke_get_usage(&u);
    TEST_ASSERT_EQUAL_UINT8(0, u.program[1]);        /* the first note's program names the channel */
    TEST_ASSERT_EQUAL_HEX16((1U << SYNTH_SECT_PIANO) | (1U << SYNTH_SECT_BRASS), u.sections);
    TEST_ASSERT_EQUAL_UINT16(2, u.notes[1]);
}

void test_usage_follows_a_gs_drum_part_on_another_channel(void)
{
    begin_file(1);
    uint32_t t = begin_track();
    ev_gs_rhythm(0x0A, 2);  /* nibble A = part 11 = channel index 10 becomes a drum part */
    ev_program(10, 25);     /* TR-808 kit */
    ev_note(10, 36, 100);   /* a kick: drums, not a bass guitar note */
    ev_note(10, 49, 100);   /* crash */
    end_track(t);
    TEST_ASSERT_TRUE(midi_karaoke_load_memory(g_smf, g_len));

    midi_usage_t u;
    midi_karaoke_get_usage(&u);
    TEST_ASSERT_TRUE((u.drum_parts & (1U << 10)) != 0U);
    TEST_ASSERT_TRUE((u.drum_parts & (1U << 9)) != 0U);
    TEST_ASSERT_EQUAL_HEX16((1U << SYNTH_SECT_KICK) | (1U << SYNTH_SECT_CYMBALS), u.sections);
}

void test_usage_with_channel_ten_as_melodic_part_is_still_drums(void)
{
    begin_file(1);
    uint32_t t = begin_track();
    ev_note(9, 38, 100);
    end_track(t);
    TEST_ASSERT_TRUE(midi_karaoke_load_memory(g_smf, g_len));

    midi_usage_t u;
    midi_karaoke_get_usage(&u);
    TEST_ASSERT_EQUAL_HEX16(1U << SYNTH_SECT_SNARE, u.sections);
}

void test_usage_is_cleared_by_the_next_load(void)
{
    begin_file(1);
    uint32_t t = begin_track();
    ev_note(0, 60, 90);
    end_track(t);
    TEST_ASSERT_TRUE(midi_karaoke_load_memory(g_smf, g_len));
    begin_file(1);
    t = begin_track();
    ev_note(4, 60, 90);
    end_track(t);
    TEST_ASSERT_TRUE(midi_karaoke_load_memory(g_smf, g_len));

    midi_usage_t u;
    midi_karaoke_get_usage(&u);
    TEST_ASSERT_EQUAL_HEX16(1U << 4, u.channels);
}

void test_usage_counts_notes_on_several_tracks(void)
{
    begin_file(2);
    uint32_t t = begin_track();
    ev_note(0, 60, 90);
    end_track(t);
    t = begin_track();
    ev_note(0, 62, 90);
    ev_note(0, 64, 90);
    end_track(t);
    TEST_ASSERT_TRUE(midi_karaoke_load_memory(g_smf, g_len));

    midi_usage_t u;
    midi_karaoke_get_usage(&u);
    TEST_ASSERT_EQUAL_UINT16(3, u.notes[0]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_channel_following_the_syllables_is_the_lead);
    RUN_TEST(test_busy_channel_overlapping_by_chance_does_not_win);
    RUN_TEST(test_chords_do_not_beat_a_single_line);
    RUN_TEST(test_guide_vocal_notes_stored_on_the_lyric_track_are_found);
    RUN_TEST(test_song_without_lyrics_has_no_melody_channel);
    RUN_TEST(test_drum_channel_is_never_the_lead);
    RUN_TEST(test_usage_lists_only_the_channels_and_sections_the_song_plays);
    RUN_TEST(test_usage_ignores_notes_with_zero_velocity_and_unused_programs);
    RUN_TEST(test_usage_uses_the_program_in_effect_when_each_note_starts);
    RUN_TEST(test_usage_follows_a_gs_drum_part_on_another_channel);
    RUN_TEST(test_usage_with_channel_ten_as_melodic_part_is_still_drums);
    RUN_TEST(test_usage_is_cleared_by_the_next_load);
    RUN_TEST(test_usage_counts_notes_on_several_tracks);
    return UNITY_END();
}
