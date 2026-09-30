/**
 * @file sd_card_reader.c
 * @brief Implementation of Micro-SD Card Inspection, FAT32/exFAT Filesystem,
 *        On-Device FAT32 Formatting, and USB Mass Storage Card Reader for RT-Spark.
 *
 * Course: BCA182 Embedded Systems Programming
 * Laboratory Activity 2: Personal MP3 Player
 */

#include "sd_card_reader.h"
#include "threads.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/fs/fs.h>
#include <ff.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/usb/usb_device.h>
#include <stm32f4xx.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#define DISK_DRIVE_NAME         "SD"
#define DISK_MOUNT_PT           "/" DISK_DRIVE_NAME ":"

static FATFS s_fat_fs;
static struct fs_mount_t s_mp = {
    .type = FS_FATFS,
    .fs_data = &s_fat_fs,
    .mnt_point = DISK_MOUNT_PT,
};

static bool s_is_mounted = false;
static sd_reader_mode_t s_current_mode = SD_MODE_STANDALONE;
static sd_track_t s_tracks[MAX_SD_TRACKS];
static uint8_t s_track_count = 0;

static sd_card_inspection_t s_card_info = {
    .card_initialized = false,
    .sector_count = 0,
    .sector_size = 512,
    .capacity_mb = 0,
    .mbr_signature_valid = false,
    .partition1_type = 0,
    .partition1_lba_start = 0,
    .oem_name = "NONE",
    .fs_label = "NONE",
    .detected_fs = SD_FS_UNKNOWN,
    .detected_fs_name = "UNKNOWN",
    .mount_errno = 0
};

/* -------------------------------------------------------------------------- */
/* Hardware Card Detect (PF3)                                                 */
/* -------------------------------------------------------------------------- */
static void card_detect_gpio_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOFEN;

    /* Configure PF3 as Input with Pull-Up */
    GPIOF->MODER &= ~(3U << (3 * 2));
    GPIOF->PUPDR = (GPIOF->PUPDR & ~(3U << (3 * 2))) | (1U << (3 * 2));
}

bool sd_card_is_present(void)
{
    /* On RT-Thread Spark: 0 = card inserted, 1 = empty (with internal pull-up) */
    return ((GPIOF->IDR & (1U << 3)) == 0);
}

bool sd_card_is_mounted(void)
{
    return s_is_mounted;
}

uint8_t sd_card_get_track_count(void)
{
    return s_track_count;
}

const sd_track_t* sd_card_get_track(uint8_t index)
{
    if (index >= s_track_count) {
        return NULL;
    }
    return &s_tracks[index];
}

sd_reader_mode_t sd_card_get_mode(void)
{
    return s_current_mode;
}

const sd_card_inspection_t* sd_card_get_inspection(void)
{
    return &s_card_info;
}

