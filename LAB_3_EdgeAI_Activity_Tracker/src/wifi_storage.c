/**
 * ==============================================================================
 * Wi-Fi Credentials Flash Persistence Implementation
 * ==============================================================================
 * Target: STM32F407ZGT6 on-chip Flash Sector 11 (0x080E0000 - 0x080FFFFF)
 * Voltage Range: 2.7V to 3.6V (32-bit parallelism PSIZE = 10b)
 * ==============================================================================
 */

#include "wifi_storage.h"
#include <string.h>

#if defined(__arm__) || defined(STM32F407xx) || defined(CONFIG_SOC_SERIES_STM32F4X) || defined(ZEPHYR_VERSION_CODE)
#include <stm32f4xx.h>
#include <zephyr/kernel.h>

#define FLASH_SECTOR_11_BASE    0x080E0000U
#define WIFI_STORAGE_MAGIC      0x57494649U /* "WIFI" */

#pragma pack(push, 4)
typedef struct {
    uint32_t magic;
    char     ssid[WIFI_SSID_MAX_LEN];
    char     pass[WIFI_PASS_MAX_LEN];
    uint32_t checksum;
} wifi_record_t;
#pragma pack(pop)

static uint32_t compute_record_checksum(const wifi_record_t *rec)
{
    const uint8_t *p = (const uint8_t *)rec;
    uint32_t sum = 0x12345678U;
    uint32_t len = sizeof(wifi_record_t) - sizeof(uint32_t);
    for (uint32_t i = 0; i < len; i++) {
        sum = (sum * 31U) + p[i];
    }
    return sum;
}

static void flash_unlock(void)
{
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = 0x45670123U;
        FLASH->KEYR = 0xCDEF89ABU;
    }
}

static void flash_lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

static void flash_flush_cache(void)
{
    FLASH->ACR &= ~(FLASH_ACR_ICEN | FLASH_ACR_DCEN);
    FLASH->ACR |= (FLASH_ACR_ICRST | FLASH_ACR_DCRST);
    FLASH->ACR &= ~(FLASH_ACR_ICRST | FLASH_ACR_DCRST);
    FLASH->ACR |= (FLASH_ACR_ICEN | FLASH_ACR_DCEN);
}

static void flash_wait_ready(void)
{
    while (FLASH->SR & FLASH_SR_BSY) {
        /* Busy wait */
    }
}

static bool flash_erase_sector_11(void)
{
    flash_unlock();
    flash_wait_ready();

    /* Clear any prior fault flags */
    FLASH->SR = (FLASH_SR_EOP | FLASH_SR_SOP | FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGPERR | FLASH_SR_PGSERR);

    /* Configure 32-bit parallelism (PSIZE = 10b) for 3.3V operation */
    FLASH->CR = (FLASH->CR & ~FLASH_CR_PSIZE) | FLASH_CR_PSIZE_1;

    /* Select Sector 11 and start erase */
    FLASH->CR |= FLASH_CR_SER | (11U << 3);
    FLASH->CR |= FLASH_CR_STRT;

    flash_wait_ready();
    FLASH->CR &= ~(FLASH_CR_SER | (11U << 3));
    flash_lock();
    flash_flush_cache();

    return !(FLASH->SR & (FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGPERR | FLASH_SR_PGSERR));
}

bool wifi_storage_read(char *out_ssid, char *out_pass)
{
    if (!out_ssid || !out_pass) return false;

    const wifi_record_t *rec = (const wifi_record_t *)FLASH_SECTOR_11_BASE;

    if (rec->magic != WIFI_STORAGE_MAGIC) {
        return false;
    }

    uint32_t expected_cks = compute_record_checksum(rec);
    if (rec->checksum != expected_cks) {
        return false;
    }

    strncpy(out_ssid, rec->ssid, WIFI_SSID_MAX_LEN - 1);
    out_ssid[WIFI_SSID_MAX_LEN - 1] = '\0';

    strncpy(out_pass, rec->pass, WIFI_PASS_MAX_LEN - 1);
    out_pass[WIFI_PASS_MAX_LEN - 1] = '\0';

    return (strlen(out_ssid) > 0);
}

bool wifi_storage_save(const char *ssid, const char *pass)
{
    if (!ssid || strlen(ssid) == 0) return false;

    wifi_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = WIFI_STORAGE_MAGIC;
    strncpy(rec.ssid, ssid, WIFI_SSID_MAX_LEN - 1);
    if (pass) {
        strncpy(rec.pass, pass, WIFI_PASS_MAX_LEN - 1);
    }
    rec.checksum = compute_record_checksum(&rec);

    unsigned int key = irq_lock();

    /* 1. Erase Sector 11 */
    if (!flash_erase_sector_11()) {
        irq_unlock(key);
        return false;
    }

    /* 2. Program Record Word-by-Word */
    flash_unlock();
    flash_wait_ready();

    FLASH->CR = (FLASH->CR & ~FLASH_CR_PSIZE) | FLASH_CR_PSIZE_1;
    FLASH->CR |= FLASH_CR_PG;

    const uint32_t *src = (const uint32_t *)&rec;
    volatile uint32_t *dst = (volatile uint32_t *)FLASH_SECTOR_11_BASE;
    uint32_t word_count = sizeof(wifi_record_t) / sizeof(uint32_t);

    for (uint32_t i = 0; i < word_count; i++) {
        dst[i] = src[i];
        flash_wait_ready();
    }

    FLASH->CR &= ~FLASH_CR_PG;
    flash_lock();
    flash_flush_cache();

    irq_unlock(key);

    /* 3. Verify Written Data */
    const wifi_record_t *check = (const wifi_record_t *)FLASH_SECTOR_11_BASE;
    return (check->magic == WIFI_STORAGE_MAGIC && check->checksum == rec.checksum);
}

void wifi_storage_clear(void)
{
    flash_erase_sector_11();
}

#else

/* Host native fallback for unit tests */
static char s_sim_ssid[32] = {0};
static char s_sim_pass[64] = {0};
static bool s_sim_valid = false;

bool wifi_storage_read(char *out_ssid, char *out_pass)
{
    if (!s_sim_valid || !out_ssid || !out_pass) return false;
    strcpy(out_ssid, s_sim_ssid);
    strcpy(out_pass, s_sim_pass);
    return true;
}

bool wifi_storage_save(const char *ssid, const char *pass)
{
    if (!ssid) return false;
    strncpy(s_sim_ssid, ssid, 31);
    if (pass) strncpy(s_sim_pass, pass, 63);
    s_sim_valid = true;
    return true;
}

void wifi_storage_clear(void)
{
    s_sim_valid = false;
}

#endif
