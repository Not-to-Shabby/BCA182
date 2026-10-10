/**
 * @file karaoke_catalog.c
 * @brief Song catalog: songs.idx on the SD card, a scan of the card's MIDI
 *        files when the index cannot be used, and built-in ROM songs as the
 *        last resort.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include "karaoke_catalog.h"
#include "dtcm.h"
#include "karaoke_embedded_songs.h"
#include "sd_card_reader.h"
#include "song_path.h"
#include "song_scan.h"
#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <ff.h>

#define SD_ROOT                 "SD:/"
#define INDEX_HEADER_BYTES      16U
#define INDEX_RECORD_BYTES      96U
#define INDEX_VERSION           1U
#define SCAN_PATH_BYTES         128

typedef enum {
    READ_OK,
    READ_NO_FILE,
    READ_FAILED
} read_result_t;

static karaoke_catalog_source_t s_source = KARAOKE_SOURCE_ROM;
static uint32_t s_total_songs = 0;
static FIL s_idx_file;
static bool s_idx_open = false;
static char s_idx_path[SCAN_PATH_BYTES];
static FIL s_song_file;
static bool s_song_open = false;
static char s_song_path[SCAN_PATH_BYTES + 4];
static uint32_t s_song_pos = 0;

/* A transfer that fails can leave the SDIO driver stuck; each failed access restarts the driver,
 * reopens the file (FatFs keeps the error on the file object) and tries again. */
#define SD_ACCESS_ATTEMPTS  3U
static DTCM_BSS song_table_t s_scan_table;
static bool s_scan_full = false;

/* Built-in ROM Fallback Songs */
static const song_entry_t ROM_SONGS[] = {
    {
        .song_code = 2106,
        .title = "(Everything I Do) I Do It for You",
        .singer = "Bryan Adams",
        .language = "ENG",
        .filename = "002106.mid"
    },
    {
        .song_code = 36527,
        .title = "#9 Dream",
        .singer = "John Lennon",
        .language = "OPM",
        .filename = "036527.mid"
    },
    {
        .song_code = 27720,
        .title = "Beer",
        .singer = "Itchyworms",
        .language = "OPM",
        .filename = "027720.mid"
    }
};
#define ROM_SONG_COUNT (sizeof(ROM_SONGS) / sizeof(ROM_SONGS[0]))

/* MIDI data for each ROM_SONGS[] entry, in the same order. */
static const struct {
    const uint8_t *data;
    uint32_t length;
} ROM_MIDI[ROM_SONG_COUNT] = {
    { EMBEDDED_MIDI_SONG_1, EMBEDDED_MIDI_SONG_1_len },
    { EMBEDDED_MIDI_SONG_2, EMBEDDED_MIDI_SONG_2_len },
    { EMBEDDED_MIDI_SONG_3, EMBEDDED_MIDI_SONG_3_len },
};

static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return le16(p) | (le16(p + 2) << 16);
}

static bool join_path(char *out, size_t out_size, const char *a, const char *b)
{
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);

    if (len_a + len_b + 1U > out_size) {
        return false;
    }
    memcpy(out, a, len_a);
    memcpy(out + len_a, b, len_b + 1U);
    return true;
}

static void close_index(void)
{
    if (s_idx_open) {
        sd_card_lock();
        f_close(&s_idx_file);
        sd_card_unlock();
        s_idx_open = false;
    }
}

static void close_song(void)
{
    if (s_song_open) {
        sd_card_lock();
        f_close(&s_song_file);
        sd_card_unlock();
        s_song_open = false;
    }
}

/* Opens songs.idx and returns how many of its records can be used, or 0 if the
 * file is missing or cannot be trusted (the file is then closed). */
