/**
 * ==============================================================================
 * InvenSense ICM-20608-G 6-Axis Vibration Sensor Driver (Laboratory 4)
 * ==============================================================================
 * Bus: Bit-banged I2C on PF1 (SCL) / PF0 (SDA), shared with AHT10 and AP3216C.
 * Configured for appliance vibration sensing:
 *   +/-8 g accel full scale (4096 LSB/g) so extract-phase shocks do not clip,
 *   +/-1000 dps gyro, mid-band DLPF to suppress out-of-band noise at 100 Hz polling.
 */

#ifndef ICM20608_H
#define ICM20608_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ICM20608_I2C_ADDR_1         0x68
#define ICM20608_I2C_ADDR_2         0x69
#define ICM20608_WHO_AM_I_VAL       0xAF

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

/* +/-8 g mode: 4096 LSB/g; +/-1000 dps: 32.8 LSB/(deg/s) */
#define ICM20608_ACCEL_SENS_8G      4096.0f
#define ICM20608_GYRO_SENS_1000DPS  32.8f
#define DEG_TO_RAD                  0.017453292519943295f

typedef struct {
    float ax; /* Acceleration X (g) */
    float ay; /* Acceleration Y (g) */
    float az; /* Acceleration Z (g) */
    float gx; /* Gyroscope X (rad/s) */
    float gy; /* Gyroscope Y (rad/s) */
    float gz; /* Gyroscope Z (rad/s) */
} imu_sample_t;

/**
 * @brief Probe both I2C addresses, reset, and configure the sensor for vibration work.
 * @return true when WHO_AM_I matched and configuration was applied.
 */
bool icm20608_init(void);

/**
 * @brief Burst-read all 6 axes and convert to g / rad/s.
 */
bool icm20608_read_sample(imu_sample_t *out_sample);

/**
 * @brief I2C address the probe settled on (0x68 or 0x69); 0 when absent.
 */
uint8_t icm20608_get_address(void);

#ifdef __cplusplus
}
#endif

#endif /* ICM20608_H */
