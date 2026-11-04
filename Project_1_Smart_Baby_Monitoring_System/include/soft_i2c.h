/**
 * @file soft_i2c.h
 * @brief Bit-banged I2C master on PF1 (SCL) / PF0 (SDA) for the RT-Spark sensor bus.
 *
 * Two independent buses (factored from the verified Lab 3 ICM-20608 driver):
 *   bus 1  soft_i2c_*   PF1/PF0  AP3216C + ICM20608
 *   bus 2  soft_i2c2_*  PE0/PE1  AHT21 (the board's software "I2C3")
 * Clock stretching is tolerated by polling SDA (all fitted slaves are simple
 * sensors that never stretch).
 */

#ifndef SOFT_I2C_H
#define SOFT_I2C_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure PF1/PF0 as open-drain GPIOs with pull-ups and release the bus.
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

/**
 * @brief Address-only probe: true when the device ACKs its address.
 */
bool soft_i2c_probe(uint8_t dev_addr);

/**
 * @brief Raw command write (no register pointer): START, addr+W, data..., STOP.
 *        Used by command-style chips (AHT2x) whose writes are commands, not
 *        register pointers.
 * @return true when every byte was ACKed.
 */
bool soft_i2c_write_raw(uint8_t dev_addr, const uint8_t *data, uint16_t len);

/**
 * @brief Raw read with no preceding pointer write: START, addr+R, data..., STOP.
 *        Companion to soft_i2c_write_raw for command-style chips.
 * @return true when the transfer completed.
 */
bool soft_i2c_read_raw(uint8_t dev_addr, uint8_t *data, uint16_t len);

/* ---- Bus 2 (soft_i2c2_*): PE0 = SCL, PE1 = SDA -- the board's software
 *      "I2C3 / i2c3" pair carrying the onboard AHT21 (0x38). Command-style
 *      primitives only; the AHT2x family needs no register-pointer forms. --- */

/**
 * @brief Configure PE0/PE1 as open-drain GPIOs with pull-ups (idempotent).
 */
void soft_i2c2_init(void);

/**
 * @brief Address-only probe on bus 2: true when the device ACKs its address.
 */
bool soft_i2c2_probe(uint8_t dev_addr);

/**
 * @brief Raw command write on bus 2 (no register pointer).
 * @return true when every byte was ACKed.
 */
bool soft_i2c2_write_raw(uint8_t dev_addr, const uint8_t *data, uint16_t len);

/**
 * @brief Raw read on bus 2 with no preceding pointer write.
 * @return true when the transfer completed.
 */
bool soft_i2c2_read_raw(uint8_t dev_addr, uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* SOFT_I2C_H */
