/**
 * @file test_sensme.c
 * @brief Host unit tests for W25Q64 SPI NOR Flash driver and SensMe mood classification engine.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include <unity.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../src/w25q64.c"
#include "../../src/sensme.c"
#include "../../src/random_play.c"

void setUp(void)
{
    w25q64_init();
}

void tearDown(void)
{
}

void test_w25q64_mock_jedec_id_and_read_write(void)
{
    TEST_ASSERT_TRUE(w25q64_init());
    TEST_ASSERT_TRUE(w25q64_is_available());
    TEST_ASSERT_EQUAL_HEX32(W25Q64_JEDEC_ID, w25q64_read_jedec_id());

    /* Sector erase */
    w25q64_erase_sector(0);
    uint8_t buf[16];
    w25q64_read(0, buf, sizeof(buf));
    for (size_t i = 0; i < sizeof(buf); i++) {
        TEST_ASSERT_EQUAL_UINT8(0xFF, buf[i]);
    }

    /* Write and read back */
    const char *msg = "W25Q64 SensMe OK";
    w25q64_write(128, msg, strlen(msg) + 1);
    char readback[32];
    w25q64_read(128, readback, sizeof(readback));
    TEST_ASSERT_EQUAL_STRING(msg, readback);
}

void test_sensme_database_initialization_and_lookup(void)
{
    /* Program a mock database into W25Q64 */
    sensme_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = SENSME_MAGIC;
    hdr.version = SENSME_VERSION;
    hdr.entry_size = SENSME_ENTRY_BYTES;
    hdr.song_count = 3;
    hdr.ch_counts[SENSME_CH_RELAX] = 1;
    hdr.ch_counts[SENSME_CH_DANCE] = 1;
    hdr.ch_counts[SENSME_CH_EXTREME] = 1;

    sensme_data_t entries[3] = {
        { .valence = 180, .energy = 50,  .channel = SENSME_CH_RELAX,   .bpm = 72 },
        { .valence = 160, .energy = 180, .channel = SENSME_CH_DANCE,   .bpm = 128 },
        { .valence = 94,  .energy = 215, .channel = SENSME_CH_EXTREME, .bpm = 135 }
    };

    w25q64_erase_sector(SENSME_FLASH_ADDR);
    w25q64_write(SENSME_FLASH_ADDR, &hdr, sizeof(hdr));
    w25q64_write(SENSME_FLASH_ADDR + SENSME_HEADER_BYTES, entries, sizeof(entries));

    TEST_ASSERT_TRUE(sensme_init());
    TEST_ASSERT_TRUE(sensme_is_ready());
    TEST_ASSERT_EQUAL_UINT32(3, sensme_get_song_count());
    TEST_ASSERT_EQUAL_UINT16(1, sensme_get_channel_song_count(SENSME_CH_RELAX));
    TEST_ASSERT_EQUAL_UINT16(1, sensme_get_channel_song_count(SENSME_CH_DANCE));
    TEST_ASSERT_EQUAL_UINT16(1, sensme_get_channel_song_count(SENSME_CH_EXTREME));

    /* Verify song lookups */
    sensme_data_t d;
    TEST_ASSERT_TRUE(sensme_get_song_data(0, &d));
    TEST_ASSERT_EQUAL_UINT8(180, d.valence);
    TEST_ASSERT_EQUAL_UINT8(50, d.energy);
    TEST_ASSERT_EQUAL_UINT8(SENSME_CH_RELAX, d.channel);
    TEST_ASSERT_EQUAL_UINT8(72, d.bpm);

    TEST_ASSERT_TRUE(sensme_get_song_data(2, &d));
    TEST_ASSERT_EQUAL_UINT8(94, d.valence);
    TEST_ASSERT_EQUAL_UINT8(215, d.energy);
    TEST_ASSERT_EQUAL_UINT8(SENSME_CH_EXTREME, d.channel);
    TEST_ASSERT_EQUAL_UINT8(135, d.bpm);

    /* Out of bounds lookup */
    TEST_ASSERT_FALSE(sensme_get_song_data(99, &d));
}

void test_sensme_channel_names_and_colors(void)
{
    TEST_ASSERT_EQUAL_STRING("All Songs", sensme_channel_name(SENSME_CH_ALL));
    TEST_ASSERT_EQUAL_STRING("Morning", sensme_channel_name(SENSME_CH_MORNING));
    TEST_ASSERT_EQUAL_STRING("Dance", sensme_channel_name(SENSME_CH_DANCE));
    TEST_ASSERT_EQUAL_STRING("Extreme", sensme_channel_name(SENSME_CH_EXTREME));
    TEST_ASSERT_EQUAL_STRING("Acoustic", sensme_channel_name(SENSME_CH_ACOUSTIC));

    TEST_ASSERT_EQUAL_HEX16(LCD_COLOR_WHITE, sensme_channel_color(SENSME_CH_ALL));
    TEST_ASSERT_EQUAL_HEX16(LCD_COLOR_MAGENTA, sensme_channel_color(SENSME_CH_DANCE));
    TEST_ASSERT_EQUAL_HEX16(LCD_COLOR_RED, sensme_channel_color(SENSME_CH_EXTREME));
}

void test_sensme_mood_channel_filtering_and_random_play(void)
{
    /* Set active channel filter */
    sensme_set_active_channel(SENSME_CH_DANCE);
    TEST_ASSERT_EQUAL_UINT8(SENSME_CH_DANCE, sensme_get_active_channel());

    /* Song 0 is Relax, Song 1 is Dance, Song 2 is Extreme */
    TEST_ASSERT_FALSE(sensme_matches_active_channel(0));
    TEST_ASSERT_TRUE(sensme_matches_active_channel(1));
    TEST_ASSERT_FALSE(sensme_matches_active_channel(2));

    /* Random play with filter: should only select song 1 */
    random_play_t rp;
    random_play_reset(&rp);
    for (uint32_t seed = 0; seed < 10; seed++) {
        uint32_t pick = random_play_pick_filtered(&rp, 3, seed, sensme_matches_active_channel);
        TEST_ASSERT_EQUAL_UINT32(1, pick);
    }

    /* Reset to ALL: should allow any song */
    sensme_set_active_channel(SENSME_CH_ALL);
    TEST_ASSERT_TRUE(sensme_matches_active_channel(0));
    TEST_ASSERT_TRUE(sensme_matches_active_channel(1));
    TEST_ASSERT_TRUE(sensme_matches_active_channel(2));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_w25q64_mock_jedec_id_and_read_write);
    RUN_TEST(test_sensme_database_initialization_and_lookup);
    RUN_TEST(test_sensme_channel_names_and_colors);
    RUN_TEST(test_sensme_mood_channel_filtering_and_random_play);
    return UNITY_END();
}
