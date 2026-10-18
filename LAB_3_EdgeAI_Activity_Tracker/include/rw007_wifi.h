/**
 * ==============================================================================
 * Realtek RW007 SPI Wi-Fi Driver Interface for Zephyr RTOS
 * ==============================================================================
 * Platform: RT-Thread Spark Board (STM32F407ZGT6)
 * Bus: SPI2 (PB13 SCK, PB14 MISO, PB15 MOSI)
 * Control: PF10 (CS), PG15 (RST), PG11 (INT_BUSY)
 * Framework: Zephyr RTOS
 * ==============================================================================
 */

#ifndef RW007_WIFI_H
#define RW007_WIFI_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RW007_UDP_PORT 8888
#define RW007_AP_IP    0x0104A8C0 /* 192.168.4.1 */
#define RW007_MAX_SCAN_APS 16

typedef struct {
    char ssid[33];
    int16_t rssi;
} rw007_ap_entry_t;

/**
 * @brief Retrieves the list of unique access points discovered during the last scan.
 * @return Number of access points written to out_aps.
 */
int rw007_wifi_get_scan_aps(rw007_ap_entry_t *out_aps, int max_count);

/**
 * @brief Initializes the RW007 hardware (SPI2, GPIOs, reset sequence).
 */
bool rw007_wifi_init(void);

/**
 * @brief Scans for access points and prints each one to the console.
 * @return number of APs found, or -1 on failure.
 */
int rw007_wifi_scan(void);

/**
 * @brief True once the module has reported its SoftAP as started.
 */
bool rw007_wifi_softap_active(void);

/**
 * @brief Connects to a 2.4 GHz Wi-Fi access point in Station mode.
 *        Returns true only if the module reports a successful association.
 */
bool rw007_wifi_connect(const char *ssid, const char *password);

/**
 * @brief Activates SoftAP mode broadcasting an access point (e.g. "Spark-Setup").
 * 
 * @param ap_ssid Hotspot SSID (e.g. "Spark-Setup")
 * @param ap_pass Hotspot password (or NULL/empty for open network)
 */
bool rw007_wifi_start_softap(const char *ap_ssid, const char *ap_pass);

/**
 * @brief Shuts down SoftAP mode and returns to Station mode.
 */
bool rw007_wifi_stop_softap(void);

/**
 * @brief Activates WeChat BLE AirKiss pairing mode.
 */
bool rw007_wifi_ble_cfgwifi(uint32_t duration_ms);

/** Called from the driver thread with each Ethernet frame the module received. */
typedef void (*rw007_rx_cb_t)(const uint8_t *frame, uint16_t len);

void rw007_wifi_set_rx_callback(rw007_rx_cb_t cb);

/**
 * @brief Queues one raw Ethernet frame for transmission. The frame is tagged as
 *        SoftAP or station traffic according to the current module mode.
 * @return false if the single TX slot stayed busy or the frame size is invalid.
 */
bool rw007_wifi_send_frame(const uint8_t *frame, uint16_t len);

/** Station MAC address read from the module. */
void rw007_wifi_get_mac(uint8_t mac[6]);

/** SoftAP MAC address read from the module (valid after rw007_wifi_start_softap). */
void rw007_wifi_get_ap_mac(uint8_t mac[6]);

/**
 * @brief Checks if Wi-Fi station is associated with an AP.
 */
bool rw007_wifi_is_connected(void);

/**
 * @brief Returns the RW007 firmware version string.
 */
const char* rw007_wifi_get_version(void);

/**
 * @brief Returns the RW007 hardware serial number.
 */
const char* rw007_wifi_get_sn(void);

#ifdef __cplusplus
}
#endif

#endif /* RW007_WIFI_H */
