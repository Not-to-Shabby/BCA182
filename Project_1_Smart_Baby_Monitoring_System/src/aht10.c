/**
 * @file aht10.c
 * @brief AHT2x temperature/humidity driver over the shared bit-banged I2C bus.
 *
 * The RT-Spark fits an AHT21 (schematic sheet 6: "U8 AHT21", addr 0x38); this
 * driver covers the AHT10/AHT20/AHT21 family -- same address, measurement and
 * status protocol, differing only in the factory-calibration opcode.
 *
 * Command-style chip: writes are raw command streams (no register pointer)
 * and reads are plain master reads, both via the soft_i2c raw primitives.
 * Sequence: soft reset 0xBA, status read (bit 7 = busy, bit 3 = calibrated),
 * factory-calibrate 0xE1 (AHT10) / 0xBE (AHT21) + 0x08 0x00, trigger
 * measurement 0xAC 0x33 0x00, wait >= 80 ms, burst-read 6 bytes.
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

    /* Soft reset first: some AHT2x parts do not ACK reads until after it. */
    if (!soft_i2c_write_raw(AHT10_I2C_ADDR, (const uint8_t[]){ 0xBA }, 1)) {
        printk("[AHT10] Not detected at 0x%02X (absent from kit)\n", AHT10_I2C_ADDR);
        return false;
    }
    k_msleep(20);

    uint8_t status = 0;
    if (!soft_i2c_read_raw(AHT10_I2C_ADDR, &status, 1)) {
        printk("[AHT10] Not detected at 0x%02X (absent from kit)\n", AHT10_I2C_ADDR);
        return false;
    }

    /* Load the factory calibration coefficients; verify the calib-enabled
     * status bit (bit 3), trying the AHT10 opcode then the AHT21 opcode. */
    static const uint8_t init_cmds[2] = { 0xE1, 0xBE };
    bool calibrated = false;
    for (int i = 0; i < 2 && !calibrated; i++) {
        if (soft_i2c_write_raw(AHT10_I2C_ADDR,
                               (const uint8_t[]){ init_cmds[i], 0x08, 0x00 },
                               3)) {
            k_msleep(10);
            if (soft_i2c_read_raw(AHT10_I2C_ADDR, &status, 1) &&
                (status & 0x08)) {
                calibrated = true;
            }
        }
    }
    if (!calibrated) {
        printk("[AHT10] Detected at 0x%02X but calibration failed\n",
               AHT10_I2C_ADDR);
        return false;
    }

    s_present = true;
    printk("[AHT10] Detected at 0x%02X (AHT2x, calib ok)\n", AHT10_I2C_ADDR);
    return true;
}

bool aht10_read(aht10_reading_t *out)
{
    if (out == NULL || !s_present) {
        return false;
    }

    if (!soft_i2c_write_raw(AHT10_I2C_ADDR, (const uint8_t[]){ 0xAC, 0x33, 0x00 }, 3)) {
        return false;
    }
    k_msleep(80);

    uint8_t raw[6];
    if (!soft_i2c_read_raw(AHT10_I2C_ADDR, raw, sizeof(raw))) {
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
