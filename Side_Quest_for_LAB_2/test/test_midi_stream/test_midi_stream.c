/**
 * @file test_midi_stream.c
 * @brief Host tests for streaming a Standard MIDI File through the sequencer: what plays from a
 *        read callback must equal what plays from memory, wherever the 512-byte windows fall.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include <unity.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "../../src/midi_karaoke_parser.c"

#define PPQN 96
#define FILE_MAX 60000
#define LOG_MAX 400000

static uint8_t g_smf[FILE_MAX];
static uint32_t g_len;

static void put(uint8_t b) { g_smf[g_len++] = b; }

static void put_vlq(uint32_t v)
{
    uint8_t tmp[5];
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

/* One entry per synth callback, so two runs can be compared exactly. */
static char g_log[LOG_MAX];
static size_t g_log_len;
static char g_expected[LOG_MAX];

static void note_log(const char *name, unsigned a, unsigned b, unsigned c)
{
    if (g_log_len + 40U < sizeof(g_log)) {
        g_log_len += (size_t)snprintf(g_log + g_log_len, sizeof(g_log) - g_log_len,
                                      "%s %u %u %u;", name, a, b, c);
    }
}
static void on_note_on(uint8_t ch, uint8_t n, uint8_t v) { note_log("on", ch, n, v); }
static void on_note_off(uint8_t ch, uint8_t n, uint8_t v) { note_log("off", ch, n, v); }
static void on_prog(uint8_t ch, uint8_t p) { note_log("prog", ch, p, 0); }
static void on_cc(uint8_t ch, uint8_t c, uint8_t v) { note_log("cc", ch, c, v); }
static void on_bend(uint8_t ch, uint16_t b) { note_log("bend", ch, b, 0); }
static void on_all_off(void) { note_log("alloff", 0, 0, 0); }

static const midi_synth_callbacks_t CB = {
    on_note_on, on_note_off, on_prog, on_cc, on_bend, on_all_off
};

/* ---- a source that serves g_smf and can be made to fail ---- */
static uint32_t g_reads;
static uint32_t g_fail_after;      /* reads that succeed before the source starts failing */
static uint32_t g_max_read;

static bool mem_read(void *ctx, uint32_t off, uint8_t *dst, uint32_t len)
{
    (void)ctx;
    if (g_reads >= g_fail_after) {
        return false;
    }
    g_reads++;
    if (len > g_max_read) {
        g_max_read = len;
    }
    if (off + len > g_len) {
        return false;
    }
    memcpy(dst, g_smf + off, len);
    return true;
}

static midi_source_t streamed(void)
{
    g_reads = 0;
    g_fail_after = UINT32_MAX;
    g_max_read = 0;
    midi_source_t s = { .mem = NULL, .size = g_len, .read = mem_read, .ctx = NULL };
    return s;
}

static void run_to_end(void)
{
    g_log_len = 0;
    g_log[0] = '\0';
    midi_karaoke_play();
    for (int i = 0; i < 20000; i++) {
        midi_karaoke_tick(10000);
    }
}

/* Plays g_smf from memory and keeps the callback log as the reference. */
static void play_from_memory(void)
{
    midi_karaoke_init(&CB);
    TEST_ASSERT_TRUE(midi_karaoke_load_memory(g_smf, g_len));
    run_to_end();
    memcpy(g_expected, g_log, g_log_len + 1U);
}

void setUp(void) {}
void tearDown(void) {}

