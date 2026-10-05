/**
 * ==============================================================================
 * InvenSense ICM-20608-G Driver Implementation
 * ==============================================================================
 * Target: RT-Thread Spark Development Board ("星火 1 号", STM32F407ZGT6)
 * Bus: Onboard Software I2C Bus on Port F:
 *      PF1 = SCL (Pin 11 of MCU)
 *      PF0 = SDA (Pin 10 of MCU)
 * Verified against RT-Thread Spark BSP (projects/03_driver_axis)
 */

#include "icm20608.h"
#include "uart_telemetry.h"

#if defined(__arm__) || defined(STM32F407xx) || defined(CONFIG_SOC_SERIES_STM32F4X) || defined(ZEPHYR_VERSION_CODE)
#include <stm32f4xx.h>

#define SCL_PIN     1   /* PF1 */
#define SDA_PIN     0   /* PF0 */

static uint8_t s_active_i2c_addr = ICM20608_I2C_ADDR; /* 0x68 or 0x69 */

static void i2c_gpio_init(void)
{
    /* Enable GPIOF clock on AHB1 */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOFEN;

    /* Configure PF1 (SCL) and PF0 (SDA) as Open-Drain Outputs with Pull-Ups */
    GPIOF->MODER = (GPIOF->MODER & ~((3U << (SCL_PIN * 2)) | (3U << (SDA_PIN * 2)))) |
                   ((1U << (SCL_PIN * 2)) | (1U << (SDA_PIN * 2))); /* Output mode */
    GPIOF->OTYPER |= (1U << SCL_PIN) | (1U << SDA_PIN);             /* Open-drain */
    GPIOF->OSPEEDR |= (3U << (SCL_PIN * 2)) | (3U << (SDA_PIN * 2)); /* Very high speed */
    GPIOF->PUPDR = (GPIOF->PUPDR & ~((3U << (SCL_PIN * 2)) | (3U << (SDA_PIN * 2)))) |
                   ((1U << (SCL_PIN * 2)) | (1U << (SDA_PIN * 2))); /* Pull-up */

    GPIOF->BSRR = (1U << SCL_PIN) | (1U << SDA_PIN);                /* Float lines HIGH (Idle) */
}

static inline void i2c_delay(void)
{
    /* ~4.5 us delay at 168 MHz for reliable 100 kHz standard-mode I2C */
    for (volatile int i = 0; i < 200; i++) {
        __NOP();
    }
}

static void i2c_start(void)
{
    GPIOF->BSRR = (1U << SCL_PIN) | (1U << SDA_PIN);
    i2c_delay();
    GPIOF->BSRR = (1U << (SDA_PIN + 16)); /* SDA LOW while SCL is HIGH */
    i2c_delay();
    GPIOF->BSRR = (1U << (SCL_PIN + 16)); /* SCL LOW */
    i2c_delay();
}

static void i2c_stop(void)
{
    GPIOF->BSRR = (1U << (SDA_PIN + 16)); /* SDA LOW */
    i2c_delay();
    GPIOF->BSRR = (1U << SCL_PIN);        /* SCL HIGH */
    i2c_delay();
    GPIOF->BSRR = (1U << SDA_PIN);        /* SDA HIGH while SCL is HIGH */
    i2c_delay();
}

static bool i2c_write_byte(uint8_t byte)
{
    for (int i = 7; i >= 0; i--) {
        if (byte & (1U << i)) {
            GPIOF->BSRR = (1U << SDA_PIN);        /* SDA HIGH */
        } else {
            GPIOF->BSRR = (1U << (SDA_PIN + 16)); /* SDA LOW */
        }
        i2c_delay();
        GPIOF->BSRR = (1U << SCL_PIN);            /* SCL HIGH (clock pulse) */
        i2c_delay();
        GPIOF->BSRR = (1U << (SCL_PIN + 16));     /* SCL LOW */
        i2c_delay();
    }

    /* Read ACK/NACK from slave */
    GPIOF->BSRR = (1U << SDA_PIN);                /* Release SDA for slave to pull LOW */
    i2c_delay();
    GPIOF->BSRR = (1U << SCL_PIN);                /* SCL HIGH */
    i2c_delay();
    bool ack = !(GPIOF->IDR & (1U << SDA_PIN));   /* ACK is LOW */
    GPIOF->BSRR = (1U << (SCL_PIN + 16));         /* SCL LOW */
    i2c_delay();

    return ack;
}

