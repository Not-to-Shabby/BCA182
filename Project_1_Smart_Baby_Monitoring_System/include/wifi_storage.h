/**
 * ==============================================================================
 * Wi-Fi Credentials Flash Persistence Interface
 * ==============================================================================
 * Target: STM32F407ZGT6 on-chip Flash Sector 11 (0x080E0000)
 * ==============================================================================
 */

#ifndef WIFI_STORAGE_H
#define WIFI_STORAGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SSID_MAX_LEN     32
#define WIFI_PASS_MAX_LEN     64

/**
 * @brief Reads saved Wi-Fi credentials from on-chip Flash Sector 11.
 * 
 * @param out_ssid Output buffer for SSID (at least 32 bytes)
 * @param out_pass Output buffer for Password (at least 64 bytes)
 * @return true if valid credentials record was found and verified with checksum.
 */
bool wifi_storage_read(char *out_ssid, char *out_pass);

/**
 * @brief Saves new Wi-Fi credentials into on-chip Flash Sector 11.
 * 
 * @param ssid SSID to save (null-terminated)
 * @param pass Password to save (null-terminated)
 * @return true if successfully erased and written to Flash.
 */
bool wifi_storage_save(const char *ssid, const char *pass);

/**
 * @brief Clears stored credentials in Flash to force Setup Portal mode.
 */
void wifi_storage_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_STORAGE_H */