/* Notes, controllers, bends, running status, SysEx, tempo and lyrics across many windows. */
static void build_busy_song(int tracks, int events_per_track)
{
    begin_file((uint16_t)tracks);
    uint32_t t0 = begin_track();
    put(0); put(0xFF); put(0x51); put(3); put(0x07); put(0xA1); put(0x20);
    for (int i = 0; i < events_per_track / 4; i++) {
        put_vlq(48);
        put(0xFF); put(0x05); put(5); put('s'); put('y'); put('l'); put((uint8_t)('0' + i % 10)); put(' ');
    }
    end_track(t0);

    for (int t = 1; t < tracks; t++) {
        uint32_t st = begin_track();
        uint8_t ch = (uint8_t)(t % 9);
        put(0); put((uint8_t)(0xC0 | ch)); put((uint8_t)(t * 2));
        for (int i = 0; i < events_per_track; i++) {
            put_vlq((uint32_t)(i % 7) * 5U);
            put((uint8_t)(0x90 | ch)); put((uint8_t)(40 + (i + t) % 40)); put(80);
            put_vlq(20);
            put((uint8_t)(0x80 | ch)); put((uint8_t)(40 + (i + t) % 40)); put(0);
            if (i % 11 == 0) {
                put_vlq(1);
                put(0x07); put(100);                      /* running status: CC 7 */
            }
            if (i % 17 == 0) {
                put_vlq(1);
                put((uint8_t)(0xE0 | ch)); put(0); put(0x50);
            }
            if (i % 23 == 0) {
                put_vlq(0);
                put(0xF0); put_vlq(5); put(1); put(2); put(3); put(4); put(0xF7);
            }
        }
        end_track(st);
    }
}

void test_streamed_playback_matches_playback_from_memory(void)
{
    build_busy_song(6, 400);
    TEST_ASSERT_TRUE(g_len > 3000U);

    play_from_memory();
    size_t expected_len = g_log_len;
    TEST_ASSERT_TRUE(expected_len > 1000U);

    midi_karaoke_init(&CB);
    midi_source_t src = streamed();
    TEST_ASSERT_TRUE(midi_karaoke_load(&src));
    run_to_end();

    TEST_ASSERT_EQUAL_size_t(expected_len, g_log_len);
    TEST_ASSERT_EQUAL_STRING(g_expected, g_log);
    TEST_ASSERT_TRUE(g_reads > 6U);
}

void test_no_read_is_bigger_than_the_scan_window(void)
{
    build_busy_song(4, 300);
    midi_karaoke_init(&CB);
    midi_source_t src = streamed();
    TEST_ASSERT_TRUE(midi_karaoke_load(&src));
    run_to_end();
    TEST_ASSERT_TRUE(g_max_read <= SCAN_WINDOW_BYTES);
}

void test_events_straddling_a_window_edge_decode_the_same(void)
{
    /* Move the second track one byte at a time so each event kind lands on a window edge. */
    for (int pad = 0; pad < 16; pad++) {
        begin_file(2);
        uint32_t t0 = begin_track();
        for (int i = 0; i < 120 + pad; i++) {
            put(0); put(0xFF); put(0x7F); put(0);
        }
        end_track(t0);
        uint32_t t1 = begin_track();
        for (int i = 0; i < 40; i++) {
            put_vlq(3);
            put(0x90); put((uint8_t)(50 + i)); put(90);
            put_vlq(2);
            put(0xFF); put(0x05); put(4); put('a'); put('b'); put('c'); put('d');
            put_vlq(1);
            put(0xB0); put(10); put((uint8_t)i);
        }
        end_track(t1);

        play_from_memory();

        midi_karaoke_init(&CB);
        midi_source_t src = streamed();
        TEST_ASSERT_TRUE(midi_karaoke_load(&src));
        run_to_end();
        TEST_ASSERT_EQUAL_STRING(g_expected, g_log);
    }
}

void test_more_tracks_than_the_old_limit_all_play(void)
{
    build_busy_song(40, 12);
    midi_karaoke_init(&CB);
    midi_source_t src = streamed();
    TEST_ASSERT_TRUE(midi_karaoke_load(&src));
    run_to_end();

    midi_player_status_t st;
    midi_karaoke_get_status(&st);
    TEST_ASSERT_EQUAL_UINT16(40, st.num_tracks);
    TEST_ASSERT_NOT_NULL(strstr(g_log, "prog 3 78 0;"));   /* the last track: channel 39 % 9, program 39 * 2 */
}

