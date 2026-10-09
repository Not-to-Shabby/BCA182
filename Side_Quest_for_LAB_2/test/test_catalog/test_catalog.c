/**
 * @file test_catalog.c
 * @brief Runs the real karaoke_catalog.c on the PC against a fake SD card (a folder).
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "../../src/song_path.c"
#include "../../src/song_scan.c"
#include "../../src/karaoke_catalog.c"
#include "host_sd.h"


/* Opens a song and reads its first bytes back through the streaming source, the way the
 * sequencer does. Returns the file length, or 0 when the song cannot be opened. */
static uint32_t open_and_peek(uint32_t index, uint8_t *head, uint32_t head_len)
{
    midi_source_t src;

    if (!karaoke_catalog_open_song(index, &src)) {
        return 0;
    }
    if (head_len > 0) {
        if (src.mem != NULL) {
            memcpy(head, src.mem, head_len);
        } else if (!src.read(src.ctx, 0, head, head_len)) {
            return 0;
        }
    }
    return src.size;
}

#define IDX_HEADER 16
#define IDX_RECORD 96

static void write_index(const char *rel_path, uint32_t declared, uint32_t stored,
                        const char *signature, uint16_t version, uint16_t record_size)
{
    static unsigned char buf[IDX_HEADER + 8 * IDX_RECORD];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, signature, 4);
    buf[4] = (unsigned char)(version & 0xFF);
    buf[5] = (unsigned char)(version >> 8);
    buf[6] = (unsigned char)(record_size & 0xFF);
    buf[7] = (unsigned char)(record_size >> 8);
    memcpy(buf + 8, &declared, 4);

    for (uint32_t i = 0; i < stored && i < 8; i++) {
        unsigned char *rec = buf + IDX_HEADER + i * IDX_RECORD;
        uint32_t code = 1000 * (i + 1) + 7;
        memcpy(rec, &code, 4);
        snprintf((char *)rec + 4, 40, "Title %u", (unsigned)i);
        snprintf((char *)rec + 44, 28, "Singer %u", (unsigned)i);
        snprintf((char *)rec + 72, 8, "OPM");
        snprintf((char *)rec + 80, 16, "%06u.mid", (unsigned)code);
    }
    host_write_bytes(rel_path, buf, IDX_HEADER + (size_t)stored * IDX_RECORD);
}

void setUp(void)
{
    snprintf(g_sd_root, sizeof(g_sd_root), "sd_mock_tmp");
    host_rmtree(g_sd_root);
    host_mkdir("");
    g_sd_mounted = true;
}

void tearDown(void)
{
    close_song();
    close_index();
    host_rmtree(g_sd_root);
}

void test_valid_index_is_used(void)
{
    write_index("songs.idx", 3, 3, "KIDX", 1, 96);
    host_mkdir("midi");
    host_mkdir("midi/001");
    host_write_file("midi/001/001007.mid", 500);

    karaoke_catalog_init();

    TEST_ASSERT_EQUAL(KARAOKE_SOURCE_INDEX, karaoke_catalog_get_source());
    TEST_ASSERT_EQUAL_UINT32(3, karaoke_catalog_get_total_songs());
    TEST_ASSERT_TRUE(karaoke_catalog_is_sd_active());

    song_entry_t s;
    TEST_ASSERT_TRUE(karaoke_catalog_get_song(0, &s));
    TEST_ASSERT_EQUAL_UINT32(1007, s.song_code);
    TEST_ASSERT_EQUAL_STRING("Title 0", s.title);
    TEST_ASSERT_EQUAL_STRING("Singer 0", s.singer);
    TEST_ASSERT_EQUAL_STRING("001007.mid", s.filename);

    uint8_t data[4] = {0};
    uint32_t len = 0;
    len = open_and_peek(0, data, 4);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_UINT32(500, len);
    TEST_ASSERT_EQUAL_MEMORY("MThd", data, 4);
}

void test_index_song_on_a_flat_card_still_loads(void)
{
    write_index("songs.idx", 1, 1, "KIDX", 1, 96);
    host_mkdir("midi");
    host_write_file("midi/001007.mid", 321);

    karaoke_catalog_init();

    uint8_t data[4] = {0};
    uint32_t len = 0;
    TEST_ASSERT_EQUAL(KARAOKE_SOURCE_INDEX, karaoke_catalog_get_source());
    len = open_and_peek(0, data, 4);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_UINT32(321, len);
}

