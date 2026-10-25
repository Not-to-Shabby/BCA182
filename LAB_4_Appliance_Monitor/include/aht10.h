/**
 * @file aht10.h
 * @brief Aosong AHT10 temperature/humidity sensor driver (I2C 0x38) - context sensing.
 *
 * The RT-Spark kit may or may not fit this sensor; the driver probes at init and
 * reports ABSENT so the product and diagnostics degrade gracefully.
 */

#ifndef AHT10_H
#define AHT10_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AHT10_I2C_ADDR 0x38

typedef struct {
    float temperature_c;
    float humidity_pct;
} aht10_reading_t;

/** Probe and soft-reset the sensor. @return true when the sensor ACKs. */
bool aht10_init(void);

/** Trigger a measurement and read T/RH. Takes ~80 ms of sensor conversion time. */
bool aht10_read(aht10_reading_t *out);

/** true when the probe succeeded (sensor physically fitted). */
bool aht10_is_present(void);

#ifdef __cplusplus
}
#endif

#endif /* AHT10_H */
