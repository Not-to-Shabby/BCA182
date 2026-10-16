/**
 * @file wifi_manager.h
 * @brief Wi-Fi Connection & Access Point Manager for ESP32-S3 under Zephyr RTOS.
 */

#ifndef WIFI_MANAGER_H_
#define WIFI_MANAGER_H_

#include <stdint.h>
#include <stdbool.h>
#include "wifi_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wifi_state_change_cb_t)(const wifi_telemetry_t *telemetry);

/**
 * @brief Initialize Wi-Fi subsystem.
 */
int wifi_manager_init(wifi_state_change_cb_t cb);

/**
 * @brief Start standalone private Hotspot (Access Point mode).
 * @param ssid Hotspot SSID (e.g. "RT-Spark-Audio")
 * @param psk Password (at least 8 chars)
 */
int wifi_manager_start_ap(const char *ssid, const char *psk);

/**
 * @brief Connect to existing home Wi-Fi network (Station mode).
 */
int wifi_manager_connect_sta(const char *ssid, const char *psk);

/**
 * @brief Trigger an asynchronous Wi-Fi scan of nearby networks.
 */
int wifi_manager_scan(void);

/**
 * @brief Get current telemetry snapshot.
 */
void wifi_manager_get_telemetry(wifi_telemetry_t *out);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H_ */
