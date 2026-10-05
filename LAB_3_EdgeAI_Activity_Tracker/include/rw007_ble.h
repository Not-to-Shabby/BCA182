/**
 * ==============================================================================
 * RW007 BLE Peripheral GATT Telemetry Service (Method A)
 * ==============================================================================
 */

#ifndef RW007_BLE_H
#define RW007_BLE_H

#include <stdint.h>
#include <stdbool.h>
#include "ble_protocol.h"
#include "activity_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the RW007 module in BLE Peripheral mode and starts advertising.
 * @return true if initialized successfully
 */
bool rw007_ble_telemetry_init(void);

/**
 * @brief Checks if a smartphone client is currently connected.
 */
bool rw007_ble_is_connected(void);

/**
 * @brief Dispatches an activity telemetry notification packet to the connected phone.
 * 
 * @param activity Classified activity (ACTIVITY_WALKING or ACTIVITY_RUNNING)
 * @param confidence Model confidence percentage (0..100)
 * @param cadence_spm Current step cadence in steps per minute
 * @param step_count Total accumulated step count
 * @param accel_mag_g Filtered acceleration magnitude in g
 * @return true if transmitted or queued successfully
 */
bool rw007_ble_send_activity(
    activity_type_t activity,
    uint8_t confidence,
    uint8_t cadence_spm,
    uint32_t step_count,
    float accel_mag_g);

#ifdef __cplusplus
}
#endif

#endif /* RW007_BLE_H */