/* -------------------------------------------------------------------------- */
/* Low-Level Sector Inspection: Inspect MBR, VBR, and Filesystem Format      */
/* -------------------------------------------------------------------------- */
int sd_card_inspect(void)
{
    char diag_buf[384];
    static uint8_t sector_buf[512];

    memset(&s_card_info, 0, sizeof(s_card_info));
    s_card_info.detected_fs_name = "NO CARD";
    s_card_info.detected_fs = SD_FS_NO_CARD;

    /* 1. Initialize physical SDIO disk layer */
    int ret = disk_access_init(DISK_DRIVE_NAME);
    for (int retry = 0; ret != 0 && retry < 3; retry++) {
        k_msleep(100);
        ret = disk_access_init(DISK_DRIVE_NAME);
    }

    if (ret != 0) {
        printk("[SD_INSPECT] disk_access_init failed (err %d)\n", ret);
        uart1_direct_print("[SD_INSPECT] disk_access_init failed!\n");
        return ret;
    }
    s_card_info.card_initialized = true;

    /* 2. Query Sector Count and Capacity */
    uint32_t count = 0;
    uint32_t size = 512;
    disk_access_ioctl(DISK_DRIVE_NAME, DISK_IOCTL_GET_SECTOR_COUNT, &count);
    disk_access_ioctl(DISK_DRIVE_NAME, DISK_IOCTL_GET_SECTOR_SIZE, &size);
    if (size == 0) size = 512;
    s_card_info.sector_count = count;
    s_card_info.sector_size = size;
    s_card_info.capacity_mb = (uint32_t)(((uint64_t)count * size) / (1024ULL * 1024ULL));

    /* 3. Read Sector 0 (MBR or Superfloppy VBR) */
    ret = disk_access_read(DISK_DRIVE_NAME, sector_buf, 0, 1);
    if (ret != 0) {
        printk("[SD_INSPECT] disk_access_read Sector 0 failed (err %d)\n", ret);
        uart1_direct_print("[SD_INSPECT] Read Sector 0 failed!\n");
        return ret;
    }

    /* Check 0x55AA boot signature */
    s_card_info.mbr_signature_valid = (sector_buf[510] == 0x55 && sector_buf[511] == 0xAA);

    /* Check if Sector 0 is directly a VBR (Volume Boot Record) */
    memcpy(s_card_info.oem_name, &sector_buf[3], 8);
    s_card_info.oem_name[8] = '\0';

    if (memcmp(&sector_buf[3], "EXFAT   ", 8) == 0) {
        s_card_info.detected_fs = SD_FS_EXFAT;
        s_card_info.detected_fs_name = "exFAT";
    } else if (memcmp(&sector_buf[3], "NTFS    ", 8) == 0) {
        s_card_info.detected_fs = SD_FS_NTFS;
        s_card_info.detected_fs_name = "NTFS";
    } else if (memcmp(&sector_buf[82], "FAT32", 5) == 0) {
        s_card_info.detected_fs = SD_FS_FAT32;
        s_card_info.detected_fs_name = "FAT32";
    } else if (memcmp(&sector_buf[54], "FAT16", 5) == 0) {
        s_card_info.detected_fs = SD_FS_FAT16;
        s_card_info.detected_fs_name = "FAT16";
    }

    /* 4. Check MBR Partition Table (Offset 0x1BE) */
    if (s_card_info.detected_fs == SD_FS_UNKNOWN && s_card_info.mbr_signature_valid) {
        uint8_t p_type = sector_buf[0x1BE + 4];
        uint32_t lba_start = (uint32_t)sector_buf[0x1BE + 8] |
                            ((uint32_t)sector_buf[0x1BE + 9] << 8) |
                            ((uint32_t)sector_buf[0x1BE + 10] << 16) |
                            ((uint32_t)sector_buf[0x1BE + 11] << 24);

        s_card_info.partition1_type = p_type;
        s_card_info.partition1_lba_start = lba_start;

        if (p_type == 0x0B || p_type == 0x0C) {
            s_card_info.detected_fs = SD_FS_FAT32;
            s_card_info.detected_fs_name = "FAT32";
        } else if (p_type == 0x04 || p_type == 0x06 || p_type == 0x0E) {
            s_card_info.detected_fs = SD_FS_FAT16;
            s_card_info.detected_fs_name = "FAT16";
        } else if (p_type == 0x01) {
            s_card_info.detected_fs = SD_FS_FAT12;
            s_card_info.detected_fs_name = "FAT12";
        } else if (p_type == 0x07) {
            /* Type 0x07 can be exFAT or NTFS: read partition 1 VBR */
            if (lba_start > 0 && lba_start < count) {
                if (disk_access_read(DISK_DRIVE_NAME, sector_buf, lba_start, 1) == 0) {
                    memcpy(s_card_info.oem_name, &sector_buf[3], 8);
                    s_card_info.oem_name[8] = '\0';
                    if (memcmp(&sector_buf[3], "EXFAT   ", 8) == 0) {
                        s_card_info.detected_fs = SD_FS_EXFAT;
                        s_card_info.detected_fs_name = "exFAT";
                    } else if (memcmp(&sector_buf[3], "NTFS    ", 8) == 0) {
                        s_card_info.detected_fs = SD_FS_NTFS;
                        s_card_info.detected_fs_name = "NTFS";
                    } else {
                        s_card_info.detected_fs_name = "exFAT/NTFS";
                    }
                }
            } else {
                s_card_info.detected_fs_name = "exFAT/NTFS";
            }
        } else if (p_type == 0x00) {
            s_card_info.detected_fs = SD_FS_RAW_NO_MBR;
            s_card_info.detected_fs_name = "RAW / UNFORMATTED";
        }
    }

    /* Print Complete Inspection Results to Console */
    printk("\n=====================================================\n");
    printk("  MICRO-SD CARD HARDWARE INSPECTION REPORT\n");
    printk("=====================================================\n");
    snprintf(diag_buf, sizeof(diag_buf),
             "  Capacity       : %u MB (%.2f GB)\n"
             "  Sector Count   : %u sectors (%u bytes/sector)\n"
             "  MBR Signature  : 0x%02X%02X (%s)\n"
             "  Partition Type : 0x%02X (LBA Start: %u)\n"
             "  OEM Identifier : '%s'\n"
             "  Filesystem     : %s\n"
             "=====================================================\n",
             s_card_info.capacity_mb, (double)s_card_info.capacity_mb / 1024.0,
             s_card_info.sector_count, s_card_info.sector_size,
             s_card_info.mbr_signature_valid ? 0x55 : 0x00,
             s_card_info.mbr_signature_valid ? 0xAA : 0x00,
             s_card_info.mbr_signature_valid ? "VALID" : "INVALID",
             s_card_info.partition1_type, s_card_info.partition1_lba_start,
             s_card_info.oem_name, s_card_info.detected_fs_name);

    printk("%s", diag_buf);
    uart1_direct_print("\n=====================================================\n");
    uart1_direct_print("  MICRO-SD CARD HARDWARE INSPECTION REPORT\n");
    uart1_direct_print("=====================================================\n");
    uart1_direct_print(diag_buf);

    return 0;
}