void test_missing_index_falls_back_to_scanning(void)
{
    host_mkdir("midi");
    host_write_file("midi/027720 - Itchyworms - Beer.mid", 600);
    host_write_file("midi/036527 - John Lennon - #9 Dream.mid", 700);

    karaoke_catalog_init();

    TEST_ASSERT_EQUAL(KARAOKE_SOURCE_SCAN, karaoke_catalog_get_source());
    TEST_ASSERT_EQUAL_UINT32(2, karaoke_catalog_get_total_songs());
    TEST_ASSERT_TRUE(karaoke_catalog_is_sd_active());

    bool saw_beer = false;
    for (uint32_t i = 0; i < 2; i++) {
        song_entry_t s;
        TEST_ASSERT_TRUE(karaoke_catalog_get_song(i, &s));
        if (s.song_code == 27720) {
            saw_beer = true;
            TEST_ASSERT_EQUAL_STRING("Beer", s.title);
            TEST_ASSERT_EQUAL_STRING("Itchyworms", s.singer);
            TEST_ASSERT_EQUAL_STRING("SD", s.language);
            uint8_t data[4] = {0};
            uint32_t len = 0;
            len = open_and_peek(i, data, 4);
    TEST_ASSERT_TRUE(len > 0);
            TEST_ASSERT_EQUAL_UINT32(600, len);
            TEST_ASSERT_EQUAL_MEMORY("MThd", data, 4);
        }
    }
    TEST_ASSERT_TRUE(saw_beer);
}

void test_scan_finds_sharded_loose_and_plain_names(void)
{
    host_mkdir("midi");
    host_mkdir("midi/036");
    host_mkdir("midi/074");
    host_write_file("midi/036/036527.mid", 100);
    host_write_file("midi/074/074531.MID", 100);
    host_write_file("midi/loose.mid", 100);
    host_write_file("rootsong.mid", 100);

    karaoke_catalog_init();

    TEST_ASSERT_EQUAL(KARAOKE_SOURCE_SCAN, karaoke_catalog_get_source());
    TEST_ASSERT_EQUAL_UINT32(4, karaoke_catalog_get_total_songs());

    for (uint32_t i = 0; i < 4; i++) {
        uint8_t data[4] = {0};
        uint32_t len = 0;
        len = open_and_peek(i, data, 4);
        TEST_ASSERT_TRUE_MESSAGE(len > 0, "every listed song must load");
        TEST_ASSERT_EQUAL_UINT32(100, len);
    }
}

void test_scan_ignores_non_midi_hidden_and_unrelated_folders(void)
{
    host_mkdir("midi");
    host_mkdir("midi/a");
    host_mkdir("midi/a/b");
    host_mkdir("System Volume Information");
    host_mkdir("other");
    host_write_file("midi/good.mid", 100);
    host_write_file("midi/notes.txt", 100);
    host_write_file("midi/._good.mid", 100);
    host_write_file("midi/a/b/toodeep.mid", 100);
    host_write_file("System Volume Information/trash.mid", 100);
    host_write_file("other/elsewhere.mid", 100);

    karaoke_catalog_init();

    TEST_ASSERT_EQUAL(KARAOKE_SOURCE_SCAN, karaoke_catalog_get_source());
    TEST_ASSERT_EQUAL_UINT32(1, karaoke_catalog_get_total_songs());
    song_entry_t s;
    TEST_ASSERT_TRUE(karaoke_catalog_get_song(0, &s));
    TEST_ASSERT_EQUAL_STRING("good", s.title);
}

void test_wrong_signature_falls_back_to_scanning(void)
{
    write_index("songs.idx", 3, 3, "JUNK", 1, 96);
    host_mkdir("midi");
    host_write_file("midi/song.mid", 100);

    karaoke_catalog_init();

    TEST_ASSERT_EQUAL(KARAOKE_SOURCE_SCAN, karaoke_catalog_get_source());
    TEST_ASSERT_EQUAL_UINT32(1, karaoke_catalog_get_total_songs());
}

