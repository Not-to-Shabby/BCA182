/**
 * @file ap3216c.c
 * @brief AP3216C ambient light / proximity driver over the shared bit-banged I2C bus.
 *
 * Registers: 0x00 system config (0x03 = ALS+PS enable), 0x0A/0x0B ALS data L/H,
 * 0x0C/0x0D PS data L/H. ALS range 0x00-0x0F of config 0x01; 0x00 = max range
 * (0.35 lux/count up to ~22.9k lux).
 */

#include "ap3216c.h"
#include "soft_i2c.h"

#if defined(__arm__) || defined(CONFIG_SOC_SERIES_STM32F4X) || defined(ZEPHYR_VERSION_CODE)
#include <zephyr/sys/printk.h>
#else
#include <stdio.h>
#define printk printf
#endif

static bool s_present;

bool ap3216c_init(void)
{
    s_present = false;

    uint8_t id = 0;
    if (!soft_i2c_read_regs(AP3216C_I2C_ADDR, 0x0A, &id, 1)) {
        printk("[AP3216C] Not detected at 0x%02X (absent from kit)\n", AP3216C_I2C_ADDR);
        return false;
    }

    /* Software reset, then enable ALS + PS in maximum range */
    soft_i2c_write_reg(AP3216C_I2C_ADDR, 0x00, 0x04);
    soft_i2c_write_reg(AP3216C_I2C_ADDR, 0x00, 0x03);
    soft_i2c_write_reg(AP3216C_I2C_ADDR, 0x01, 0x00);
    s_present = true;
    printk("[AP3216C] Detected at 0x%02X\n", AP3216C_I2C_ADDR);
    return true;
}

bool ap3216c_read(ap3216c_reading_t *out)
{
    if (out == NULL || !s_present) {
        return false;
    }

    uint8_t raw[4];
    if (!soft_i2c_read_regs(AP3216C_I2C_ADDR, 0x0A, raw, sizeof(raw))) {
        return false;
    }

    uint16_t als = ((uint16_t)(raw[1] & 0x0F) << 8) | raw[0];
    uint16_t ps = ((uint16_t)(raw[3] & 0x3F) << 8) | raw[2];

    out->lux = als;       /* ~0.35 lux per count in range 0 */
    out->proximity = ps;
    return true;
}

bool ap3216c_is_present(void)
{
    return s_present;
}
