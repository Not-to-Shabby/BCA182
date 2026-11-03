/**
 * @file aht10.c
 * @brief AHT10 temperature/humidity driver over the shared bit-banged I2C bus.
 *
 * Protocol: trigger measurement 0xAC 0x33 0x00, wait >= 80 ms, burst-read 6 bytes;
 * status bit 7 must be 0 (calibration enabled flag in 0x1B init commands).
 */

#include "aht10.h"
#include "soft_i2c.h"

#if defined(__arm__) || defined(CONFIG_SOC_SERIES_STM32F4X) || defined(ZEPHYR_VERSION_CODE)
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#else
#include <stdio.h>
#define printk printf
#define k_msleep(ms)
#endif

static bool s_present;

bool aht10_init(void)
{
    s_present = false;

    uint8_t status = 0;
    if (!soft_i2c_read_regs(AHT10_I2C_ADDR, 0x00, &status, 1)) {
        printk("[AHT10] Not detected at 0x%02X (absent from kit)\n", AHT10_I2C_ADDR);
        return false;
    }

    /* Soft reset, then load the factory calibration coefficients */
    soft_i2c_write_reg(AHT10_I2C_ADDR, 0x00, 0xBA);
    k_msleep(20);
    soft_i2c_write_regs(AHT10_I2C_ADDR, 0xE1, (const uint8_t[]){ 0x08, 0x00 }, 2);
    k_msleep(10);
    s_present = true;
    printk("[AHT10] Detected at 0x%02X\n", AHT10_I2C_ADDR);
    return true;
}

bool aht10_read(aht10_reading_t *out)
{
    if (out == NULL || !s_present) {
        return false;
    }

    if (!soft_i2c_write_regs(AHT10_I2C_ADDR, 0xAC, (const uint8_t[]){ 0x33, 0x00 }, 2)) {
        return false;
    }
    k_msleep(80);

    uint8_t raw[6];
    if (!soft_i2c_read_regs(AHT10_I2C_ADDR, 0x00, raw, sizeof(raw))) {
        return false;
    }
    if (raw[0] & 0x80) {
        return false; /* Busy flag still set */
    }

    uint32_t hum = ((uint32_t)raw[1] << 12) | ((uint32_t)raw[2] << 4) | (raw[3] >> 4);
    uint32_t tem = (((uint32_t)raw[3] & 0x0F) << 16) | ((uint32_t)raw[4] << 8) | raw[5];

    out->humidity_pct = (float)hum * 100.0f / 1048576.0f;
    out->temperature_c = (float)tem * 200.0f / 1048576.0f - 50.0f;
    return true;
}

bool aht10_is_present(void)
{
    return s_present;
}
