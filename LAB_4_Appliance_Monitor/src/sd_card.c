/**
 * @file sd_card.c
 * @brief microSD (FatFs) mount / lock / recover, adapted from the verified
 *        Lab 2 side-quest sd_card_reader.c (karaoke track indexing removed).
 *
 * The recover path restarts the STM32 SDIO driver with two driver-internal fixes
 * (1-bit bus mode before restart, stale DMA completion drop) whose layout checks
 * keep them safe when the Zephyr driver changes.
 */

#include "sd_card.h"

#if defined(__arm__) || defined(CONFIG_SOC_SERIES_STM32F4X) || defined(ZEPHYR_VERSION_CODE)
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/printk.h>
#include <errno.h>
#include <ff.h>

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
static uint32_t s_recover_count;

void sd_card_lock(void) { k_mutex_lock(&s_fs_lock, K_FOREVER); }
void sd_card_unlock(void) { k_mutex_unlock(&s_fs_lock); }

/* One bad transfer can leave the SDIO driver stuck: the DMA channel stays claimed,
 * the driver reports the disk as uninitialised so FatFs refuses every call, or a
 * stale completion makes the next read return before its data arrived. The struct
 * heads below reach the driver internals Zephyr keeps private; the layout is
 * checked before anything is written, so a different driver is left alone. */
struct sdmmc_hsd_head {
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
    void *irq_config;
    struct k_sem thread_lock;
    struct k_sem sync;
    struct sdmmc_hsd_head hsd;
};

#define SDIO_BASE_ADDRESS   0x40012C00U
#define HAL_SDIO_BUS_1BIT   0x00000000U
#define HAL_SDIO_BUS_4BIT   0x00000800U

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

/* The driver leaves the handle in 4-bit mode after the first start; a restart then
 * runs card initialisation in 4-bit mode against a freshly reset 1-bit card and the
 * SCR read times out forever. Forcing 1-bit before restart makes recovery work. */
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

    s_recover_count++;
    (void)disk_access_ioctl(SD_DISK_NAME, DISK_IOCTL_CTRL_DEINIT, &force);
    prepare_restart();
    int err = disk_access_init(SD_DISK_NAME);
    if (err == 0) {
        drop_stale_completion();
        err = (disk_access_status(SD_DISK_NAME) == 0) ? 0 : -EIO;
    }

    printk("[SD_FS] Restarted the SD driver (#%u): %s, %u ms\n", (unsigned)s_recover_count,
           err == 0 ? "card answers" : "card did not answer", (unsigned)(k_uptime_get_32() - t0));
    return err == 0;
}

void sd_card_init(void)
{
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
}

bool sd_card_is_mounted(void) { return s_mounted; }
uint32_t sd_card_get_recover_count(void) { return s_recover_count; }

#else
/* Host stubs: no SD card in unit tests. */
void sd_card_init(void) {}
void sd_card_lock(void) {}
void sd_card_unlock(void) {}
bool sd_card_recover(void) { return false; }
bool sd_card_is_mounted(void) { return false; }
uint32_t sd_card_get_recover_count(void) { return 0; }
#endif
