/**
 * @file sd_card_reader.c
 * @brief Implementation of Micro-SD Card FAT32 Filesystem and USB Mass Storage
 *        Card Reader for the RT-Thread Spark Board (STM32F407ZGT6) on Zephyr RTOS.
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

/* -------------------------------------------------------------------------- */
/* Hardware Card Detect (PF3) Initialization                                  */
/* -------------------------------------------------------------------------- */
static void card_detect_gpio_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOFEN;

    /* Configure PF3 (GPIO_CARD_DETECT) as Input with Pull-Up */
    GPIOF->MODER &= ~(3U << (3 * 2));
    GPIOF->PUPDR = (GPIOF->PUPDR & ~(3U << (3 * 2))) | (1U << (3 * 2));
}

bool sd_card_is_present(void)
{
    /* Active LOW on RT-Thread Spark board: 0 = card inserted, 1 = empty */
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
                    strncpy(s_tracks[s_track_count].filename, entry.name, MAX_FILENAME_LEN - 1);
                    s_tracks[s_track_count].filename[MAX_FILENAME_LEN - 1] = '\0';
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
        if (sd_card_is_present()) {
            int ret = fs_mount(&s_mp);
            if (ret == 0) {
                s_is_mounted = true;
                printk("[SD_FS] Re-mounted FAT32 filesystem successfully.\n");
                sd_card_scan_tracks();
            } else {
                printk("[SD_FS] Failed to re-mount FAT32 filesystem (err %d)\n", ret);
            }
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

    if (sd_card_is_present()) {
        printk("[SD_Card] Micro-SD card physically detected in socket.\n");

        /* Attempt to mount FAT32 filesystem */
        int ret = fs_mount(&s_mp);
        if (ret == 0) {
            s_is_mounted = true;
            printk("[SD_FS] FAT32 filesystem mounted successfully at %s\n", DISK_MOUNT_PT);
            sd_card_scan_tracks();
        } else {
            printk("[SD_FS] Notice: FAT32 mount returned %d (card may need FAT formatting or reader mode)\n", ret);
        }
    } else {
        printk("[SD_Card] No Micro-SD card detected in socket (PF3 HIGH).\n");
    }
}