/* -------------------------------------------------------------------------- */
/* On-Device Micro-SD Card FAT32 Formatting                                  */
/* -------------------------------------------------------------------------- */
int sd_card_format_fat32(void)
{
    printk("\n[SD_FORMAT] =======================================\n");
    printk("[SD_FORMAT] Formatting Micro-SD card to FAT32...\n");
    uart1_direct_print("\n[SD_FORMAT] Formatting Micro-SD card to FAT32...\n");

    /* 1. Unmount existing filesystem if mounted */
    if (s_is_mounted) {
        fs_unmount(&s_mp);
        s_is_mounted = false;
        printk("[SD_FORMAT] Unmounted active filesystem.\n");
    }

    /* 2. Configure FatFs MKFS options: FAT32 format, auto cluster size */
    MKFS_PARM opt = {
        .fmt = FM_FAT32,
        .n_fat = 1,
        .align = 0,
        .n_root = 512,
        .au_size = 0
    };

    static uint8_t work_buf[FF_MAX_SS];
    FRESULT fr = f_mkfs("SD:", &opt, work_buf, sizeof(work_buf));
    if (fr != FR_OK) {
        printk("[SD_FORMAT] Format failed with FatFs code %d!\n", (int)fr);
        uart1_direct_print("[SD_FORMAT] Format failed!\n");
        return -(int)fr;
    }

    printk("[SD_FORMAT] Format to FAT32 completed successfully!\n");
    uart1_direct_print("[SD_FORMAT] Format to FAT32 completed successfully!\n");

    /* 3. Re-mount the newly formatted FAT32 filesystem */
    int ret = fs_mount(&s_mp);
    if (ret == 0) {
        s_is_mounted = true;
        s_card_info.detected_fs = SD_FS_FAT32;
        s_card_info.detected_fs_name = "FAT32";
        printk("[SD_FORMAT] Mounted new FAT32 volume at /SD:\n");
        uart1_direct_print("[SD_FORMAT] Mounted new FAT32 volume at /SD:\n");
        sd_card_scan_tracks();
    } else {
        printk("[SD_FORMAT] Re-mount returned error %d\n", ret);
    }
    printk("[SD_FORMAT] =======================================\n\n");

    return ret;
}

/* -------------------------------------------------------------------------- */
/* Directory Scanning for .WAV and .MP3 Audio Files                          */
/* -------------------------------------------------------------------------- */
uint8_t sd_card_scan_tracks(void)
{
    s_track_count = 0;

    if (!s_is_mounted) {
        return 0;
    }

    struct fs_dir_t dirp;
    fs_dir_t_init(&dirp);

    int res = fs_opendir(&dirp, DISK_MOUNT_PT);
    if (res != 0) {
        printk("[SD_FS] Failed to open root directory %s (err %d)\n", DISK_MOUNT_PT, res);
        return 0;
    }

    printk("[SD_FS] Scanning %s for audio tracks...\n", DISK_MOUNT_PT);
    struct fs_dirent entry;

    while (fs_readdir(&dirp, &entry) == 0 && entry.name[0] != 0) {
        if (entry.type == FS_DIR_ENTRY_FILE) {
            char *ext = strrchr(entry.name, '.');
            if (ext != NULL && (strcasecmp(ext, ".wav") == 0 || strcasecmp(ext, ".mp3") == 0)) {
                if (s_track_count < MAX_SD_TRACKS) {
                    snprintf(s_tracks[s_track_count].filename, sizeof(s_tracks[s_track_count].filename),
                             "%.31s", entry.name);
                    s_tracks[s_track_count].size_bytes = entry.size;
                    printk("[SD_FS] Track [%u]: %s (%u KB)\n",
                           s_track_count + 1, entry.name, (unsigned int)(entry.size / 1024));
                    s_track_count++;
                }
            }
        }
    }

    fs_closedir(&dirp);
    printk("[SD_FS] Scan complete. Found %u audio tracks on SD card.\n", s_track_count);
    return s_track_count;
}

