/**
 * @file sensme.c
 * @brief Sony SensMe acoustic mood classification engine and 8MB SPI NOR Flash database manager.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include "sensme.h"
#include "w25q64.h"
#include "sd_card_reader.h"
#include "lcd_st7789.h"

#include <string.h>
#include <stdio.h>
#include <zephyr/sys/printk.h>

#if defined(__arm__)
#include <zephyr/kernel.h>
#include <ff.h>
#endif

typedef struct __attribute__((packed)) {
    uint32_t magic;         /* 0x534E4553 ("SENS" in little-endian) */
    uint16_t version;       /* 1 */
    uint16_t entry_size;    /* 4 bytes */
    uint32_t song_count;    /* Total songs (e.g. 47,998) */
    uint32_t data_crc32;    /* CRC32 of entry table */
    uint16_t ch_counts[16]; /* Song count per SensMe channel */
    uint8_t  reserved[16];  /* Zero padding to 64 bytes */
} sensme_header_t;

_Static_assert(sizeof(sensme_header_t) == SENSME_HEADER_BYTES, "SensMe header must be exactly 64 bytes");

static bool s_ready = false;
static uint32_t s_song_count = 0;
static uint16_t s_ch_counts[16];
static uint8_t s_active_channel = SENSME_CH_ALL;

static const char *const CHANNEL_NAMES[SENSME_CH_COUNT] = {
    "All Songs",
    "Morning",
    "Daytime",
    "Evening",
    "Night",
    "Relax",
    "Mellow",
    "Upbeat",
    "Emotional",
    "Lounge",
    "Dance",
    "Extreme",
    "Acoustic"
};

static const uint16_t CHANNEL_COLORS[SENSME_CH_COUNT] = {
    LCD_COLOR_WHITE,        /* 0: All Songs */
    LCD_COLOR_YELLOW,       /* 1: Morning (Warm Sun) */
    LCD_COLOR_CYAN,         /* 2: Daytime (Sky Blue) */
    LCD_COLOR_ORANGE,       /* 3: Evening (Sunset) */
    LCD_COLOR_BLUE,         /* 4: Night (Deep Blue) */
    LCD_COLOR_GREEN,        /* 5: Relax (Emerald Green) */
    0x07E8,                 /* 6: Mellow (Mint Green) */
    0xFFE0,                 /* 7: Upbeat (Bright Gold) */
    LCD_COLOR_PURPLE,       /* 8: Emotional (Deep Violet) */
    LCD_COLOR_OLIVE,        /* 9: Lounge (Smooth Olive) */
    LCD_COLOR_MAGENTA,      /* 10: Dance (Hot Pink / Magenta) */
    LCD_COLOR_RED,          /* 11: Extreme (Crimson Fire) */
    LCD_COLOR_LIGHTGREY     /* 12: Acoustic (Natural Timber) */
};

#if defined(__arm__)
static uint32_t compute_crc32(uint32_t crc, const uint8_t *buf, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (-(int32_t)(crc & 1U)));
        }
    }
    return ~crc;
}

static bool sync_from_sd_card(const sensme_header_t *nor_hdr)
{
    if (!sd_card_is_mounted()) {
        return false;
    }

    FIL f;
    sd_card_lock();
    if (f_open(&f, SENSME_SD_PATH, FA_READ) != FR_OK) {
        sd_card_unlock();
        return false;
    }

    sensme_header_t sd_hdr;
    UINT br = 0;
    FRESULT fr = f_read(&f, &sd_hdr, sizeof(sd_hdr), &br);
    if (fr != FR_OK || br != sizeof(sd_hdr) || sd_hdr.magic != SENSME_MAGIC) {
        f_close(&f);
        sd_card_unlock();
        printk("[SensMe] Invalid or corrupt header in %s\n", SENSME_SD_PATH);
        return false;
    }

    /* Check if NOR Flash already has the exact same database */
    if (nor_hdr != NULL && nor_hdr->magic == SENSME_MAGIC &&
        nor_hdr->version == sd_hdr.version &&
        nor_hdr->song_count == sd_hdr.song_count &&
        nor_hdr->data_crc32 == sd_hdr.data_crc32) {
        f_close(&f);
        sd_card_unlock();
        printk("[SensMe] NOR Flash is already up-to-date with %s (%u songs, CRC 0x%08X)\n",
               SENSME_SD_PATH, (unsigned)sd_hdr.song_count, (unsigned)sd_hdr.data_crc32);
        return true;
    }

    printk("[SensMe] Flashing SensMe database to W25Q64 NOR Flash (%u songs, %u KB)...\n",
           (unsigned)sd_hdr.song_count, (unsigned)((sizeof(sd_hdr) + sd_hdr.song_count * 4U) / 1024U));

    uint32_t total_bytes = sizeof(sd_hdr) + sd_hdr.song_count * SENSME_ENTRY_BYTES;
    uint32_t sectors = (total_bytes + W25Q64_SECTOR_SIZE - 1U) / W25Q64_SECTOR_SIZE;

    /* 1. Erase required 4KB sectors */
    for (uint32_t s = 0; s < sectors; s++) {
        w25q64_erase_sector(SENSME_FLASH_ADDR + s * W25Q64_SECTOR_SIZE);
    }

    /* 2. Program Header */
    w25q64_write(SENSME_FLASH_ADDR, &sd_hdr, sizeof(sd_hdr));

    /* 3. Stream table from SD to NOR in 2 KB buffer chunks */
    static uint8_t s_sync_buf[2048];
    uint32_t written = sizeof(sd_hdr);
    uint32_t remaining = sd_hdr.song_count * SENSME_ENTRY_BYTES;
    uint32_t calc_crc = 0;

    while (remaining > 0) {
        uint32_t chunk = (remaining > sizeof(s_sync_buf)) ? (uint32_t)sizeof(s_sync_buf) : remaining;
        fr = f_read(&f, s_sync_buf, chunk, &br);
        if (fr != FR_OK || br != chunk) {
            printk("[SensMe] Read error from %s at offset %u\n", SENSME_SD_PATH, (unsigned)written);
            f_close(&f);
            sd_card_unlock();
            return false;
        }

        calc_crc = compute_crc32(calc_crc, s_sync_buf, chunk);
        w25q64_write(SENSME_FLASH_ADDR + written, s_sync_buf, chunk);

        written += chunk;
        remaining -= chunk;
    }

    f_close(&f);
    sd_card_unlock();

    if (calc_crc != sd_hdr.data_crc32) {
        printk("[SensMe] CRC mismatch: computed 0x%08X vs expected 0x%08X\n", (unsigned)calc_crc, (unsigned)sd_hdr.data_crc32);
        return false;
    }

    printk("[SensMe] Flashing successful! %u KB written and verified (CRC 0x%08X)\n",
           (unsigned)(total_bytes / 1024U), (unsigned)calc_crc);
    return true;
}
#endif

