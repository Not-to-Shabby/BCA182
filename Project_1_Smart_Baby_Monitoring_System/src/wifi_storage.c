/**
 * ==============================================================================
 * Wi-Fi + MQTT-Broker Credentials Flash Persistence Implementation (v2)
 * ==============================================================================
 * Ported from the LAB_3 Edge AI Activity Tracker project and EXTENDED for the
 * P4 Smart Baby Monitoring System captive portal: the saved record now also
 * carries the MQTT broker host/port captured by the portal form.
 *
 * Target: STM32F407ZGT6 on-chip Flash Sector 11 (0x080E0000 - 0x080FFFFF)
 * Voltage Range: 2.7V to 3.6V (32-bit parallelism PSIZE = 10b)
 *
 * Record v2 migration: the v1 record used magic "WIFI" (0x57494649) with
 * ssid+pass+checksum only. v2 changes the magic to "WIF2" (0x57494632) and
 * adds an explicit version word, so a leftover v1 sector fails the magic
 * check and reads as "no saved credentials" -- the setup portal runs on the
 * next boot and the first successful save overwrites the stale sector.
 *
 * NOTE: save/clear run under irq_lock() and stall every thread while the
 * erase (~1-2 s) and program complete -- never call during MQTT traffic;
 * only the portal/save path may use it.
 * ==============================================================================
 */

#include "wifi_storage.h"
#include <string.h>

#if defined(__arm__) || defined(STM32F407xx) || defined(CONFIG_SOC_SERIES_STM32F4X) || defined(ZEPHYR_VERSION_CODE)
#include <stm32f4xx.h>
#include <zephyr/kernel.h>

#define FLASH_SECTOR_11_BASE    0x080E0000U
#define WIFI_STORAGE_MAGIC      0x57494632U /* "WIF2" (v1 was "WIFI") */
#define WIFI_STORAGE_VERSION    2U

#pragma pack(push, 4)
typedef struct {
    uint32_t magic;                    /* "WIF2" -- v1 records fail this check */
    uint32_t version;                  /* 2 */
    char     ssid[WIFI_SSID_MAX_LEN];
    char     pass[WIFI_PASS_MAX_LEN];
    char     broker_host[WIFI_BROKER_HOST_MAX_LEN];
    uint16_t broker_port;              /* 0 = no broker saved */
    uint16_t reserved;                 /* keep checksum word 4-byte aligned  */
    uint32_t checksum;
} wifi_record_t;                       /* 176 bytes, sector 11 is 128 KB     */
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

/* Shared reader: validates magic/version/checksum, returns the raw record. */
static bool record_valid(const wifi_record_t *rec)
{
    if (rec->magic != WIFI_STORAGE_MAGIC || rec->version != WIFI_STORAGE_VERSION) {
        return false;
    }
    return (rec->checksum == compute_record_checksum(rec));
}

bool wifi_storage_read(char *out_ssid, char *out_pass)
{
    if (!out_ssid || !out_pass) return false;

    const wifi_record_t *rec = (const wifi_record_t *)FLASH_SECTOR_11_BASE;

    if (!record_valid(rec)) {
        return false;
    }

    strncpy(out_ssid, rec->ssid, WIFI_SSID_MAX_LEN - 1);
    out_ssid[WIFI_SSID_MAX_LEN - 1] = '\0';

    strncpy(out_pass, rec->pass, WIFI_PASS_MAX_LEN - 1);
    out_pass[WIFI_PASS_MAX_LEN - 1] = '\0';

    return (strlen(out_ssid) > 0);
}

bool wifi_storage_get_broker(char *host, size_t host_len, uint16_t *port)
{
    if (!host || host_len == 0 || !port) {
        return false;
    }

    const wifi_record_t *rec = (const wifi_record_t *)FLASH_SECTOR_11_BASE;

    if (!record_valid(rec) || rec->broker_host[0] == '\0' || rec->broker_port == 0) {
        return false; /* no valid record or no broker preference saved */
    }

    strncpy(host, rec->broker_host, host_len - 1);
    host[host_len - 1] = '\0';
    *port = rec->broker_port;
    return true;
}

bool wifi_storage_save_ex(const char *ssid, const char *pass,
                          const char *broker_host, uint16_t broker_port)
{
    if (!ssid || strlen(ssid) == 0) return false;

    wifi_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = WIFI_STORAGE_MAGIC;
    rec.version = WIFI_STORAGE_VERSION;
    strncpy(rec.ssid, ssid, WIFI_SSID_MAX_LEN - 1);
    if (pass) {
        strncpy(rec.pass, pass, WIFI_PASS_MAX_LEN - 1);
    }
    if (broker_host && broker_host[0] != '\0' && broker_port != 0) {
        strncpy(rec.broker_host, broker_host, WIFI_BROKER_HOST_MAX_LEN - 1);
        rec.broker_port = broker_port;
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
    return (check->magic == WIFI_STORAGE_MAGIC && check->version == WIFI_STORAGE_VERSION &&
            check->checksum == rec.checksum);
}

bool wifi_storage_save(const char *ssid, const char *pass)
{
    return wifi_storage_save_ex(ssid, pass, NULL, 0);
}

void wifi_storage_clear(void)
{
    flash_erase_sector_11();
}

#else

/* Host native fallback for unit tests */
static char     s_sim_ssid[32] = {0};
static char     s_sim_pass[64] = {0};
static char     s_sim_broker[WIFI_BROKER_HOST_MAX_LEN] = {0};
static uint16_t s_sim_broker_port;
static bool     s_sim_valid = false;

bool wifi_storage_read(char *out_ssid, char *out_pass)
{
    if (!s_sim_valid || !out_ssid || !out_pass) return false;
    strcpy(out_ssid, s_sim_ssid);
    strcpy(out_pass, s_sim_pass);
    return true;
}

bool wifi_storage_get_broker(char *host, size_t host_len, uint16_t *port)
{
    if (!s_sim_valid || !host || host_len == 0 || !port) return false;
    if (s_sim_broker[0] == '\0' || s_sim_broker_port == 0) return false;
    strncpy(host, s_sim_broker, host_len - 1);
    host[host_len - 1] = '\0';
    *port = s_sim_broker_port;
    return true;
}

bool wifi_storage_save_ex(const char *ssid, const char *pass,
                          const char *broker_host, uint16_t broker_port)
{
    if (!ssid) return false;
    strncpy(s_sim_ssid, ssid, 31);
    if (pass) strncpy(s_sim_pass, pass, 63);
    if (broker_host && broker_host[0] != '\0' && broker_port != 0) {
        strncpy(s_sim_broker, broker_host, WIFI_BROKER_HOST_MAX_LEN - 1);
        s_sim_broker_port = broker_port;
    } else {
        s_sim_broker[0] = '\0';
        s_sim_broker_port = 0;
    }
    s_sim_valid = true;
    return true;
}

bool wifi_storage_save(const char *ssid, const char *pass)
{
    return wifi_storage_save_ex(ssid, pass, NULL, 0);
}

void wifi_storage_clear(void)
{
    s_sim_valid = false;
}

#endif