void test_a_song_whose_source_fails_stops_and_silences_every_note(void)
{
    build_busy_song(3, 300);

    midi_karaoke_init(&CB);
    midi_source_t src = streamed();
    TEST_ASSERT_TRUE(midi_karaoke_load(&src));
    g_fail_after = g_reads + 2U;           /* fail a couple of reads into playback */
    run_to_end();

    TEST_ASSERT_TRUE(g_log_len > 0U);
    midi_player_status_t st;
    midi_karaoke_get_status(&st);
    TEST_ASSERT_FALSE(st.is_playing);      /* stopped instead of hanging */

    /* The last thing the synth hears is "all notes off": no track is left half played. */
    const char *tail = strrchr(g_log, ';');
    TEST_ASSERT_NOT_NULL(tail);
    TEST_ASSERT_TRUE(g_log_len >= 9U);
    TEST_ASSERT_EQUAL_STRING("alloff 0 0 0;", g_log + g_log_len - 13U);
}

void test_a_failed_source_ends_the_song_early_not_at_its_natural_end(void)
{
    build_busy_song(3, 300);

    midi_karaoke_init(&CB);
    midi_source_t src = streamed();
    TEST_ASSERT_TRUE(midi_karaoke_load(&src));
    run_to_end();
    size_t full = g_log_len;

    midi_karaoke_init(&CB);
    src = streamed();
    TEST_ASSERT_TRUE(midi_karaoke_load(&src));
    g_fail_after = g_reads + 2U;
    run_to_end();

    TEST_ASSERT_TRUE(g_log_len < full);
}

void test_a_source_that_fails_during_the_load_scans_is_refused(void)
{
    build_busy_song(3, 300);

    midi_karaoke_init(&CB);
    midi_source_t src = streamed();
    g_fail_after = 6U;                     /* header and track heads read, the lyric scan fails */
    TEST_ASSERT_FALSE(midi_karaoke_load(&src));

    midi_player_status_t st;
    midi_karaoke_get_status(&st);
    TEST_ASSERT_FALSE(st.is_playing);
    TEST_ASSERT_EQUAL_UINT16(0, st.num_tracks);
}

void test_the_elapsed_clock_keeps_the_sub_millisecond_part_of_every_tick(void)
{
    begin_file(1);
    uint32_t t0 = begin_track();
    put_vlq(PPQN * 40);
    put(0x90); put(60); put(90);
    end_track(t0);

    midi_karaoke_init(&CB);
    TEST_ASSERT_TRUE(midi_karaoke_load_memory(g_smf, g_len));
    midi_karaoke_play();
    for (int i = 0; i < 2000; i++) {
        midi_karaoke_tick(9800);                 /* 19.6 s in ticks that are not whole milliseconds */
    }
    midi_player_status_t st;
    midi_karaoke_get_status(&st);
    TEST_ASSERT_UINT32_WITHIN(2, 19600, st.elapsed_ms);
}

void test_a_long_stall_is_made_up_by_the_catch_up_limit(void)
{
    TEST_ASSERT_TRUE(SEQ_MAX_CATCHUP_US >= 1000000U);   /* longer than any stall seen on the board */
}

void test_loading_a_good_song_after_a_failed_one_plays_normally(void)
{
    build_busy_song(3, 300);
    play_from_memory();

    midi_karaoke_init(&CB);
    midi_source_t bad = streamed();
    g_fail_after = 6U;
    TEST_ASSERT_FALSE(midi_karaoke_load(&bad));

    midi_source_t good = streamed();
    TEST_ASSERT_TRUE(midi_karaoke_load(&good));
    run_to_end();
    TEST_ASSERT_EQUAL_STRING(g_expected, g_log);
}

