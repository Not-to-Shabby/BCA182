/**
 * @file icm20608.c
 * @brief ICM-20608 vibration-sensing driver over the shared bit-banged I2C bus.
 */

#include "icm20608.h"
#include "soft_i2c.h"

#if defined(__arm__) || defined(CONFIG_SOC_SERIES_STM32F4X) || defined(ZEPHYR_VERSION_CODE)
#include <zephyr/sys/printk.h>
#include <stm32f4xx.h>
#else
#include <stdio.h>
#define printk printf
#endif

static uint8_t s_active_addr;

bool icm20608_init(void)
{
    soft_i2c_init();
    s_active_addr = 0;

    uint8_t id = 0;
    const uint8_t addrs[2] = { ICM20608_I2C_ADDR_1, ICM20608_I2C_ADDR_2 };
    for (unsigned i = 0; i < 2; i++) {
        if (soft_i2c_read_regs(addrs[i], ICM20608_REG_WHO_AM_I, &id, 1) &&
            (id == ICM20608_WHO_AM_I_VAL || id == 0xA8 || id == 0x98)) {
            s_active_addr = addrs[i];
            printk("[ICM20608] Found at 0x%02X (WHO_AM_I=0x%02X)\n", s_active_addr, id);
            break;
        }
    }
    if (s_active_addr == 0) {
        printk("[ICM20608] Probe failed at 0x68/0x69 (read 0x%02X)\n", id);
        return false;
    }

    /* Reset, then wake with PLL clock and all axes enabled */
    soft_i2c_write_reg(s_active_addr, ICM20608_REG_PWR_MGMT_1, 0x80);
    for (volatile int i = 0; i < 20000; i++) {
        __NOP();
    }
    soft_i2c_write_reg(s_active_addr, ICM20608_REG_PWR_MGMT_1, 0x01);
    soft_i2c_write_reg(s_active_addr, ICM20608_REG_PWR_MGMT_2, 0x00);

    /* Full-rate internal sampling; external polling at 100 Hz */
    soft_i2c_write_reg(s_active_addr, ICM20608_REG_SMPLRT_DIV, 0x00);
    /* Gyro: +/-1000 dps, mid-band DLPF */
    soft_i2c_write_reg(s_active_addr, ICM20608_REG_CONFIG, 0x02);
    soft_i2c_write_reg(s_active_addr, ICM20608_REG_GYRO_CONFIG, 0x10);
    /* Accel: +/-8 g full scale, mid-band DLPF so >=50 Hz appliance content survives */
    soft_i2c_write_reg(s_active_addr, ICM20608_REG_ACCEL_CONFIG, 0x10);
    soft_i2c_write_reg(s_active_addr, ICM20608_REG_ACCEL_CONFIG2, 0x02);
    return true;
}

bool icm20608_read_sample(imu_sample_t *out_sample)
{
    if (out_sample == NULL || s_active_addr == 0) {
        return false;
    }

    uint8_t raw[14];
    if (!soft_i2c_read_regs(s_active_addr, ICM20608_REG_ACCEL_XOUT_H, raw, sizeof(raw))) {
        return false;
    }

    const float accel_lsb = 1.0f / ICM20608_ACCEL_SENS_8G;
    const float gyro_lsb = DEG_TO_RAD / ICM20608_GYRO_SENS_1000DPS;

    int16_t ax = (int16_t)(((uint16_t)raw[0] << 8) | raw[1]);
    int16_t ay = (int16_t)(((uint16_t)raw[2] << 8) | raw[3]);
    int16_t az = (int16_t)(((uint16_t)raw[4] << 8) | raw[5]);
    int16_t gx = (int16_t)(((uint16_t)raw[8] << 8) | raw[9]);
    int16_t gy = (int16_t)(((uint16_t)raw[10] << 8) | raw[11]);
    int16_t gz = (int16_t)(((uint16_t)raw[12] << 8) | raw[13]);

    out_sample->ax = (float)ax * accel_lsb;
    out_sample->ay = (float)ay * accel_lsb;
    out_sample->az = (float)az * accel_lsb;
    out_sample->gx = (float)gx * gyro_lsb;
    out_sample->gy = (float)gy * gyro_lsb;
    out_sample->gz = (float)gz * gyro_lsb;
    return true;
}

uint8_t icm20608_get_address(void)
{
    return s_active_addr;
}
