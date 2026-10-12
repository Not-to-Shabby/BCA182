/**
 * @file w25q64.h
 * @brief High-speed SPI2 driver for the onboard Winbond W25Q64 (64 Mbit / 8 MByte) SPI NOR Flash.
 *
 * Pinout on RT-Thread RT-Spark (STM32F407ZGT6):
 *   - PB12: Flash CS (Chip Select, Active LOW, GPIO Output)
 *   - PB13: SPI2_SCK (15.0 MHz clock, AF5)
 *   - PC2:  SPI2_MISO (Master-In Slave-Out, AF5)
 *   - PC3:  SPI2_MOSI (Master-Out Slave-In, AF5)
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#ifndef W25Q64_H_
#define W25Q64_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define W25Q64_JEDEC_ID         0xEF4017U   /**< Winbond W25Q64: Mfr 0xEF, Memory 0x40, Capacity 0x17 (8MB) */
#define W25Q64_TOTAL_SIZE       (8U * 1024U * 1024U) /**< 8,388,608 bytes total capacity */
#define W25Q64_SECTOR_SIZE      4096U       /**< 4 KB Sector Erase block */
#define W25Q64_PAGE_SIZE        256U        /**< 256 Byte Page Program buffer */

/**
 * @brief Initialize SPI2 peripheral and GPIO pins, then probe the W25Q64 JEDEC ID.
 * @return true if communication succeeded and JEDEC ID matches W25Q64_JEDEC_ID (0xEF4017).
 */
bool w25q64_init(void);

/**
 * @brief Returns whether the W25Q64 was detected and is ready.
 */
bool w25q64_is_available(void);

/**
 * @brief Read the 24-bit JEDEC manufacturer and device ID (0x9F command).
 */
uint32_t w25q64_read_jedec_id(void);

/**
 * @brief Read Status Register 1 (0x05 command).
 */
uint8_t w25q64_read_status(void);

/**
 * @brief Clear hardware write-protect bits (BP0, BP1, BP2) to allow sector erase and write.
 */
void w25q64_unlock(void);

/**
 * @brief Read raw data from SPI Flash starting at byte address @p addr.
 *        Uses Fast Read (0x0B) at 15.0 MHz.
 */
void w25q64_read(uint32_t addr, void *buf, size_t len);

/**
 * @brief Erase a 4 KB sector containing address @p addr (0x20 command).
 *        Blocks until the flash internal erase cycle finishes (~30-50 ms).
 */
void w25q64_erase_sector(uint32_t addr);

/**
 * @brief Program up to 256 bytes into a single page at address @p addr (0x02 command).
 *        Blocks until the page program cycle finishes (~0.4-0.8 ms).
 */
void w25q64_page_program(uint32_t addr, const void *buf, size_t len);

/**
 * @brief Multi-page write helper that automatically breaks large buffers across page boundaries.
 */
void w25q64_write(uint32_t addr, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* W25Q64_H_ */
