/**
 * @file w25q64.c
 * @brief High-speed SPI2 driver for the onboard Winbond W25Q64 (64 Mbit / 8 MByte) SPI NOR Flash.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include "w25q64.h"
#include <string.h>

#if defined(__arm__)
#include <stm32f4xx.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/* SPI2 / W25Q64 Command Set */
#define CMD_WRITE_ENABLE        0x06U
#define CMD_READ_STATUS_1       0x05U
#define CMD_READ_DATA           0x03U
#define CMD_FAST_READ           0x0BU
#define CMD_PAGE_PROGRAM        0x02U
#define CMD_SECTOR_ERASE_4K     0x20U
#define CMD_CHIP_ERASE          0xC7U
#define CMD_JEDEC_ID            0x9FU

#define STATUS_BUSY_BIT         0x01U

static bool s_available = false;

static inline void cs_low(void)
{
    GPIOB->BSRR = (1U << (12 + 16)); /* PB12 LOW */
}

static inline void cs_high(void)
{
    GPIOB->BSRR = (1U << 12);        /* PB12 HIGH */
}

static inline uint8_t spi_transfer(uint8_t byte)
{
    /* Wait until TX buffer is empty */
    while (!(SPI2->SR & SPI_SR_TXE)) {
    }
    SPI2->DR = byte;
    /* Wait until RX buffer has received byte */
    while (!(SPI2->SR & SPI_SR_RXNE)) {
    }
    return (uint8_t)SPI2->DR;
}

static void wait_busy(void)
{
    while (1) {
        cs_low();
        spi_transfer(CMD_READ_STATUS_1);
        uint8_t status = spi_transfer(0xFF);
        cs_high();
        if (!(status & STATUS_BUSY_BIT)) {
            break;
        }
    }
}

static void write_enable(void)
{
    cs_low();
    spi_transfer(CMD_WRITE_ENABLE);
    cs_high();
}

uint8_t w25q64_read_status(void)
{
    cs_low();
    spi_transfer(CMD_READ_STATUS_1);
    uint8_t st = spi_transfer(0xFF);
    cs_high();
    return st;
}

void w25q64_unlock(void)
{
    write_enable();
    cs_low();
    spi_transfer(0x01); /* Write Status Register 1 */
    spi_transfer(0x00); /* Clear BP0, BP1, BP2 block protect bits */
    cs_high();
    wait_busy();
}

bool w25q64_init(void)
{
    /* 1. Clocks: GPIOB, GPIOC, SPI2 */
    RCC->AHB1ENR |= (RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOCEN);
    RCC->APB1ENR |= RCC_APB1ENR_SPI2EN;

    /* 2. Configure PB12 as GPIO Output, Push-Pull, High-Speed (CS pin) */
    GPIOB->MODER = (GPIOB->MODER & ~(3U << 24)) | (1U << 24); /* Output mode */
    GPIOB->OTYPER &= ~(1U << 12);                              /* Push-Pull */
    GPIOB->OSPEEDR |= (3U << 24);                              /* Very High Speed */
    GPIOB->PUPDR &= ~(3U << 24);
    cs_high();                                                 /* Start HIGH (deselected) */

    /* 3. Configure PB13 (SCK) as AF5 */
    GPIOB->MODER = (GPIOB->MODER & ~(3U << 26)) | (2U << 26); /* Alternate Function */
    GPIOB->OTYPER &= ~(1U << 13);
    GPIOB->OSPEEDR |= (3U << 26);
    GPIOB->PUPDR &= ~(3U << 26);
    GPIOB->AFR[1] = (GPIOB->AFR[1] & ~(0xFU << 20)) | (5U << 20); /* AF5 */

    /* 4. Configure PC2 (MISO) & PC3 (MOSI) as AF5 */
    GPIOC->MODER = (GPIOC->MODER & ~((3U << 4) | (3U << 6))) |
                   ((2U << 4) | (2U << 6));                   /* Alternate Function */
    GPIOC->OTYPER &= ~((1U << 2) | (1U << 3));
    GPIOC->OSPEEDR |= ((3U << 4) | (3U << 6));
    GPIOC->PUPDR &= ~((3U << 4) | (3U << 6));
    GPIOC->AFR[0] = (GPIOC->AFR[0] & ~((0xFU << 8) | (0xFU << 12))) |
                    ((5U << 8) | (5U << 12));                 /* AF5 */

    /* 5. Configure SPI2:
     * Master mode, Software slave management (SSM=1, SSI=1),
     * Prescaler /2 (Baud = 30 MHz / 2 = 15 MHz), CPOL=0, CPHA=0, 8-bit.
     */
    SPI2->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI;
    SPI2->CR2 = 0;
    SPI2->CR1 |= SPI_CR1_SPE; /* Enable SPI2 */

    /* 6. Probe JEDEC ID */
    uint32_t id = w25q64_read_jedec_id();
    s_available = (id == W25Q64_JEDEC_ID);

    if (s_available) {
        w25q64_unlock();
        printk("[W25Q64] Flash detected & unlocked: JEDEC ID 0x%06X (Winbond 8MB SPI NOR Flash, 15 MHz)\n", (unsigned)id);
    } else {
        printk("[W25Q64] Probe failed: JEDEC ID 0x%06X (expected 0x%06X)\n", (unsigned)id, (unsigned)W25Q64_JEDEC_ID);
    }

    return s_available;
}

