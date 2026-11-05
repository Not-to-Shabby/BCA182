/**
 * ==============================================================================
 * Wi-Fi + MQTT-Broker Credentials Flash Persistence Interface (record v2)
 * ==============================================================================
 * Target: STM32F407ZGT6 on-chip Flash Sector 11 (0x080E0000)
 *
 * Record v2 (P4 captive-portal phase) carries the Wi-Fi credentials PLUS the
 * MQTT broker preference captured by the setup portal. The old v1 record
 * ("WIFI" magic) is cleanly invalidated: v2 uses a different magic, so a
 * stale v1 sector simply reads as "no saved credentials" and the setup
 * portal runs on the next boot.
 * ==============================================================================
 */

#ifndef WIFI_STORAGE_H
#define WIFI_STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SSID_MAX_LEN          32
#define WIFI_PASS_MAX_LEN          64
#define WIFI_BROKER_HOST_MAX_LEN   64

/**
 * @brief Reads saved Wi-Fi credentials from on-chip Flash Sector 11.
 *
 * @param out_ssid Output buffer for SSID (at least WIFI_SSID_MAX_LEN bytes)
 * @param out_pass Output buffer for Password (at least WIFI_PASS_MAX_LEN bytes)
 * @return true if a valid v2 credentials record was found and checksum-verified.
 */
bool wifi_storage_read(char *out_ssid, char *out_pass);

/**
 * @brief Reads the saved MQTT broker preference from the v2 record.
 *
 * @param host     Output buffer for the broker host (name or dotted quad).
 * @param host_len Capacity of @p host (value is always NUL-terminated).
 * @param port     Receives the broker port when true is returned.
 * @return false when there is no valid record, or the record carries no
 *         broker (empty host / port 0) -- callers must fall back to the
 *         secrets.h default in that case.
 */
bool wifi_storage_get_broker(char *host, size_t host_len, uint16_t *port);

/**
 * @brief Saves Wi-Fi credentials (no broker) -- thin wrapper over
 *        wifi_storage_save_ex() with an empty broker preference.
 * @note  Writes take 1-2 s under irq_lock() (erase + program of sector 11
 *        stall every thread); only the portal/save path may call it, never
 *        during MQTT/telemetry traffic.
 */
bool wifi_storage_save(const char *ssid, const char *pass);

/**
 * @brief Saves the full v2 record: Wi-Fi credentials + MQTT broker preference.
 *
 * @param ssid        SSID to save (null-terminated, non-empty)
 * @param pass        Password to save (NULL/empty allowed for open APs)
 * @param broker_host MQTT broker host to save (NULL/empty = no broker saved)
 * @param broker_port MQTT broker port to save (0 = none saved)
 * @return true if the record was erased, programmed and verified.
 */
bool wifi_storage_save_ex(const char *ssid, const char *pass,
                          const char *broker_host, uint16_t broker_port);

/**
 * @brief Clears stored credentials in Flash to force Setup Portal mode.
 */
void wifi_storage_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_STORAGE_H */
