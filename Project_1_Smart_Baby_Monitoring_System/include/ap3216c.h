/**
 * @file ap3216c.h
 * @brief AP3216C ambient light / proximity sensor driver (I2C 0x1E) - context sensing.
 *
 * Used for night mode (dim LCD + gentler LED alert pulse in a dark laundry room)
 * and reported in diagnostics. Probe at init; graceful ABSENT when not fitted.
 */

#ifndef AP3216C_H
#define AP3216C_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AP3216C_I2C_ADDR 0x1E

typedef struct {
    uint16_t lux;
    uint16_t proximity;
} ap3216c_reading_t;

/** Probe and configure ALS+PS mode. @return true when the sensor ACKs. */
bool ap3216c_init(void);

/** Read ambient light (lux) and proximity counts. */
bool ap3216c_read(ap3216c_reading_t *out);

/** true when the probe succeeded. */
bool ap3216c_is_present(void);

#ifdef __cplusplus
}
#endif

#endif /* AP3216C_H */
