/**
 * @file soft_i2c.c
 * @brief Bit-banged I2C masters shared by the RT-Spark onboard sensors.
 *
 * Two independent buses, matching the board schematic and the official
 * RT-Thread SDK demo (projects/03_driver_temp_humi):
 *   Bus 1 (soft_i2c_*):  PF1 = SCL, PF0 = SDA -- AP3216C (0x1E) + ICM20608 (0x68)
 *   Bus 2 (soft_i2c2_*): PE0 = SCL, PE1 = SDA -- AHT21 (0x38), the board's
 *                        "software I2C3 / i2c3" pair
 * Verified in Lab 3 bring-up on this exact board (bus 1). Open-drain outputs
 * with pull-ups, ~100 kHz. All transfers are single-master per bus; each bus
 * has its own mutex so whole transfers serialize without cross-bus blocking.
 */

#include "soft_i2c.h"

#if defined(__arm__) || defined(CONFIG_SOC_SERIES_STM32F4X) || defined(ZEPHYR_VERSION_CODE)
#include <stm32f4xx.h>
#include <zephyr/kernel.h>

/* One lock per bus: a transfer on the AHT bus never blocks sensor polling
 * on the AP3216C/ICM bus (and vice versa). */
static K_MUTEX_DEFINE(s_bus1_lock);
static K_MUTEX_DEFINE(s_bus2_lock);

typedef struct {
	GPIO_TypeDef *port;
	uint32_t scl_pin;
	uint32_t sda_pin;
	struct k_mutex *lock;
} i2c_bus_t;

/* Bus 1: PF1 = SCL, PF0 = SDA (Lab 3/4 proven pair). */
static const i2c_bus_t BUS1 = { GPIOF, 1U, 0U, &s_bus1_lock };
/* Bus 2: PE0 = SCL, PE1 = SDA (SDK demo 03_driver_temp_humi: "i2c3"). */
static const i2c_bus_t BUS2 = { GPIOE, 0U, 1U, &s_bus2_lock };

static void i2c_ll_init(const i2c_bus_t *bus)
{
	RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN |
			RCC_AHB1ENR_GPIOCEN | RCC_AHB1ENR_GPIOEEN |
			RCC_AHB1ENR_GPIOFEN;
	(void)RCC->AHB1ENR; /* settle after clock enable */

	const uint32_t mask = (3U << (bus->scl_pin * 2)) |
			      (3U << (bus->sda_pin * 2));
	bus->port->MODER = (bus->port->MODER & ~mask) |
			   ((1U << (bus->scl_pin * 2)) |
			    (1U << (bus->sda_pin * 2)));            /* Output  */
	bus->port->OTYPER |= (1U << bus->scl_pin) |
			     (1U << bus->sda_pin);                  /* Open-drain */
	bus->port->OSPEEDR |= mask;
	bus->port->PUPDR = (bus->port->PUPDR & ~mask) |
			   ((1U << (bus->scl_pin * 2)) |
			    (1U << (bus->sda_pin * 2)));            /* Pull-up */
	bus->port->BSRR = (1U << bus->scl_pin) |
			  (1U << bus->sda_pin);                     /* Idle high */
}

static inline void i2c_delay(void)
{
	for (volatile int i = 0; i < 200; i++) {
		__NOP();
	}
}

static void i2c_ll_start(const i2c_bus_t *bus)
{
	bus->port->BSRR = (1U << bus->scl_pin) | (1U << bus->sda_pin);
	i2c_delay();
	bus->port->BSRR = (1U << (bus->sda_pin + 16));
	i2c_delay();
	bus->port->BSRR = (1U << (bus->scl_pin + 16));
	i2c_delay();
}

static void i2c_ll_stop(const i2c_bus_t *bus)
{
	bus->port->BSRR = (1U << (bus->sda_pin + 16));
	i2c_delay();
	bus->port->BSRR = (1U << bus->scl_pin);
	i2c_delay();
	bus->port->BSRR = (1U << bus->sda_pin);
	i2c_delay();
}