bool sensme_init(void)
{
    s_ready = false;
    s_song_count = 0;
    memset(s_ch_counts, 0, sizeof(s_ch_counts));

    if (!w25q64_init()) {
        printk("[SensMe] W25Q64 SPI NOR Flash not available\n");
        return false;
    }

    sensme_header_t hdr;
    w25q64_read(SENSME_FLASH_ADDR, &hdr, sizeof(hdr));

#if defined(__arm__)
    /* Check for SD card sync update */
    bool sync_attempted = sync_from_sd_card(&hdr);
    if (sync_attempted) {
        /* Re-read updated header from NOR */
        w25q64_read(SENSME_FLASH_ADDR, &hdr, sizeof(hdr));
    }
#endif

    if (hdr.magic == SENSME_MAGIC && hdr.version == SENSME_VERSION && hdr.song_count > 0) {
        s_song_count = hdr.song_count;
        memcpy(s_ch_counts, hdr.ch_counts, sizeof(s_ch_counts));
        s_ready = true;
        printk("[SensMe] Ready: %u songs categorized across 12 mood channels (W25Q64 NOR Flash)\n",
               (unsigned)s_song_count);
        return true;
    }

    printk("[SensMe] No valid database in NOR Flash (Magic=0x%08X)\n", (unsigned)hdr.magic);
    return false;
}

bool sensme_is_ready(void)
{
    return s_ready;
}

uint32_t sensme_get_song_count(void)
{
    return s_song_count;
}

bool sensme_get_song_data(uint32_t catalog_index, sensme_data_t *out)
{
    if (!out) {
        return false;
    }
    if (s_ready && catalog_index < s_song_count) {
        uint32_t addr = SENSME_FLASH_ADDR + SENSME_HEADER_BYTES + catalog_index * SENSME_ENTRY_BYTES;
        w25q64_read(addr, out, sizeof(sensme_data_t));
        return true;
    }

    /* Fallback for demonstration / embedded songs before NOR Flash is synced */
    if (catalog_index == 34393U) {
        /* Rasputin (#61845) */
        out->valence = 94;
        out->energy = 211;
        out->channel = SENSME_CH_EXTREME;
        out->bpm = 127;
        return true;
    } else if (catalog_index == 0x66666U) {
        /* DOOM E1M1 (#66666) */
        out->valence = 85;
        out->energy = 245;
        out->channel = SENSME_CH_EXTREME;
        out->bpm = 160;
        return true;
    } else if (catalog_index == 0U) {
        /* #9 Dream (#36527) */
        out->valence = 145;
        out->energy = 130;
        out->channel = SENSME_CH_DAYTIME;
        out->bpm = 114;
        return true;
    }

    out->valence = 140;
    out->energy = 130;
    out->channel = SENSME_CH_DAYTIME;
    out->bpm = 120;
    return false;
}

const char *sensme_channel_name(uint8_t channel_id)
{
    if (channel_id < SENSME_CH_COUNT) {
        return CHANNEL_NAMES[channel_id];
    }
    return "Unknown";
}

uint16_t sensme_channel_color(uint8_t channel_id)
{
    if (channel_id < SENSME_CH_COUNT) {
        return CHANNEL_COLORS[channel_id];
    }
    return LCD_COLOR_WHITE;
}

uint16_t sensme_get_channel_song_count(uint8_t channel_id)
{
    if (channel_id < 16) {
        return s_ch_counts[channel_id];
    }
    return 0;
}

void sensme_set_active_channel(uint8_t channel)
{
    if (channel < SENSME_CH_COUNT) {
        s_active_channel = channel;
    }
}

uint8_t sensme_get_active_channel(void)
{
    return s_active_channel;
}

bool sensme_matches_active_channel(uint32_t catalog_index)
{
    if (s_active_channel == SENSME_CH_ALL) {
        return true;
    }
    sensme_data_t data;
    if (sensme_get_song_data(catalog_index, &data)) {
        return (data.channel == s_active_channel);
    }
    return true;
}