static uint32_t open_index(void)
{
    static const char * const CANDIDATE_INDEX_PATHS[] = {
        SD_ROOT "songs.idx",
        SD_ROOT "SONGS.IDX",
        SD_ROOT "midi/songs.idx",
        SD_ROOT "midi/SONGS.IDX",
        SD_ROOT "songs.idx.txt",
        SD_ROOT "SONGS.IDX.TXT",
        SD_ROOT "KARAOKE_SD_CARD_SHARDED/songs.idx",
        SD_ROOT "KARAOKE_SD_CARD/songs.idx"
    };

    FRESULT fr = FR_NO_FILE;
    const char *found_path = NULL;

    for (size_t i = 0; i < sizeof(CANDIDATE_INDEX_PATHS) / sizeof(CANDIDATE_INDEX_PATHS[0]); i++) {
        fr = f_open(&s_idx_file, CANDIDATE_INDEX_PATHS[i], FA_READ);
        if (fr == FR_OK) {
            found_path = CANDIDATE_INDEX_PATHS[i];
            snprintf(s_idx_path, sizeof(s_idx_path), "%s", found_path);
            s_idx_open = true;
            break;
        }
    }

    if (!s_idx_open) {
        printk("[Catalog] No songs.idx on the card (FatFs error %d)\n", (int)fr);
        printk("[Catalog] Files present in " SD_ROOT ":\n");
        DIR d;
        FILINFO fno;
        if (f_opendir(&d, SD_ROOT) == FR_OK) {
            while (f_readdir(&d, &fno) == FR_OK && fno.fname[0] != '\0') {
                printk("  [%c] %s\n", (fno.fattrib & AM_DIR) ? 'D' : 'F', fno.fname);
            }
            f_closedir(&d);
        }
        return 0;
    }

    printk("[Catalog] Opened index at: %s\n", found_path);

    uint8_t hdr[INDEX_HEADER_BYTES];
    UINT br = 0;
    fr = f_read(&s_idx_file, hdr, sizeof(hdr), &br);

    const char *problem = NULL;
    uint32_t usable = 0;

    if (fr != FR_OK || br != sizeof(hdr)) {
        problem = "header unreadable";
    } else if (memcmp(hdr, "KIDX", 4) != 0) {
        problem = "bad signature";
    } else if (le16(hdr + 4) != INDEX_VERSION) {
        problem = "unsupported version";
    } else if (le16(hdr + 6) != INDEX_RECORD_BYTES) {
        problem = "unexpected record size";
    } else {
        uint32_t declared = le32(hdr + 8);
        FSIZE_t size = f_size(&s_idx_file);
        uint32_t stored = (size > INDEX_HEADER_BYTES)
                              ? (uint32_t)((size - INDEX_HEADER_BYTES) / INDEX_RECORD_BYTES)
                              : 0U;

        usable = declared;
        if (stored < declared) {
            usable = stored;
            if (usable > 0) {
                printk("[Catalog] songs.idx is truncated: %u of %u records present\n",
                       (unsigned)usable, (unsigned)declared);
            }
        }
        if (usable == 0) {
            problem = "no song records";
        }
    }

    if (problem != NULL) {
        printk("[Catalog] songs.idx unusable: %s\n", problem);
        f_close(&s_idx_file);
        s_idx_open = false;
        return 0;
    }
    return usable;
}

/* Adds the MIDI files in one folder to the scan table. Subfolders are entered
 * only while @p depth is above zero; folders such as "System Volume Information"
 * are never reached because the root is scanned with depth 0. */
static void scan_folder(const char *fs_path, const char *rel_prefix, int depth)
{
    DIR dir;
    FILINFO info;

    if (f_opendir(&dir, fs_path) != FR_OK) {
        return;
    }

    while (!s_scan_full && f_readdir(&dir, &info) == FR_OK && info.fname[0] != '\0') {
        if (info.fname[0] == '.' || (info.fattrib & (AM_HID | AM_SYS)) != 0) {
            continue;
        }

        char rel[SCAN_PATH_BYTES];
        if (!join_path(rel, sizeof(rel), rel_prefix, info.fname)) {
            continue;
        }

        if ((info.fattrib & AM_DIR) != 0) {
            if (depth > 0) {
                char sub_fs[SCAN_PATH_BYTES + 4];
                char sub_prefix[SCAN_PATH_BYTES];
                if (join_path(sub_fs, sizeof(sub_fs), SD_ROOT, rel) &&
                    join_path(sub_prefix, sizeof(sub_prefix), rel, "/")) {
                    scan_folder(sub_fs, sub_prefix, depth - 1);
                }
            }
        } else if (song_name_is_midi(info.fname)) {
            if (!song_table_add(&s_scan_table, rel)) {
                s_scan_full = true;
            }
        }
    }

    f_closedir(&dir);
}

