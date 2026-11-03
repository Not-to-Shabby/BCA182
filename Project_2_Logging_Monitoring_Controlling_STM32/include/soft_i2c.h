/**
 * @file soft_i2c.h
 * @brief Bit-banged I2C master on PB10 (SCL) / PB11 (SDA) for the RT-Spark sensor bus.
 *
 * Factored from the verified Lab 3 ICM-20608 driver so the IMU, AHT10 and AP3216C
 * share one proven bus implementation. Clock stretching is tolerated by polling SDA
 * (all fitted slaves are simple sensors that never stretch).
 */

#ifndef SOFT_I2C_H
#define SOFT_I2C_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure PB10/PB11 as open-drain GPIOs with pull-ups and release the bus.
 */
void soft_i2c_init(void);

/**
 * @brief Write bytes to a 7-bit-addressed register.
 * @return true when every byte was ACKed.
 */
bool soft_i2c_write_regs(uint8_t dev_addr, uint8_t reg, const uint8_t *data, uint16_t len);

/**
 * @brief Write one register value.
 */
bool soft_i2c_write_reg(uint8_t dev_addr, uint8_t reg, uint8_t value);

/**
 * @brief Read bytes starting at a register (repeated-start burst read).
 * @return true when the transfer completed.
 */
bool soft_i2c_read_regs(uint8_t dev_addr, uint8_t reg, uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* SOFT_I2C_H */