static bool i2c_ll_write_byte(const i2c_bus_t *bus, uint8_t byte)
{
	for (int i = 7; i >= 0; i--) {
		if (byte & (1U << i)) {
			bus->port->BSRR = (1U << bus->sda_pin);
		} else {
			bus->port->BSRR = (1U << (bus->sda_pin + 16));
		}
		i2c_delay();
		bus->port->BSRR = (1U << bus->scl_pin);
		i2c_delay();
		bus->port->BSRR = (1U << (bus->scl_pin + 16));
		i2c_delay();
	}

	bus->port->BSRR = (1U << bus->sda_pin); /* Release SDA for the ACK */
	i2c_delay();
	bus->port->BSRR = (1U << bus->scl_pin);
	i2c_delay();
	bool ack = !(bus->port->IDR & (1U << bus->sda_pin));
	bus->port->BSRR = (1U << (bus->scl_pin + 16));
	i2c_delay();

	return ack;
}

static uint8_t i2c_ll_read_byte(const i2c_bus_t *bus, bool send_ack)
{
	uint8_t byte = 0;
	bus->port->BSRR = (1U << bus->sda_pin);

	for (int i = 7; i >= 0; i--) {
		i2c_delay();
		bus->port->BSRR = (1U << bus->scl_pin);
		i2c_delay();
		if (bus->port->IDR & (1U << bus->sda_pin)) {
			byte |= (uint8_t)(1U << i);
		}
		bus->port->BSRR = (1U << (bus->scl_pin + 16));
	}

	if (send_ack) {
		bus->port->BSRR = (1U << (bus->sda_pin + 16));
	} else {
		bus->port->BSRR = (1U << bus->sda_pin);
	}
	i2c_delay();
	bus->port->BSRR = (1U << bus->scl_pin);
	i2c_delay();
	bus->port->BSRR = (1U << (bus->scl_pin + 16));
	bus->port->BSRR = (1U << bus->sda_pin);
	i2c_delay();

	return byte;
}

/* ------------------------------------------------------------------ bus 1 API */
void soft_i2c_init(void)
{
	i2c_ll_init(&BUS1);
}

bool soft_i2c_write_regs(uint8_t dev_addr, uint8_t reg, const uint8_t *data, uint16_t len)
{
	if (data == NULL || len == 0) {
		return false;
	}

	bool ok;
	k_mutex_lock(BUS1.lock, K_FOREVER);
	i2c_ll_start(&BUS1);
	ok = i2c_ll_write_byte(&BUS1, (uint8_t)(dev_addr << 1));
	if (ok) {
		ok = i2c_ll_write_byte(&BUS1, reg);
	}
	for (uint16_t i = 0; ok && i < len; i++) {
		ok = i2c_ll_write_byte(&BUS1, data[i]);
	}
	i2c_ll_stop(&BUS1);
	k_mutex_unlock(BUS1.lock);
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
	k_mutex_lock(BUS1.lock, K_FOREVER);
	i2c_ll_start(&BUS1);
	ok = i2c_ll_write_byte(&BUS1, (uint8_t)(dev_addr << 1)) &&
	     i2c_ll_write_byte(&BUS1, reg);
	if (ok) {
		i2c_ll_start(&BUS1); /* Repeated start for the read phase */
		ok = i2c_ll_write_byte(&BUS1, (uint8_t)((dev_addr << 1) | 0x01));
	}
	if (ok) {
		for (uint16_t i = 0; i < len; i++) {
			data[i] = i2c_ll_read_byte(&BUS1, i < (uint16_t)(len - 1));
		}
	}
	i2c_ll_stop(&BUS1);
	k_mutex_unlock(BUS1.lock);
	return ok;
}

bool soft_i2c_probe(uint8_t dev_addr)
{
	bool ok;
	k_mutex_lock(BUS1.lock, K_FOREVER);
	i2c_ll_start(&BUS1);
	ok = i2c_ll_write_byte(&BUS1, (uint8_t)(dev_addr << 1));
	i2c_ll_stop(&BUS1);
	k_mutex_unlock(BUS1.lock);
	return ok;
}