void karaoke_catalog_init(void)
{
    close_index();
    s_source = KARAOKE_SOURCE_ROM;
    s_total_songs = ROM_SONG_COUNT;
    song_table_clear(&s_scan_table);
    s_scan_full = false;

    if (!sd_card_is_mounted()) {
        printk("[Catalog] SD card not mounted; using the %u built-in songs.\n",
               (unsigned)ROM_SONG_COUNT);
        return;
    }

    close_song();
    printk("[Catalog] Probing SD:/songs.idx...\n");
    sd_card_lock();
    uint32_t indexed = open_index();
    sd_card_unlock();
    if (indexed > 0) {
        s_source = KARAOKE_SOURCE_INDEX;
        s_total_songs = indexed;
        printk("[Catalog] Found MicroSD index with %u songs!\n", (unsigned)indexed);
        return;
    }

    printk("[Catalog] Scanning the SD card for MIDI files...\n");
    sd_card_lock();
    scan_folder(SD_ROOT "midi", "midi/", 1);
    scan_folder(SD_ROOT, "", 0);
    sd_card_unlock();

    if (s_scan_table.count > 0) {
        s_source = KARAOKE_SOURCE_SCAN;
        s_total_songs = s_scan_table.count;
        printk("[Catalog] Found %u MIDI files by scanning%s.\n",
               (unsigned)s_total_songs,
               s_scan_full ? " (list is full, more files exist on the card)" : "");
        return;
    }

    printk("[Catalog] No MIDI files on the card; using the %u built-in songs.\n",
           (unsigned)ROM_SONG_COUNT);
}

karaoke_catalog_source_t karaoke_catalog_get_source(void)
{
    return s_source;
}

bool karaoke_catalog_is_sd_active(void)
{
    return s_source != KARAOKE_SOURCE_ROM;
}

uint32_t karaoke_catalog_get_total_songs(void)
{
    return s_total_songs;
}

/* Restarts the SD driver and reopens songs.idx. */
static bool recover_index(void)
{
    (void)sd_card_recover();
    sd_card_lock();
    f_close(&s_idx_file);
    bool ok = (f_open(&s_idx_file, s_idx_path, FA_READ) == FR_OK);
    sd_card_unlock();
    return ok;
}

/* Binary search over the sorted records. @p io_error tells a failed read apart from "not found". */
static bool search_index(uint32_t target_code, uint32_t *out_index, bool *io_error)
{
    sd_card_lock();
    bool found = false;
    uint32_t low = 0;
    uint32_t high = s_total_songs - 1;

    while (low <= high) {
        uint32_t mid = low + (high - low) / 2;
        FSIZE_t offset = INDEX_HEADER_BYTES + (FSIZE_t)mid * INDEX_RECORD_BYTES;
        uint8_t rec[4];
        UINT br = 0;

        if (f_lseek(&s_idx_file, offset) != FR_OK ||
            f_read(&s_idx_file, rec, sizeof(rec), &br) != FR_OK || br != sizeof(rec)) {
            *io_error = true;
            break;
        }

        uint32_t mid_code = le32(rec);
        if (mid_code == target_code) {
            *out_index = mid;
            found = true;
            break;
        } else if (mid_code < target_code) {
            low = mid + 1;
        } else {
            if (mid == 0) break;
            high = mid - 1;
        }
    }
    sd_card_unlock();
    return found;
}

