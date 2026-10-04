#include "sd_card_reader.h"
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/printk.h>
#include <zephyr/storage/disk_access.h>
#include <ff.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#define SD_DISK_NAME "SD"
#define SD_MOUNT_POINT "/SD:"

static FATFS s_fatfs;
static struct fs_mount_t s_mount = {
    .type = FS_FATFS,
    .fs_data = &s_fatfs,
    .mnt_point = SD_MOUNT_POINT,
};
static bool s_mounted;
static sd_track_t s_tracks[MAX_SD_TRACKS];
static uint8_t s_track_count;

bool sd_card_is_mounted(void) { return s_mounted; }
uint8_t sd_card_get_track_count(void) { return s_track_count; }

const sd_track_t *sd_card_get_track(uint8_t index)
{
    return index < s_track_count ? &s_tracks[index] : NULL;
}

void sd_card_reader_init(void)
{
    s_track_count = 0;
    k_msleep(1000);
    int err = disk_access_init(SD_DISK_NAME);
    if (err != 0) {
        printk("[SD_FS] Disk init failed (err %d)\n", err);
        return;
    }
    err = fs_mount(&s_mount);
    if (err != 0) {
        printk("[SD_FS] Mount %s failed (err %d)\n", SD_MOUNT_POINT, err);
        return;
    }

    s_mounted = true;
    printk("[SD_FS] Filesystem mounted at %s\n", SD_MOUNT_POINT);

    struct fs_dir_t dir;
    struct fs_dirent entry;
    fs_dir_t_init(&dir);
    if (fs_opendir(&dir, SD_MOUNT_POINT) != 0) {
        printk("[SD_FS] Failed to open %s\n", SD_MOUNT_POINT);
        return;
    }

    while (s_track_count < MAX_SD_TRACKS && fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
        if (entry.type != FS_DIR_ENTRY_FILE) {
            continue;
        }
        const char *ext = strrchr(entry.name, '.');
        if (ext == NULL || (strcasecmp(ext, ".wav") != 0 && strcasecmp(ext, ".mp3") != 0)) {
            continue;
        }
        strncpy(s_tracks[s_track_count].filename, entry.name, MAX_FILENAME_LEN - 1);
        s_tracks[s_track_count].filename[MAX_FILENAME_LEN - 1] = '\0';
        s_tracks[s_track_count].size_bytes = entry.size;
        printk("[SD_FS] Track [%u]: %s (%u KB)\n", s_track_count + 1,
               s_tracks[s_track_count].filename,
               (unsigned)(entry.size / 1024U));
        s_track_count++;
    }
    fs_closedir(&dir);
    printk("[SD_FS] Found %u WAV track(s)\n", s_track_count);

    if (s_track_count > 0) {
        /* Direct FatFs probe: verifies the card is readable through the same
         * API the WAV player uses, independent of the Zephyr fs wrapper. */
        FIL probe;
        uint8_t header[12];
        UINT bytes_read = 0;
        char path[128];
        snprintf(path, sizeof(path), "SD:/%s", s_tracks[0].filename);
        FRESULT fr = f_open(&probe, path, FA_READ);
        if (fr == FR_OK) {
            fr = f_read(&probe, header, sizeof(header), &bytes_read);
            printk("[SD_FS] FatFs probe: read=%u, RIFF=%s\n",
                   bytes_read,
                   (fr == FR_OK && bytes_read == sizeof(header) && memcmp(header, "RIFF", 4) == 0) ? "YES" : "NO");
            f_close(&probe);
        } else {
            printk("[SD_FS] FatFs probe open failed: %s (result %d)\n", path, (int)fr);
        }
    }
}
