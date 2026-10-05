/**
 * ==============================================================================
 * InvenSense ICM-20608-G 6-Axis Motion Sensor Driver
 * ==============================================================================
 * Interfaced via I2C2 on PB10 (SCL) and PB11 (SDA)
 * Configured for ±4g Accel Full-Scale and ±1000 dps Gyro Full-Scale
 * Outputs acceleration in standard gravity (g) and gyro in radians/second (rad/s)
 */

#ifndef ICM20608_H
#define ICM20608_H

#include <stdint.h>
#include <stdbool.h>
#include "feature_extractor.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ICM20608_I2C_ADDR           0x68    /* AD0 = 0 (Pin 4 pulled low) */
#define ICM20608_WHO_AM_I_VAL       0xAF    /* Expected Device ID */

/* Register Addresses */
#define ICM20608_REG_SMPLRT_DIV     0x19
#define ICM20608_REG_CONFIG         0x1A
#define ICM20608_REG_GYRO_CONFIG    0x1B
#define ICM20608_REG_ACCEL_CONFIG   0x1C
#define ICM20608_REG_ACCEL_CONFIG2  0x1D
#define ICM20608_REG_PWR_MGMT_1     0x6B
#define ICM20608_REG_PWR_MGMT_2     0x6C
#define ICM20608_REG_WHO_AM_I       0x75
#define ICM20608_REG_ACCEL_XOUT_H   0x3B

/* Scale Factor Conversions */
/* ±4g mode: 8192 LSB/g */
#define ICM20608_ACCEL_SENS_4G      8192.0f
/* ±1000 dps mode: 32.8 LSB/(deg/s) */
#define ICM20608_GYRO_SENS_1000DPS  32.8f
/* Deg/s to Rad/s multiplier: pi / 180 */
#define DEG_TO_RAD                  0.017453292519943295f

/**
 * @brief Initializes the ICM-20608 sensor over I2C2.
 * @return true if WHO_AM_I matched 0xAF and initialization succeeded.
 */
bool icm20608_init(void);

/**
 * @brief Reads all 6 axes in burst mode and converts to calibrated engineering units.
 * 
 * @param out_sample Output structure storing ax, ay, az (in g) and gx, gy, gz (in rad/s)
 * @return true if I2C burst read succeeded
 */
bool icm20608_read_sample(imu_sample_t *out_sample);

#ifdef __cplusplus
}
#endif

#endif /* ICM20608_H */