bool karaoke_catalog_find_by_code(uint32_t target_code, uint32_t *out_index)
{
    if (!out_index || s_total_songs == 0) return false;

    if (s_source == KARAOKE_SOURCE_ROM) {
        for (uint32_t i = 0; i < ROM_SONG_COUNT; i++) {
            if (ROM_SONGS[i].song_code == target_code) {
                *out_index = i;
                return true;
            }
        }
        return false;
    }

    if (s_source == KARAOKE_SOURCE_SCAN) {
        for (uint32_t i = 0; i < s_scan_table.count; i++) {
            const char *rel = song_table_get(&s_scan_table, (uint16_t)i);
            if (!rel) continue;
            const char *slash = strrchr(rel, '/');
            const char *base = (slash != NULL) ? slash + 1 : rel;
            uint32_t code = 0;
            char dummy[16];
            song_name_parse(base, &code, dummy, sizeof(dummy), dummy, sizeof(dummy));
            if (code == target_code) {
                *out_index = i;
                return true;
            }
        }
        return false;
    }

    /* Fast Binary Search on sorted songs.idx. A read error restarts the whole search after the
     * card has been brought back. */
    for (unsigned attempt = 0; attempt < SD_ACCESS_ATTEMPTS; attempt++) {
        if (attempt > 0U && !recover_index()) {
            continue;
        }
        bool io_error = false;
        bool found = search_index(target_code, out_index, &io_error);

        if (!io_error) {
            return found;
        }
    }
    return false;
}

bool karaoke_catalog_get_song(uint32_t index, song_entry_t *out_song)
{
    if (!out_song || index >= s_total_songs) return false;

    if (s_source == KARAOKE_SOURCE_ROM) {
        *out_song = ROM_SONGS[index];
        return true;
    }

    if (s_source == KARAOKE_SOURCE_SCAN) {
        const char *rel = song_table_get(&s_scan_table, (uint16_t)index);
        if (rel == NULL) return false;

        const char *slash = strrchr(rel, '/');
        const char *base = (slash != NULL) ? slash + 1 : rel;

        memset(out_song, 0, sizeof(*out_song));
        song_name_parse(base, &out_song->song_code,
                        out_song->singer, sizeof(out_song->singer),
                        out_song->title, sizeof(out_song->title));
        memcpy(out_song->language, "SD", 3);

        size_t name_len = strnlen(base, sizeof(out_song->filename) - 1);
        memcpy(out_song->filename, base, name_len);
        out_song->filename[name_len] = '\0';
        return true;
    }

    /* Read 96-byte record from songs.idx: offset = 16 + index * 96 */
    FSIZE_t offset = INDEX_HEADER_BYTES + (FSIZE_t)index * INDEX_RECORD_BYTES;
    uint8_t rec[INDEX_RECORD_BYTES];
    UINT br = 0;

    FRESULT fr = FR_DISK_ERR;
    for (unsigned attempt = 0; attempt < SD_ACCESS_ATTEMPTS; attempt++) {
        if (attempt > 0U && !recover_index()) {
            continue;
        }
        br = 0;
        sd_card_lock();
        fr = f_lseek(&s_idx_file, offset);
        if (fr == FR_OK) {
            fr = f_read(&s_idx_file, rec, sizeof(rec), &br);
        }
        sd_card_unlock();
        if (fr == FR_OK && br == sizeof(rec)) {
            break;
        }
        fr = FR_DISK_ERR;
    }
    if (fr != FR_OK) return false;

    out_song->song_code = le32(rec);

    memcpy(out_song->title, rec + 4, 39);
    out_song->title[39] = '\0';

    memcpy(out_song->singer, rec + 44, 27);
    out_song->singer[27] = '\0';

    memcpy(out_song->language, rec + 72, 7);
    out_song->language[7] = '\0';

    memcpy(out_song->filename, rec + 80, 15);
    out_song->filename[15] = '\0';

    return true;
}

/* Called from the sequencer thread. Playback reads sectors in a scattered order, so seek only
 * when the file position is not already where the read starts. */
static bool song_read_once(uint32_t offset, uint8_t *dst, uint32_t len)
{
    UINT br = 0;
    FRESULT fr = FR_OK;

    sd_card_lock();
    if (s_song_pos != offset) {
        fr = f_lseek(&s_song_file, offset);
    }
    if (fr == FR_OK) {
        fr = f_read(&s_song_file, dst, len, &br);
    }
    sd_card_unlock();

    if (fr != FR_OK || br != len) {
        s_song_pos = UINT32_MAX;
        return false;
    }
    s_song_pos = offset + len;
    return true;
}