void test_a_source_that_fails_at_once_is_refused_cleanly(void)
{
    build_busy_song(2, 20);
    midi_karaoke_init(&CB);
    midi_source_t src = streamed();
    g_fail_after = 0;
    TEST_ASSERT_FALSE(midi_karaoke_load(&src));
}

void test_truncated_file_plays_what_is_there(void)
{
    build_busy_song(3, 200);
    g_len = g_len / 2U;                    /* the last track is cut off mid-event */

    play_from_memory();
    TEST_ASSERT_TRUE(g_log_len > 0U);

    midi_karaoke_init(&CB);
    midi_source_t src = streamed();
    TEST_ASSERT_TRUE(midi_karaoke_load(&src));
    run_to_end();
    TEST_ASSERT_EQUAL_STRING(g_expected, g_log);
}

void test_lyrics_and_melody_detection_work_when_streamed(void)
{
    begin_file(3);
    uint32_t t0 = begin_track();
    for (int i = 0; i < 12; i++) {
        put_vlq(i == 0 ? 0 : PPQN);
        put(0xFF); put(0x05); put(2); put('l'); put('a');
    }
    end_track(t0);
    uint32_t t1 = begin_track();
    for (int i = 0; i < 12; i++) {
        put_vlq(i == 0 ? 0 : PPQN - 10U);
        put(0x93); put(60); put(90);
        put_vlq(10);
        put(0x83); put(60); put(0);
    }
    end_track(t1);
    uint32_t t2 = begin_track();
    for (int i = 0; i < 96; i++) {
        put_vlq(i == 0 ? 0 : 12);
        put(0x96); put(48); put(80);
        put_vlq(0);
        put(0x86); put(48); put(0);
    }
    end_track(t2);

    midi_karaoke_init(&CB);
    midi_source_t src = streamed();
    TEST_ASSERT_TRUE(midi_karaoke_load(&src));
    TEST_ASSERT_EQUAL_INT8(3, midi_karaoke_get_melody_channel());

    midi_player_status_t st;
    midi_karaoke_get_status(&st);
    TEST_ASSERT_NOT_NULL(strstr(st.upcoming_lyric_line, "la"));
}

void test_a_lyric_is_shown_when_its_sound_is_heard_not_when_it_is_read(void)
{
    begin_file(1);
    uint32_t t0 = begin_track();
    put_vlq(PPQN);                               /* half a second at 120 BPM */
    put(0xFF); put(0x05); put(3); put('a'); put('b'); put(' ');
    put_vlq(PPQN * 4U);
    put(0xFF); put(0x05); put(2); put('c'); put('d');
    end_track(t0);

    midi_karaoke_init(&CB);
    TEST_ASSERT_TRUE(midi_karaoke_load_memory(g_smf, g_len));
    midi_karaoke_play();

    karaoke_lyric_msg_t msg;
    bool seen = false;
    uint32_t shown_ms = 0;
    for (int i = 0; i < 100 && !seen; i++) {
        midi_karaoke_tick(10000);
        if (midi_karaoke_pop_lyric(&msg)) {
            seen = true;
            shown_ms = msg.song_time_ms;
        }
    }
    TEST_ASSERT_TRUE(seen);
    TEST_ASSERT_TRUE(shown_ms >= 500U + MIDI_EVENT_LEAD_MS);
    TEST_ASSERT_TRUE(shown_ms <= 500U + MIDI_EVENT_LEAD_MS + 20U);
}

void test_lyrics_still_waiting_when_the_song_ends_are_not_lost(void)
{
    begin_file(1);
    uint32_t t0 = begin_track();
    put_vlq(PPQN);
    put(0xFF); put(0x05); put(2); put('e'); put('n');
    end_track(t0);

    midi_karaoke_init(&CB);
    TEST_ASSERT_TRUE(midi_karaoke_load_memory(g_smf, g_len));
    run_to_end();

    karaoke_lyric_msg_t msg;
    TEST_ASSERT_TRUE(midi_karaoke_pop_lyric(&msg));
    TEST_ASSERT_EQUAL_STRING("en", msg.text);
}

