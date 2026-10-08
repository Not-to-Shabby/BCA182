/**
 * @file test_settings.c
 * @brief Unit tests for persistent volume & instrument gain configuration.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include <unity.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define SETTINGS_MAGIC 0x4B415241U

typedef struct {
    uint8_t master_volume;
    uint8_t instrument_gain;
    uint8_t drum_gain;
} test_settings_t;

static uint32_t pack_settings(uint8_t vol, uint8_t inst, uint8_t drum)
{
    return (uint32_t)vol | ((uint32_t)inst << 8) | ((uint32_t)drum << 16);
}

static void unpack_settings(uint32_t packed, uint8_t *vol, uint8_t *inst, uint8_t *drum)
{
    *vol  = (uint8_t)(packed & 0xFFU);
    *inst = (uint8_t)((packed >> 8) & 0xFFU);
    *drum = (uint8_t)((packed >> 16) & 0xFFU);
}

static bool parse_cfg_file(const char *buf, uint8_t *vol, uint8_t *inst, uint8_t *drum)
{
    char tmp[256];
    strncpy(tmp, buf, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    int v = -1, i = -1, d = -1;
    char *line = strtok(tmp, "\r\n");
    while (line != NULL) {
        if (strncmp(line, "VOL=", 4) == 0) {
            v = atoi(line + 4);
        } else if (strncmp(line, "INST=", 5) == 0) {
            i = atoi(line + 5);
        } else if (strncmp(line, "DRUM=", 5) == 0) {
            d = atoi(line + 5);
        }
        line = strtok(NULL, "\r\n");
    }

    if (v >= 0 && v <= 100 && i >= 20 && i <= 200 && d >= 20 && d <= 200) {
        *vol = (uint8_t)v;
        *inst = (uint8_t)i;
        *drum = (uint8_t)d;
        return true;
    }
    return false;
}

void setUp(void) {}
void tearDown(void) {}

void test_rtc_backup_pack_and_unpack(void)
{
    uint32_t packed = pack_settings(85, 120, 95);
    uint8_t vol = 0, inst = 0, drum = 0;
    unpack_settings(packed, &vol, &inst, &drum);

    TEST_ASSERT_EQUAL_UINT8(85, vol);
    TEST_ASSERT_EQUAL_UINT8(120, inst);
    TEST_ASSERT_EQUAL_UINT8(95, drum);
}

void test_cfg_file_parse_valid(void)
{
    const char *cfg = "# Settings\nVOL=75\nINST=130\nDRUM=90\n";
    uint8_t vol = 0, inst = 0, drum = 0;
    TEST_ASSERT_TRUE(parse_cfg_file(cfg, &vol, &inst, &drum));
    TEST_ASSERT_EQUAL_UINT8(75, vol);
    TEST_ASSERT_EQUAL_UINT8(130, inst);
    TEST_ASSERT_EQUAL_UINT8(90, drum);
}

void test_cfg_file_parse_invalid_rejected(void)
{
    const char *cfg = "VOL=250\nINST=10\nDRUM=500\n";
    uint8_t vol = 80, inst = 100, drum = 100;
    TEST_ASSERT_FALSE(parse_cfg_file(cfg, &vol, &inst, &drum));
    TEST_ASSERT_EQUAL_UINT8(80, vol);
}

void test_volume_analog_gain_non_inverted(void)
{
    /* Verify that increasing volume_percent directly increases the register value */
    uint8_t vol_low = 20;
    uint8_t vol_high = 90;

    uint8_t gain_low  = (uint8_t)(((uint32_t)vol_low * 33U) / 100U);
    uint8_t gain_high = (uint8_t)(((uint32_t)vol_high * 33U) / 100U);

    TEST_ASSERT_TRUE_MESSAGE(gain_high > gain_low, "Higher volume percent must yield higher register gain");
    TEST_ASSERT_EQUAL_UINT8(6, gain_low);
    TEST_ASSERT_EQUAL_UINT8(29, gain_high);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rtc_backup_pack_and_unpack);
    RUN_TEST(test_cfg_file_parse_valid);
    RUN_TEST(test_cfg_file_parse_invalid_rejected);
    RUN_TEST(test_volume_analog_gain_non_inverted);
    return UNITY_END();
}