static uint8_t i2c_read_byte(bool send_ack)
{
    uint8_t byte = 0;
    GPIOF->BSRR = (1U << SDA_PIN); /* Release SDA for slave to drive */

    for (int i = 7; i >= 0; i--) {
        i2c_delay();
        GPIOF->BSRR = (1U << SCL_PIN); /* SCL HIGH */
        i2c_delay();
        if (GPIOF->IDR & (1U << SDA_PIN)) {
            byte |= (uint8_t)(1U << i);
        }
        GPIOF->BSRR = (1U << (SCL_PIN + 16)); /* SCL LOW */
    }

    /* Send ACK or NACK to slave */
    if (send_ack) {
        GPIOF->BSRR = (1U << (SDA_PIN + 16)); /* Drive SDA LOW (ACK) */
    } else {
        GPIOF->BSRR = (1U << SDA_PIN);        /* Release SDA HIGH (NACK) */
    }
    i2c_delay();
    GPIOF->BSRR = (1U << SCL_PIN);            /* SCL HIGH */
    i2c_delay();
    GPIOF->BSRR = (1U << (SCL_PIN + 16));     /* SCL LOW */
    GPIOF->BSRR = (1U << SDA_PIN);            /* Release SDA */
    i2c_delay();

    return byte;
}

static bool icm_write_reg(uint8_t reg, uint8_t val)
{
    i2c_start();
    if (!i2c_write_byte(s_active_i2c_addr << 1)) {
        i2c_stop();
        return false;
    }
    if (!i2c_write_byte(reg)) {
        i2c_stop();
        return false;
    }
    if (!i2c_write_byte(val)) {
        i2c_stop();
        return false;
    }
    i2c_stop();
    return true;
}

static bool icm_read_reg_addr(uint8_t addr, uint8_t reg, uint8_t *val)
{
    i2c_start();
    if (!i2c_write_byte(addr << 1)) {
        i2c_stop();
        return false;
    }
    if (!i2c_write_byte(reg)) {
        i2c_stop();
        return false;
    }

    /* Repeated start for read */
    i2c_start();
    if (!i2c_write_byte((addr << 1) | 0x01)) {
        i2c_stop();
        return false;
    }
    *val = i2c_read_byte(false); /* Send NACK to terminate single-byte read */
    i2c_stop();
    return true;
}

static bool icm_read_reg(uint8_t reg, uint8_t *val)
{
    return icm_read_reg_addr(s_active_i2c_addr, reg, val);
}

static bool icm_read_burst(uint8_t start_reg, uint8_t *buf, uint16_t len)
{
    if (!buf || len == 0) return false;

    i2c_start();
    if (!i2c_write_byte(s_active_i2c_addr << 1)) {
        i2c_stop();
        return false;
    }
    if (!i2c_write_byte(start_reg)) {
        i2c_stop();
        return false;
    }

    /* Repeated start for burst read */
    i2c_start();
    if (!i2c_write_byte((s_active_i2c_addr << 1) | 0x01)) {
        i2c_stop();
        return false;
    }

    for (uint16_t i = 0; i < len; i++) {
        bool send_ack = (i < len - 1);
        buf[i] = i2c_read_byte(send_ack);
    }
    i2c_stop();
    return true;
}

#else
/* Host native simulation stub for automated unit testing */
static void i2c_gpio_init(void) {}
static bool icm_write_reg(uint8_t reg, uint8_t val) { (void)reg; (void)val; return true; }
static bool icm_read_reg(uint8_t reg, uint8_t *val) { (void)reg; *val = ICM20608_WHO_AM_I_VAL; return true; }
static bool icm_read_burst(uint8_t start_reg, uint8_t *buf, uint16_t len) { (void)start_reg; (void)buf; (void)len; return true; }
#endif