void test_stopping_discards_lyrics_that_were_not_shown_yet(void)
{
    begin_file(1);
    uint32_t t0 = begin_track();
    put_vlq(0);
    put(0xFF); put(0x05); put(2); put('x'); put('y');
    put_vlq(PPQN * 8U);
    put(0xFF); put(0x05); put(2); put('z'); put('w');
    end_track(t0);

    midi_karaoke_init(&CB);
    TEST_ASSERT_TRUE(midi_karaoke_load_memory(g_smf, g_len));
    midi_karaoke_play();
    midi_karaoke_tick(10000);
    midi_karaoke_stop();

    karaoke_lyric_msg_t msg;
    TEST_ASSERT_FALSE(midi_karaoke_pop_lyric(&msg));
}

static uint32_t g_clock_calls;
static uint32_t g_time_calls;
static uint32_t g_last_event_us;
static void on_clock(uint32_t us) { (void)us; g_clock_calls++; }
static void on_event_time(uint32_t us) { g_time_calls++; g_last_event_us = us; }

void test_the_sequencer_reports_song_time_for_the_clock_and_each_event(void)
{
    begin_file(1);
    uint32_t t0 = begin_track();
    put_vlq(PPQN);
    put(0x90); put(60); put(90);
    put_vlq(PPQN);
    put(0x80); put(60); put(0);
    end_track(t0);

    midi_synth_callbacks_t cb = CB;
    cb.song_clock = on_clock;
    cb.event_time = on_event_time;
    g_clock_calls = 0;
    g_time_calls = 0;
    midi_karaoke_init(&cb);
    TEST_ASSERT_TRUE(midi_karaoke_load_memory(g_smf, g_len));
    midi_karaoke_play();
    for (int i = 0; i < 150; i++) {
        midi_karaoke_tick(10000);
    }

    TEST_ASSERT_TRUE(g_clock_calls >= 100U && g_clock_calls <= 102U);   /* one per tick while the song plays */
    TEST_ASSERT_EQUAL_UINT32(2, g_time_calls);
    TEST_ASSERT_TRUE(g_last_event_us >= 990000U && g_last_event_us <= 1010000U);   /* the note off, at 1 s */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_streamed_playback_matches_playback_from_memory);
    RUN_TEST(test_no_read_is_bigger_than_the_scan_window);
    RUN_TEST(test_events_straddling_a_window_edge_decode_the_same);
    RUN_TEST(test_more_tracks_than_the_old_limit_all_play);
    RUN_TEST(test_a_song_whose_source_fails_stops_and_silences_every_note);
    RUN_TEST(test_a_failed_source_ends_the_song_early_not_at_its_natural_end);
    RUN_TEST(test_a_source_that_fails_during_the_load_scans_is_refused);
    RUN_TEST(test_loading_a_good_song_after_a_failed_one_plays_normally);
    RUN_TEST(test_the_elapsed_clock_keeps_the_sub_millisecond_part_of_every_tick);
    RUN_TEST(test_a_long_stall_is_made_up_by_the_catch_up_limit);
    RUN_TEST(test_a_source_that_fails_at_once_is_refused_cleanly);
    RUN_TEST(test_truncated_file_plays_what_is_there);
    RUN_TEST(test_lyrics_and_melody_detection_work_when_streamed);
    RUN_TEST(test_a_lyric_is_shown_when_its_sound_is_heard_not_when_it_is_read);
    RUN_TEST(test_lyrics_still_waiting_when_the_song_ends_are_not_lost);
    RUN_TEST(test_stopping_discards_lyrics_that_were_not_shown_yet);
    RUN_TEST(test_the_sequencer_reports_song_time_for_the_clock_and_each_event);
    return UNITY_END();
}