void test_wrong_version_or_record_size_falls_back_to_scanning(void)
{
    host_mkdir("midi");
    host_write_file("midi/song.mid", 100);

    write_index("songs.idx", 3, 3, "KIDX", 2, 96);
    karaoke_catalog_init();
    TEST_ASSERT_EQUAL(KARAOKE_SOURCE_SCAN, karaoke_catalog_get_source());

    write_index("songs.idx", 3, 3, "KIDX", 1, 64);
    karaoke_catalog_init();
    TEST_ASSERT_EQUAL(KARAOKE_SOURCE_SCAN, karaoke_catalog_get_source());
}

void test_header_only_index_falls_back_to_scanning(void)
{
    write_index("songs.idx", 5, 0, "KIDX", 1, 96);
    host_mkdir("midi");
    host_write_file("midi/song.mid", 100);

    karaoke_catalog_init();

    TEST_ASSERT_EQUAL(KARAOKE_SOURCE_SCAN, karaoke_catalog_get_source());
}

void test_declared_zero_songs_falls_back_to_scanning(void)
{
    write_index("songs.idx", 0, 2, "KIDX", 1, 96);
    host_mkdir("midi");
    host_write_file("midi/song.mid", 100);

    karaoke_catalog_init();

    TEST_ASSERT_EQUAL(KARAOKE_SOURCE_SCAN, karaoke_catalog_get_source());
}

void test_tiny_index_file_falls_back_to_scanning(void)
{
    static const unsigned char junk[7] = { 'K', 'I', 'D', 'X', 1, 0, 96 };
    host_write_bytes("songs.idx", junk, sizeof(junk));
    host_mkdir("midi");
    host_write_file("midi/song.mid", 100);

    karaoke_catalog_init();

    TEST_ASSERT_EQUAL(KARAOKE_SOURCE_SCAN, karaoke_catalog_get_source());
}

void test_truncated_index_keeps_the_records_that_exist(void)
{
    write_index("songs.idx", 100, 5, "KIDX", 1, 96);

    karaoke_catalog_init();

    TEST_ASSERT_EQUAL(KARAOKE_SOURCE_INDEX, karaoke_catalog_get_source());
    TEST_ASSERT_EQUAL_UINT32(5, karaoke_catalog_get_total_songs());
    song_entry_t s;
    TEST_ASSERT_TRUE(karaoke_catalog_get_song(4, &s));
    TEST_ASSERT_EQUAL_STRING("Title 4", s.title);
    TEST_ASSERT_FALSE(karaoke_catalog_get_song(5, &s));
}

void test_index_with_a_partial_last_record_ignores_it(void)
{
    static unsigned char extra[IDX_HEADER + 3 * IDX_RECORD + 40];
    write_index("songs.idx", 4, 3, "KIDX", 1, 96);
    FILE *f = fopen("sd_mock_tmp/songs.idx", "ab");
    memset(extra, 0, 40);
    fwrite(extra, 1, 40, f);
    fclose(f);

    karaoke_catalog_init();

    TEST_ASSERT_EQUAL(KARAOKE_SOURCE_INDEX, karaoke_catalog_get_source());
    TEST_ASSERT_EQUAL_UINT32(3, karaoke_catalog_get_total_songs());
}

void test_empty_card_uses_the_built_in_songs(void)
{
    karaoke_catalog_init();

    TEST_ASSERT_EQUAL(KARAOKE_SOURCE_ROM, karaoke_catalog_get_source());
    TEST_ASSERT_EQUAL_UINT32(3, karaoke_catalog_get_total_songs());
    TEST_ASSERT_FALSE(karaoke_catalog_is_sd_active());

    song_entry_t s;
    TEST_ASSERT_TRUE(karaoke_catalog_get_song(2, &s));
    TEST_ASSERT_EQUAL_STRING("Beer", s.title);

    uint8_t data[4] = {0};
    uint32_t len = 0;
    midi_source_t rom;
    TEST_ASSERT_TRUE(karaoke_catalog_open_song(2, &rom));
    TEST_ASSERT_EQUAL_PTR(EMBEDDED_MIDI_SONG_3, rom.mem);
    len = rom.size;
    TEST_ASSERT_EQUAL_UINT32(EMBEDDED_MIDI_SONG_3_len, len);
}

