/**
 * @file soft_i2c.c
 * @brief Bit-banged I2C master shared by all RT-Spark onboard sensors.
 *
 * Physical bus (verified in Lab 3 bring-up on this exact board):
 *   PF1 = SCL, PF0 = SDA, open-drain outputs with pull-ups, ~100 kHz.
 * All transfers are single-master; the fitted sensors never clock-stretch.
 */

#include "soft_i2c.h"

#if defined(__arm__) || defined(CONFIG_SOC_SERIES_STM32F4X) || defined(ZEPHYR_VERSION_CODE)
#include <stm32f4xx.h>
#include <zephyr/kernel.h>

/* The sampler thread (IMU, 100 Hz) and the inference thread (AHT10/AP3216C)
 * both drive this bus; FatFs-style single mutex serializes whole transfers. */
static K_MUTEX_DEFINE(s_bus_lock);

#define SCL_PIN 1 /* PF1 */
#define SDA_PIN 0 /* PF0 */

static void i2c_gpio_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOFEN;

    GPIOF->MODER = (GPIOF->MODER & ~((3U << (SCL_PIN * 2)) | (3U << (SDA_PIN * 2)))) |
                   ((1U << (SCL_PIN * 2)) | (1U << (SDA_PIN * 2))); /* Output mode */
    GPIOF->OTYPER |= (1U << SCL_PIN) | (1U << SDA_PIN);             /* Open-drain */
    GPIOF->OSPEEDR |= (3U << (SCL_PIN * 2)) | (3U << (SDA_PIN * 2));
    GPIOF->PUPDR = (GPIOF->PUPDR & ~((3U << (SCL_PIN * 2)) | (3U << (SDA_PIN * 2)))) |
                   ((1U << (SCL_PIN * 2)) | (1U << (SDA_PIN * 2))); /* Pull-up */

    GPIOF->BSRR = (1U << SCL_PIN) | (1U << SDA_PIN); /* Idle high */
}

static inline void i2c_delay(void)
{
    for (volatile int i = 0; i < 200; i++) {
        __NOP();
    }
}

static void i2c_start(void)
{
    GPIOF->BSRR = (1U << SCL_PIN) | (1U << SDA_PIN);
    i2c_delay();
    GPIOF->BSRR = (1U << (SDA_PIN + 16));
    i2c_delay();
    GPIOF->BSRR = (1U << (SCL_PIN + 16));
    i2c_delay();
}

static void i2c_stop(void)
{
    GPIOF->BSRR = (1U << (SDA_PIN + 16));
    i2c_delay();
    GPIOF->BSRR = (1U << SCL_PIN);
    i2c_delay();
    GPIOF->BSRR = (1U << SDA_PIN);
    i2c_delay();
}

static bool i2c_write_byte(uint8_t byte)
{
    for (int i = 7; i >= 0; i--) {
        if (byte & (1U << i)) {
            GPIOF->BSRR = (1U << SDA_PIN);
        } else {
            GPIOF->BSRR = (1U << (SDA_PIN + 16));
        }
        i2c_delay();
        GPIOF->BSRR = (1U << SCL_PIN);
        i2c_delay();
        GPIOF->BSRR = (1U << (SCL_PIN + 16));
        i2c_delay();
    }

    GPIOF->BSRR = (1U << SDA_PIN); /* Release SDA for the ACK */
    i2c_delay();
    GPIOF->BSRR = (1U << SCL_PIN);
    i2c_delay();
    bool ack = !(GPIOF->IDR & (1U << SDA_PIN));
    GPIOF->BSRR = (1U << (SCL_PIN + 16));
    i2c_delay();

    return ack;
}

static uint8_t i2c_read_byte(bool send_ack)
{
    uint8_t byte = 0;
    GPIOF->BSRR = (1U << SDA_PIN);

    for (int i = 7; i >= 0; i--) {
        i2c_delay();
        GPIOF->BSRR = (1U << SCL_PIN);
        i2c_delay();
        if (GPIOF->IDR & (1U << SDA_PIN)) {
            byte |= (uint8_t)(1U << i);
        }
        GPIOF->BSRR = (1U << (SCL_PIN + 16));
    }

    if (send_ack) {
        GPIOF->BSRR = (1U << (SDA_PIN + 16));
    } else {
        GPIOF->BSRR = (1U << SDA_PIN);
    }
    i2c_delay();
    GPIOF->BSRR = (1U << SCL_PIN);
    i2c_delay();
    GPIOF->BSRR = (1U << (SCL_PIN + 16));
    GPIOF->BSRR = (1U << SDA_PIN);
    i2c_delay();

    return byte;
}

void soft_i2c_init(void)
{
    i2c_gpio_init();
}

bool soft_i2c_write_regs(uint8_t dev_addr, uint8_t reg, const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) {
        return false;
    }

    bool ok;
    k_mutex_lock(&s_bus_lock, K_FOREVER);
    i2c_start();
    ok = i2c_write_byte((uint8_t)(dev_addr << 1));
    if (ok) {
        ok = i2c_write_byte(reg);
    }
    for (uint16_t i = 0; ok && i < len; i++) {
        ok = i2c_write_byte(data[i]);
    }
    i2c_stop();
    k_mutex_unlock(&s_bus_lock);
    return ok;
}

bool soft_i2c_write_reg(uint8_t dev_addr, uint8_t reg, uint8_t value)
{
    return soft_i2c_write_regs(dev_addr, reg, &value, 1);
}

bool soft_i2c_read_regs(uint8_t dev_addr, uint8_t reg, uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) {
        return false;
    }

    bool ok;
    k_mutex_lock(&s_bus_lock, K_FOREVER);
    i2c_start();
    ok = i2c_write_byte((uint8_t)(dev_addr << 1)) && i2c_write_byte(reg);
    if (ok) {
        i2c_start(); /* Repeated start for the read phase */
        ok = i2c_write_byte((uint8_t)((dev_addr << 1) | 0x01));
    }
    if (ok) {
        for (uint16_t i = 0; i < len; i++) {
            data[i] = i2c_read_byte(i < (uint16_t)(len - 1));
        }
    }
    i2c_stop();
    k_mutex_unlock(&s_bus_lock);
    return ok;
}

#else
/* Host native stubs: the sensor bus is not simulated in unit tests. */
void soft_i2c_init(void) {}

bool soft_i2c_write_regs(uint8_t dev_addr, uint8_t reg, const uint8_t *data, uint16_t len)
{
    (void)dev_addr; (void)reg; (void)data; (void)len;
    return false;
}

bool soft_i2c_write_reg(uint8_t dev_addr, uint8_t reg, uint8_t value)
{
    (void)dev_addr; (void)reg; (void)value;
    return false;
}

bool soft_i2c_read_regs(uint8_t dev_addr, uint8_t reg, uint8_t *data, uint16_t len)
{
    (void)dev_addr; (void)reg; (void)data; (void)len;
    return false;
}
#endif
