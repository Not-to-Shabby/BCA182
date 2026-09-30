/**
 * @file sd_card_reader.h
 * @brief Micro-SD Card FAT32 Filesystem and USB Mass Storage Card Reader Driver
 *        for the RT-Thread Spark Development Board (STM32F407ZGT6) on Zephyr RTOS.
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
 * @brief Initialize SD card GPIOs (PF3 Card Detect), detect presence,
 *        and attempt to mount the FAT32 volume.
 */
void sd_card_reader_init(void);

/**
 * @brief Check if a physical micro-SD card is currently inserted in the socket (Pin PF3).
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