void test_card_with_only_other_files_uses_the_built_in_songs(void)
{
    host_write_file("readme.txt", 50);
    host_mkdir("midi");
    host_write_file("midi/cover.jpg", 50);

    karaoke_catalog_init();

    TEST_ASSERT_EQUAL(KARAOKE_SOURCE_ROM, karaoke_catalog_get_source());
}

void test_unmounted_card_uses_the_built_in_songs(void)
{
    write_index("songs.idx", 3, 3, "KIDX", 1, 96);
    host_mkdir("midi");
    host_write_file("midi/song.mid", 100);
    g_sd_mounted = false;

    karaoke_catalog_init();

    TEST_ASSERT_EQUAL(KARAOKE_SOURCE_ROM, karaoke_catalog_get_source());
    TEST_ASSERT_EQUAL_UINT32(3, karaoke_catalog_get_total_songs());
}

void test_scan_stops_at_the_list_limit(void)
{
    char rel[64];
    host_mkdir("midi");
    for (int i = 0; i < SONG_TABLE_MAX_SONGS + 40; i++) {
        snprintf(rel, sizeof(rel), "midi/s%03d.mid", i);
        host_write_file(rel, 10);
    }

    karaoke_catalog_init();

    TEST_ASSERT_EQUAL(KARAOKE_SOURCE_SCAN, karaoke_catalog_get_source());
    TEST_ASSERT_EQUAL_UINT32(SONG_TABLE_MAX_SONGS, karaoke_catalog_get_total_songs());
    TEST_ASSERT_TRUE(s_scan_full);
}

void test_a_file_larger_than_ram_streams_from_the_card(void)
{
    host_mkdir("midi");
    host_write_file("midi/big.mid", 400000);

    karaoke_catalog_init();

    midi_source_t src;
    TEST_ASSERT_TRUE(karaoke_catalog_open_song(0, &src));
    TEST_ASSERT_NULL(src.mem);
    TEST_ASSERT_EQUAL_UINT32(400000, src.size);

    uint8_t sector[512];
    TEST_ASSERT_TRUE(src.read(src.ctx, 0, sector, sizeof(sector)));
    TEST_ASSERT_EQUAL_MEMORY("MThd", sector, 4);
    TEST_ASSERT_TRUE(src.read(src.ctx, 399488, sector, 512));       /* last sector, after a seek */
    TEST_ASSERT_TRUE(src.read(src.ctx, 512, sector, sizeof(sector))); /* and back again */
    TEST_ASSERT_FALSE(src.read(src.ctx, 399900, sector, 512));      /* runs past the end */
}

void test_a_file_too_short_to_be_midi_is_refused(void)
{
    host_mkdir("midi");
    host_write_file("midi/tiny.mid", 10);

    karaoke_catalog_init();

    midi_source_t src;
    TEST_ASSERT_FALSE(karaoke_catalog_open_song(0, &src));
}

void test_opening_the_next_song_closes_the_previous_file(void)
{
    host_mkdir("midi");
    host_write_file("midi/a.mid", 2000);
    host_write_file("midi/b.mid", 3000);

    karaoke_catalog_init();

    uint8_t head[4];
    for (int round = 0; round < 50; round++) {
        TEST_ASSERT_TRUE(open_and_peek((uint32_t)(round & 1), head, 4) > 0);
    }
}

void test_song_deleted_after_listing_fails_cleanly(void)
{
    host_mkdir("midi");
    host_write_file("midi/gone.mid", 100);
    karaoke_catalog_init();
    TEST_ASSERT_EQUAL(KARAOKE_SOURCE_SCAN, karaoke_catalog_get_source());

    host_rmtree("sd_mock_tmp/midi");

    uint8_t data[4] = {0};
    uint32_t len = 0;
    TEST_ASSERT_EQUAL_UINT32(0, open_and_peek(0, data, 4));
}