bool icm20608_init(void)
{
    i2c_gpio_init();

    uint8_t who_am_i = 0;
    
    /* Try primary I2C address 0x68 (AD0 = 0) */
    s_active_i2c_addr = 0x68;
    if (icm_read_reg(ICM20608_REG_WHO_AM_I, &who_am_i) && (who_am_i == ICM20608_WHO_AM_I_VAL || who_am_i == 0xA8 || who_am_i == 0x98)) {
        uart1_printf("[ICM20608] Found at I2C address 0x68 (WHO_AM_I = 0x%02X)\n", who_am_i);
    } else {
        /* Try secondary I2C address 0x69 (AD0 = 1) */
        s_active_i2c_addr = 0x69;
        if (icm_read_reg(ICM20608_REG_WHO_AM_I, &who_am_i) && (who_am_i == ICM20608_WHO_AM_I_VAL || who_am_i == 0xA8 || who_am_i == 0x98)) {
            uart1_printf("[ICM20608] Found at I2C address 0x69 (WHO_AM_I = 0x%02X)\n", who_am_i);
        } else {
            uart1_printf("[ICM20608] Probe failed at 0x68 and 0x69 (read 0x%02X, expected 0xAF)\n", who_am_i);
            return false;
        }
    }

    /* Reset device (PWR_MGMT_1 bit 7 = DEVICE_RESET) */
    icm_write_reg(ICM20608_REG_PWR_MGMT_1, 0x80);
    for (volatile int i = 0; i < 10000; i++) __NOP();
    
    /* Auto select best clock source (PLL) */
    icm_write_reg(ICM20608_REG_PWR_MGMT_1, 0x01);
    
    /* Enable all accel and gyro axes */
    icm_write_reg(ICM20608_REG_PWR_MGMT_2, 0x00);
    
    /* DLPF config: Gyro 41 Hz bandwidth, delay 5.9 ms */
    icm_write_reg(ICM20608_REG_CONFIG, 0x03);
    
    /* Sample rate divider = 9 -> 100 Hz / (1 + 9) = 10 Hz output rate */
    icm_write_reg(ICM20608_REG_SMPLRT_DIV, 0x09);
    
    /* Gyro Full-Scale ±1000 dps (FS_SEL = 2 -> bits [4:3] = 10b = 0x10) */
    icm_write_reg(ICM20608_REG_GYRO_CONFIG, 0x10);
    
    /* Accel Full-Scale ±4g (AFS_SEL = 1 -> bits [4:3] = 01b = 0x08) */
    icm_write_reg(ICM20608_REG_ACCEL_CONFIG, 0x08);
    
    /* Accel DLPF: 44.8 Hz bandwidth */
    icm_write_reg(ICM20608_REG_ACCEL_CONFIG2, 0x03);

    return true;
}

bool icm20608_read_sample(imu_sample_t *out_sample)
{
    if (!out_sample) return false;

    uint8_t raw_buf[14];
    /* Burst read 14 bytes:
     * 0..5:   ACCEL_X, Y, Z
     * 6..7:   TEMP
     * 8..13:  GYRO_X, Y, Z
     */
    if (!icm_read_burst(ICM20608_REG_ACCEL_XOUT_H, raw_buf, 14)) {
        return false;
    }

    int16_t raw_ax = (int16_t)((raw_buf[0] << 8) | raw_buf[1]);
    int16_t raw_ay = (int16_t)((raw_buf[2] << 8) | raw_buf[3]);
    int16_t raw_az = (int16_t)((raw_buf[4] << 8) | raw_buf[5]);
    
    int16_t raw_gx = (int16_t)((raw_buf[8] << 8) | raw_buf[9]);
    int16_t raw_gy = (int16_t)((raw_buf[10] << 8) | raw_buf[11]);
    int16_t raw_gz = (int16_t)((raw_buf[12] << 8) | raw_buf[13]);

    /* Convert to standard gravity (g) */
    out_sample->ax = (float)raw_ax / ICM20608_ACCEL_SENS_4G;
    out_sample->ay = (float)raw_ay / ICM20608_ACCEL_SENS_4G;
    out_sample->az = (float)raw_az / ICM20608_ACCEL_SENS_4G;

    /* Convert to radians per second (rad/s) */
    out_sample->gx = ((float)raw_gx / ICM20608_GYRO_SENS_1000DPS) * DEG_TO_RAD;
    out_sample->gy = ((float)raw_gy / ICM20608_GYRO_SENS_1000DPS) * DEG_TO_RAD;
    out_sample->gz = ((float)raw_gz / ICM20608_GYRO_SENS_1000DPS) * DEG_TO_RAD;

    return true;
}