/* Restarts the SD driver and reopens the song file, which FatFs refuses to read again once a
 * read has failed on it. */
static bool recover_song(void)
{
    (void)sd_card_recover();
    sd_card_lock();
    f_close(&s_song_file);
    bool ok = (f_open(&s_song_file, s_song_path, FA_READ) == FR_OK);
    sd_card_unlock();
    s_song_pos = UINT32_MAX;
    return ok;
}

static bool song_read(void *ctx, uint32_t offset, uint8_t *dst, uint32_t len)
{
    ARG_UNUSED(ctx);

    for (unsigned attempt = 0; attempt < SD_ACCESS_ATTEMPTS; attempt++) {
        if (attempt > 0U && !recover_song()) {
            continue;
        }
        if (song_read_once(offset, dst, len)) {
            return true;
        }
    }
    printk("[Catalog] Song read failed at offset %u after %u attempts\n",
           (unsigned)offset, (unsigned)SD_ACCESS_ATTEMPTS);
    return false;
}

static read_result_t open_midi_file(const char *path, midi_source_t *out)
{
    close_song();

    sd_card_lock();
    FRESULT fr = f_open(&s_song_file, path, FA_READ);
    if (fr != FR_OK) {
        sd_card_unlock();
        if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
            return READ_NO_FILE;
        }
        /* The card itself failed (not "no such file"): restart the driver and look once more. */
        printk("[Catalog] Open of %s failed (FatFs error %d)\n", path, (int)fr);
        (void)sd_card_recover();
        sd_card_lock();
        fr = f_open(&s_song_file, path, FA_READ);
        if (fr != FR_OK) {
            sd_card_unlock();
            return (fr == FR_NO_FILE || fr == FR_NO_PATH) ? READ_NO_FILE : READ_FAILED;
        }
    }
    FSIZE_t fsize = f_size(&s_song_file);
    if (fsize < 14U || fsize > 0x7FFFFFFFU) {
        f_close(&s_song_file);
        sd_card_unlock();
        printk("[Catalog] Song file %s has an unusable size (%u bytes)\n", path, (unsigned)fsize);
        return READ_FAILED;
    }
    sd_card_unlock();

    s_song_open = true;
    s_song_pos = UINT32_MAX;
    snprintf(s_song_path, sizeof(s_song_path), "%s", path);
    out->mem = NULL;
    out->size = (uint32_t)fsize;
    out->read = song_read;
    out->ctx = NULL;
    return READ_OK;
}

bool karaoke_catalog_open_song(uint32_t index, midi_source_t *out_src)
{
    if (!out_src || index >= s_total_songs) return false;

    if (s_source == KARAOKE_SOURCE_ROM) {
        close_song();
        out_src->mem = ROM_MIDI[index].data;
        out_src->size = ROM_MIDI[index].length;
        out_src->read = NULL;
        out_src->ctx = NULL;
        return true;
    }

    read_result_t result = READ_NO_FILE;
    char path[SCAN_PATH_BYTES + 4];

    if (s_source == KARAOKE_SOURCE_SCAN) {
        const char *rel = song_table_get(&s_scan_table, (uint16_t)index);
        if (rel == NULL || !join_path(path, sizeof(path), SD_ROOT, rel)) {
            return false;
        }
        result = open_midi_file(path, out_src);
    } else {
        song_entry_t s;
        if (!karaoke_catalog_get_song(index, &s)) return false;

        /* Sharded card first (SD:/midi/036/036527.mid), then the older flat card. */
        for (int flat = 0; flat < 2 && result == READ_NO_FILE; flat++) {
            if (!song_midi_path(path, sizeof(path), s.song_code, s.filename, flat != 0)) {
                printk("[Catalog] Path too long for %s\n", s.filename);
                return false;
            }
            result = open_midi_file(path, out_src);
        }
    }

    if (result == READ_NO_FILE) {
        printk("[Catalog] Failed to open %s\n", path);
    }
    return result == READ_OK;
}