/* -------------------------------------------------------------------------- */
/* Mode Switching: Standalone Player vs USB Card Reader                      */
/* -------------------------------------------------------------------------- */
void sd_card_set_mode(sd_reader_mode_t mode)
{
    if (mode == s_current_mode) {
        return;
    }

    if (mode == SD_MODE_USB_CARD_READER) {
        printk("\n[USB_MSC] Switching to USB Card Reader mode...\n");
        uart1_direct_print("[USB_MSC] Switching to USB Card Reader mode...\n");

        /* 1. Unmount FAT32 volume from microcontroller to avoid filesystem collision */
        if (s_is_mounted) {
            fs_unmount(&s_mp);
            s_is_mounted = false;
            printk("[SD_FS] Unmounted FAT32 volume from MCU for exclusive PC access.\n");
        }

        /* Disconnect USB DP (PA12) explicitly by setting to general purpose output LOW */
        RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
        uint32_t old_moder = GPIOA->MODER;
        uint32_t old_otyper = GPIOA->OTYPER;
        uint32_t old_pupdr = GPIOA->PUPDR;
        
        GPIOA->MODER = (GPIOA->MODER & ~(3U << 24)) | (1U << 24); /* Output */
        GPIOA->OTYPER &= ~(1U << 12); /* Push-Pull */
        GPIOA->PUPDR &= ~(3U << 24); /* No pull */
        GPIOA->ODR &= ~(1U << 12); /* Drive LOW */
        
        k_msleep(150); /* Hold low long enough for Windows to see disconnect */
        
        /* Restore Alternate Function */
        GPIOA->MODER = old_moder;
        GPIOA->OTYPER = old_otyper;
        GPIOA->PUPDR = old_pupdr;
        k_msleep(50);

        /* 2. Enable Zephyr USB Device Stack with Mass Storage Class */
        int ret = usb_enable(NULL);
        if (ret == 0) {
            s_current_mode = SD_MODE_USB_CARD_READER;
            printk("[USB_MSC] USB Card Reader active! Plug cable into CN4 to view SD card in Windows.\n");
            uart1_direct_print("[USB_MSC] USB Card Reader active! Connect CN4 to PC.\n");
        } else {
            printk("[USB_MSC] Failed to enable USB Device (err %d)\n", ret);
        }
    } else {
        printk("\n[USB_MSC] Disabling USB Card Reader mode...\n");
        uart1_direct_print("[USB_MSC] Disabling USB Card Reader mode...\n");

        /* 1. Disable USB Device */
        usb_disable();
        k_msleep(200);

        /* 2. Re-mount FAT32 volume on the microcontroller */
        int ret = disk_access_init(DISK_DRIVE_NAME);
        for (uint8_t attempt = 0; ret != 0 && attempt < 3; attempt++) {
            k_msleep(300);
            ret = disk_access_init(DISK_DRIVE_NAME);
        }
        if (ret == 0) {
            ret = fs_mount(&s_mp);
        }
        if (ret == 0) {
            s_is_mounted = true;
            printk("[SD_FS] Re-mounted FAT32 filesystem successfully.\n");
            sd_card_scan_tracks();
        } else {
            s_card_info.mount_errno = ret;
            printk("[SD_FS] Failed to re-mount FAT32 filesystem (err %d)\n", ret);
        }

        s_current_mode = SD_MODE_STANDALONE;
    }
}

sd_reader_mode_t sd_card_toggle_mode(void)
{
    if (s_current_mode == SD_MODE_STANDALONE) {
        sd_card_set_mode(SD_MODE_USB_CARD_READER);
    } else {
        sd_card_set_mode(SD_MODE_STANDALONE);
    }
    return s_current_mode;
}

/* -------------------------------------------------------------------------- */
/* Initialization                                                             */
/* -------------------------------------------------------------------------- */
void sd_card_reader_init(void)
{
    printk("[SD_Card] Initializing Micro-SD interface and Card Detect (PF3)...\n");
    card_detect_gpio_init();

    /* Inspect raw card sectors (MBR / VBR / Filesystem type) */
    sd_card_inspect();

    /* Attempt to mount FAT32/exFAT filesystem */
    int ret = fs_mount(&s_mp);
    s_card_info.mount_errno = ret;

    if (ret == 0) {
        s_is_mounted = true;
        printk("[SD_FS] Filesystem mounted successfully at %s\n", DISK_MOUNT_PT);
        uart1_direct_print("[SD_FS] Filesystem mounted successfully!\n");
        sd_card_scan_tracks();
    } else {
        printk("[SD_FS] Notice: Mount returned %d (%s). Hold LEFT+UP to format to FAT32.\n",
               ret, s_card_info.detected_fs_name);
        uart1_direct_print("[SD_FS] Filesystem mount failed. Hold LEFT+UP to format to FAT32.\n");
    }
}