bool w25q64_is_available(void)
{
    return s_available;
}

uint32_t w25q64_read_jedec_id(void)
{
    cs_low();
    spi_transfer(CMD_JEDEC_ID);
    uint32_t mfr = spi_transfer(0xFF);
    uint32_t mem = spi_transfer(0xFF);
    uint32_t cap = spi_transfer(0xFF);
    cs_high();

    return (mfr << 16) | (mem << 8) | cap;
}

void w25q64_read(uint32_t addr, void *buf, size_t len)
{
    if (!buf || len == 0) return;
    uint8_t *dst = (uint8_t *)buf;

    cs_low();
    /* Fast Read (0x0B) with 1 dummy cycle allows full 15 MHz execution */
    spi_transfer(CMD_FAST_READ);
    spi_transfer((uint8_t)(addr >> 16));
    spi_transfer((uint8_t)(addr >> 8));
    spi_transfer((uint8_t)addr);
    spi_transfer(0xFF); /* Dummy byte */

    for (size_t i = 0; i < len; i++) {
        dst[i] = spi_transfer(0xFF);
    }
    cs_high();
}

void w25q64_erase_sector(uint32_t addr)
{
    write_enable();
    cs_low();
    spi_transfer(CMD_SECTOR_ERASE_4K);
    spi_transfer((uint8_t)(addr >> 16));
    spi_transfer((uint8_t)(addr >> 8));
    spi_transfer((uint8_t)addr);
    cs_high();

    wait_busy();
}

void w25q64_page_program(uint32_t addr, const void *buf, size_t len)
{
    if (!buf || len == 0) return;
    if (len > W25Q64_PAGE_SIZE) len = W25Q64_PAGE_SIZE;
    const uint8_t *src = (const uint8_t *)buf;

    write_enable();
    cs_low();
    spi_transfer(CMD_PAGE_PROGRAM);
    spi_transfer((uint8_t)(addr >> 16));
    spi_transfer((uint8_t)(addr >> 8));
    spi_transfer((uint8_t)addr);

    for (size_t i = 0; i < len; i++) {
        spi_transfer(src[i]);
    }
    cs_high();

    wait_busy();
}

void w25q64_write(uint32_t addr, const void *buf, size_t len)
{
    if (!buf || len == 0) return;
    const uint8_t *src = (const uint8_t *)buf;

    while (len > 0) {
        uint32_t page_offset = addr % W25Q64_PAGE_SIZE;
        uint32_t chunk = W25Q64_PAGE_SIZE - page_offset;
        if (chunk > len) chunk = len;

        w25q64_page_program(addr, src, chunk);
        addr += chunk;
        src += chunk;
        len -= chunk;
    }
}

#else
/* -------------------------------------------------------------------------- */
/* Host / Native Test Mock Emulation (RAM backing store)                      */
/* -------------------------------------------------------------------------- */
#define MOCK_NOR_SIZE   (256U * 1024U)
static uint8_t s_mock_nor[MOCK_NOR_SIZE];
static bool s_mock_inited = false;

uint8_t w25q64_read_status(void)
{
    return 0x00;
}

void w25q64_unlock(void)
{
}

bool w25q64_init(void)
{
    if (!s_mock_inited) {
        memset(s_mock_nor, 0xFF, sizeof(s_mock_nor));
        s_mock_inited = true;
    }
    return true;
}

bool w25q64_is_available(void)
{
    return true;
}

uint32_t w25q64_read_jedec_id(void)
{
    return W25Q64_JEDEC_ID;
}

void w25q64_read(uint32_t addr, void *buf, size_t len)
{
    if (addr + len <= MOCK_NOR_SIZE) {
        memcpy(buf, s_mock_nor + addr, len);
    } else {
        memset(buf, 0xFF, len);
    }
}

void w25q64_erase_sector(uint32_t addr)
{
    uint32_t sector_start = (addr / W25Q64_SECTOR_SIZE) * W25Q64_SECTOR_SIZE;
    if (sector_start + W25Q64_SECTOR_SIZE <= MOCK_NOR_SIZE) {
        memset(s_mock_nor + sector_start, 0xFF, W25Q64_SECTOR_SIZE);
    }
}

void w25q64_page_program(uint32_t addr, const void *buf, size_t len)
{
    if (addr + len <= MOCK_NOR_SIZE) {
        const uint8_t *src = (const uint8_t *)buf;
        for (size_t i = 0; i < len; i++) {
            s_mock_nor[addr + i] &= src[i]; /* Flash bit programming */
        }
    }
}

void w25q64_write(uint32_t addr, const void *buf, size_t len)
{
    if (addr + len <= MOCK_NOR_SIZE) {
        const uint8_t *src = (const uint8_t *)buf;
        for (size_t i = 0; i < len; i++) {
            s_mock_nor[addr + i] &= src[i];
        }
    }
}
#endif
