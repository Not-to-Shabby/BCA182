/**
 * @file sd_card_reader.h
 * @brief Micro-SD Card FAT32/exFAT Filesystem Inspection, Formatting, and
 *        USB Mass Storage Card Reader Driver for the RT-Thread Spark Board.
 *
 * Course: BCA182 Embedded Systems Programming
 * Laboratory Activity 2: Personal MP3 Player
 */

#ifndef SD_CARD_READER_H_
#define SD_CARD_READER_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SD_TRACKS           16
#define MAX_FILENAME_LEN        32

/**
 * @brief Detected filesystem types on the Micro-SD card.
 */
typedef enum {
    SD_FS_UNKNOWN = 0,
    SD_FS_FAT12,
    SD_FS_FAT16,
    SD_FS_FAT32,
    SD_FS_EXFAT,
    SD_FS_NTFS,
    SD_FS_RAW_NO_MBR,
    SD_FS_NO_CARD
} sd_fs_type_t;

/**
 * @brief Low-level Micro-SD card inspection data.
 */
typedef struct {
    bool card_initialized;
    uint32_t sector_count;
    uint32_t sector_size;
    uint32_t capacity_mb;
    bool mbr_signature_valid;
    uint8_t partition1_type;
    uint32_t partition1_lba_start;
    char oem_name[9];
    char fs_label[9];
    sd_fs_type_t detected_fs;
    const char *detected_fs_name;
    int mount_errno;
} sd_card_inspection_t;

/**
 * @brief Metadata for a file discovered on the SD card FAT32 filesystem.
 */
typedef struct {
    char filename[MAX_FILENAME_LEN];
    uint32_t size_bytes;
} sd_track_t;

/**
 * @brief Operational modes of the SD Card subsystem.
 */
typedef enum {
    SD_MODE_STANDALONE = 0,   /**< Microcontroller mounts FAT32 via FatFs for music playback */
    SD_MODE_USB_CARD_READER   /**< USB U-Disk mode: PC mounts SD card over USB CN4 to drag & drop files */
} sd_reader_mode_t;

/**
 * @brief Initialize SD card interface, inspect raw MBR/VBR sectors,
 *        and attempt to mount the filesystem.
 */
void sd_card_reader_init(void);

/**
 * @brief Low-level inspection of Sector 0 (MBR) and VBR to identify the exact
 *        filesystem (FAT12, FAT16, FAT32, exFAT, NTFS, or RAW).
 * @return 0 on success, negative error code on read failure
 */
int sd_card_inspect(void);

/**
 * @brief Retrieve the latest inspection details.
 */
const sd_card_inspection_t* sd_card_get_inspection(void);

/**
 * @brief Format the Micro-SD card directly to FAT32 on the microcontroller
 *        using FatFs f_mkfs().
 * @return 0 on success, negative error code on failure
 */
int sd_card_format_fat32(void);

/**
 * @brief Check if a physical micro-SD card is currently inserted in the socket.
 */
bool sd_card_is_present(void);

/**
 * @brief Check if the SD card is currently mounted on the microcontroller.
 */
bool sd_card_is_mounted(void);

/**
 * @brief Scan the root directory (/SD:) for .wav and .mp3 audio files.
 * @return Number of tracks found
 */
uint8_t sd_card_scan_tracks(void);

/**
 * @brief Get the total number of audio files discovered on the SD card.
 */
uint8_t sd_card_get_track_count(void);

/**
 * @brief Retrieve track information by index.
 */
const sd_track_t* sd_card_get_track(uint8_t index);

/**
 * @brief Switch operating mode between Standalone Player and USB Card Reader.
 */
void sd_card_set_mode(sd_reader_mode_t mode);

/**
 * @brief Toggle between Standalone Player and USB Card Reader modes.
 * @return New active mode
 */
sd_reader_mode_t sd_card_toggle_mode(void);

/**
 * @brief Get the current active SD card reader mode.
 */
sd_reader_mode_t sd_card_get_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_CARD_READER_H_ */