bool soft_i2c_write_raw(uint8_t dev_addr, const uint8_t *data, uint16_t len)
{
	if (data == NULL || len == 0) {
		return false;
	}

	bool ok;
	k_mutex_lock(BUS1.lock, K_FOREVER);
	i2c_ll_start(&BUS1);
	ok = i2c_ll_write_byte(&BUS1, (uint8_t)(dev_addr << 1));
	for (uint16_t i = 0; ok && i < len; i++) {
		ok = i2c_ll_write_byte(&BUS1, data[i]);
	}
	i2c_ll_stop(&BUS1);
	k_mutex_unlock(BUS1.lock);
	return ok;
}

bool soft_i2c_read_raw(uint8_t dev_addr, uint8_t *data, uint16_t len)
{
	if (data == NULL || len == 0) {
		return false;
	}

	bool ok;
	k_mutex_lock(BUS1.lock, K_FOREVER);
	i2c_ll_start(&BUS1);
	ok = i2c_ll_write_byte(&BUS1, (uint8_t)((dev_addr << 1) | 0x01));
	if (ok) {
		for (uint16_t i = 0; i < len; i++) {
			data[i] = i2c_ll_read_byte(&BUS1, i < (uint16_t)(len - 1));
		}
	}
	i2c_ll_stop(&BUS1);
	k_mutex_unlock(BUS1.lock);
	return ok;
}

/* ------------------------------------------------------------------ bus 2 API */
void soft_i2c2_init(void)
{
	i2c_ll_init(&BUS2);
}

bool soft_i2c2_probe(uint8_t dev_addr)
{
	bool ok;
	k_mutex_lock(BUS2.lock, K_FOREVER);
	i2c_ll_start(&BUS2);
	ok = i2c_ll_write_byte(&BUS2, (uint8_t)(dev_addr << 1));
	i2c_ll_stop(&BUS2);
	k_mutex_unlock(BUS2.lock);
	return ok;
}

bool soft_i2c2_write_raw(uint8_t dev_addr, const uint8_t *data, uint16_t len)
{
	if (data == NULL || len == 0) {
		return false;
	}

	bool ok;
	k_mutex_lock(BUS2.lock, K_FOREVER);
	i2c_ll_start(&BUS2);
	ok = i2c_ll_write_byte(&BUS2, (uint8_t)(dev_addr << 1));
	for (uint16_t i = 0; ok && i < len; i++) {
		ok = i2c_ll_write_byte(&BUS2, data[i]);
	}
	i2c_ll_stop(&BUS2);
	k_mutex_unlock(BUS2.lock);
	return ok;
}

bool soft_i2c2_read_raw(uint8_t dev_addr, uint8_t *data, uint16_t len)
{
	if (data == NULL || len == 0) {
		return false;
	}

	bool ok;
	k_mutex_lock(BUS2.lock, K_FOREVER);
	i2c_ll_start(&BUS2);
	ok = i2c_ll_write_byte(&BUS2, (uint8_t)((dev_addr << 1) | 0x01));
	if (ok) {
		for (uint16_t i = 0; i < len; i++) {
			data[i] = i2c_ll_read_byte(&BUS2, i < (uint16_t)(len - 1));
		}
	}
	i2c_ll_stop(&BUS2);
	k_mutex_unlock(BUS2.lock);
	return ok;
}

#else
/* Host native stubs: the sensor buses are not simulated in unit tests. */
void soft_i2c_init(void) {}
void soft_i2c2_init(void) {}

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

bool soft_i2c_probe(uint8_t dev_addr)
{
	(void)dev_addr;
	return false;
}

bool soft_i2c_write_raw(uint8_t dev_addr, const uint8_t *data, uint16_t len)
{
	(void)dev_addr; (void)data; (void)len;
	return false;
}

bool soft_i2c_read_raw(uint8_t dev_addr, uint8_t *data, uint16_t len)
{
	(void)dev_addr; (void)data; (void)len;
	return false;
}

bool soft_i2c2_probe(uint8_t dev_addr)
{
	(void)dev_addr;
	return false;
}

bool soft_i2c2_write_raw(uint8_t dev_addr, const uint8_t *data, uint16_t len)
{
	(void)dev_addr; (void)data; (void)len;
	return false;
}

bool soft_i2c2_read_raw(uint8_t dev_addr, uint8_t *data, uint16_t len)
{
	(void)dev_addr; (void)data; (void)len;
	return false;
}
#endif