void test_second_init_replaces_the_first_catalog(void)
{
    write_index("songs.idx", 3, 3, "KIDX", 1, 96);
    karaoke_catalog_init();
    TEST_ASSERT_EQUAL(KARAOKE_SOURCE_INDEX, karaoke_catalog_get_source());

    /* Windows cannot delete songs.idx while the catalog still has it open. */
    close_index();
    host_rmtree(g_sd_root);
    host_mkdir("");
    host_mkdir("midi");
    host_write_file("midi/a.mid", 100);
    host_write_file("midi/b.mid", 100);
    karaoke_catalog_init();
    TEST_ASSERT_EQUAL(KARAOKE_SOURCE_SCAN, karaoke_catalog_get_source());
    TEST_ASSERT_EQUAL_UINT32(2, karaoke_catalog_get_total_songs());

    host_rmtree(g_sd_root);
    host_mkdir("");
    karaoke_catalog_init();
    TEST_ASSERT_EQUAL(KARAOKE_SOURCE_ROM, karaoke_catalog_get_source());
    TEST_ASSERT_EQUAL_UINT32(3, karaoke_catalog_get_total_songs());
}

void test_out_of_range_requests_fail(void)
{
    host_mkdir("midi");
    host_write_file("midi/a.mid", 100);
    karaoke_catalog_init();

    song_entry_t s;
    uint8_t data[4] = {0};
    uint32_t len = 0;
    TEST_ASSERT_FALSE(karaoke_catalog_get_song(1, &s));
    TEST_ASSERT_EQUAL_UINT32(0, open_and_peek(1, data, 4));
    TEST_ASSERT_FALSE(karaoke_catalog_get_song(0, NULL));
}

void test_find_by_code_in_index_and_rom(void)
{
    /* 1. ROM Mode */
    karaoke_catalog_init();
    uint32_t idx = 999;
    TEST_ASSERT_TRUE(karaoke_catalog_find_by_code(27720, &idx));
    TEST_ASSERT_EQUAL_UINT32(2, idx);
    TEST_ASSERT_FALSE(karaoke_catalog_find_by_code(99999, &idx));

    /* 2. Index Mode */
    write_index("songs.idx", 3, 3, "KIDX", 1, 96);
    karaoke_catalog_init();
    TEST_ASSERT_TRUE(karaoke_catalog_find_by_code(1007, &idx));
    TEST_ASSERT_EQUAL_UINT32(0, idx);
    TEST_ASSERT_TRUE(karaoke_catalog_find_by_code(3007, &idx));
    TEST_ASSERT_EQUAL_UINT32(2, idx);
    TEST_ASSERT_FALSE(karaoke_catalog_find_by_code(5555, &idx));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_valid_index_is_used);
    RUN_TEST(test_index_song_on_a_flat_card_still_loads);
    RUN_TEST(test_missing_index_falls_back_to_scanning);
    RUN_TEST(test_scan_finds_sharded_loose_and_plain_names);
    RUN_TEST(test_scan_ignores_non_midi_hidden_and_unrelated_folders);
    RUN_TEST(test_wrong_signature_falls_back_to_scanning);
    RUN_TEST(test_wrong_version_or_record_size_falls_back_to_scanning);
    RUN_TEST(test_header_only_index_falls_back_to_scanning);
    RUN_TEST(test_declared_zero_songs_falls_back_to_scanning);
    RUN_TEST(test_tiny_index_file_falls_back_to_scanning);
    RUN_TEST(test_truncated_index_keeps_the_records_that_exist);
    RUN_TEST(test_index_with_a_partial_last_record_ignores_it);
    RUN_TEST(test_empty_card_uses_the_built_in_songs);
    RUN_TEST(test_card_with_only_other_files_uses_the_built_in_songs);
    RUN_TEST(test_unmounted_card_uses_the_built_in_songs);
    RUN_TEST(test_scan_stops_at_the_list_limit);
    RUN_TEST(test_a_file_larger_than_ram_streams_from_the_card);
    RUN_TEST(test_a_file_too_short_to_be_midi_is_refused);
    RUN_TEST(test_opening_the_next_song_closes_the_previous_file);
    RUN_TEST(test_song_deleted_after_listing_fails_cleanly);
    RUN_TEST(test_second_init_replaces_the_first_catalog);
    RUN_TEST(test_out_of_range_requests_fail);
    RUN_TEST(test_find_by_code_in_index_and_rom);
    return UNITY_END();
}
