#include "sd_card_reader.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/printk.h>
#include <zephyr/storage/disk_access.h>
#include <errno.h>
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
static K_MUTEX_DEFINE(s_fs_lock);

void sd_card_lock(void) { k_mutex_lock(&s_fs_lock, K_FOREVER); }
void sd_card_unlock(void) { k_mutex_unlock(&s_fs_lock); }

static uint32_t s_recover_count;

/* The Zephyr SDMMC driver keeps its completion semaphore among the first members of its private
 * data (drivers/disk/sdmmc_stm32.c, struct stm32_sdmmc_priv) and offers no way to clear it. A
 * transfer that fails can leave a stale token there, and the next read would then return before
 * its data arrived. The semaphore limits are checked before touching it, so a driver with another
 * layout is left alone. */
struct sdmmc_hsd_head {         /* start of the HAL's SD_HandleTypeDef */
    uint32_t instance;
    /* cppcheck-suppress unusedStructMember */
    uint32_t clock_edge;
    /* cppcheck-suppress unusedStructMember */
    uint32_t clock_bypass;
    /* cppcheck-suppress unusedStructMember */
    uint32_t clock_power_save;
    uint32_t bus_wide;
    /* cppcheck-suppress unusedStructMember */
    uint32_t hw_flow_control;
    /* cppcheck-suppress unusedStructMember */
    uint32_t clock_div;
};

struct sdmmc_priv_head {
    /* cppcheck-suppress unusedStructMember */
    void *irq_config;       /* placeholder: keeps the members below at the driver's offsets */
    struct k_sem thread_lock;
    struct k_sem sync;
    struct sdmmc_hsd_head hsd;
};

#define SDIO_BASE_ADDRESS   0x40012C00U
#define HAL_SDIO_BUS_1BIT   0x00000000U
#define HAL_SDIO_BUS_4BIT   0x00000800U

/* Both fixes below need driver internals that Zephyr keeps private. The layout is checked before
 * anything is written, so a driver that looks different is left alone. */
static struct sdmmc_priv_head *sdmmc_priv(void)
{
    const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(sdmmc1));
    struct sdmmc_priv_head *priv = dev->data;

    if (priv != NULL && priv->thread_lock.limit == 1U && priv->sync.limit == 1U &&
        priv->hsd.instance == SDIO_BASE_ADDRESS &&
        (priv->hsd.bus_wide == HAL_SDIO_BUS_1BIT || priv->hsd.bus_wide == HAL_SDIO_BUS_4BIT)) {
        return priv;
    }
    return NULL;
}

/* The driver leaves the handle set to 4-bit bus mode after the first start. A restart then runs
 * the card initialisation in 4-bit mode while the freshly reset card is still in 1-bit mode, so
 * the SCR read times out and the card never comes back. The first start works because the handle
 * begins as 1-bit. */
static void prepare_restart(void)
{
    struct sdmmc_priv_head *priv = sdmmc_priv();

    if (priv != NULL) {
        priv->hsd.bus_wide = HAL_SDIO_BUS_1BIT;
    }
}

static void drop_stale_completion(void)
{
    struct sdmmc_priv_head *priv = sdmmc_priv();

    if (priv != NULL) {
        k_sem_reset(&priv->sync);
    }
}

bool sd_card_recover(void)
{
    bool force = true;
    uint32_t t0 = k_uptime_get_32();

    sd_card_lock();
    s_recover_count++;
    (void)disk_access_ioctl(SD_DISK_NAME, DISK_IOCTL_CTRL_DEINIT, &force);
    prepare_restart();
    int err = disk_access_init(SD_DISK_NAME);
    if (err == 0) {
        drop_stale_completion();
        err = (disk_access_status(SD_DISK_NAME) == 0) ? 0 : -EIO;
    }
    sd_card_unlock();

    printk("[SD_FS] Restarted the SD driver (#%u): %s, %u ms\n", (unsigned)s_recover_count,
           err == 0 ? "card answers" : "card did not answer", (unsigned)(k_uptime_get_32() - t0));
    return err == 0;
}

uint32_t sd_card_get_recover_count(void) { return s_recover_count; }

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
        size_t name_len = strnlen(entry.name, MAX_FILENAME_LEN - 1);
        memcpy(s_tracks[s_track_count].filename, entry.name, name_len);
        s_tracks[s_track_count].filename[name_len] = '\0';
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
        UINT bytes_read = 0;
        char path[128];
        snprintf(path, sizeof(path), "SD:/%s", s_tracks[0].filename);
        FRESULT fr = f_open(&probe, path, FA_READ);
        if (fr == FR_OK) {
            uint8_t header[12];
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
