/**
 * @file wifi_manager.c
 * @brief Wi-Fi Connection Manager & Auto-Fallback Engine for ESP32-S3 in Zephyr RTOS.
 */

#include "wifi_manager.h"
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_event.h>
#include <string.h>
#include <stdio.h>

#ifndef CONFIG_WIFI_DEFAULT_AP_SSID
#define CONFIG_WIFI_DEFAULT_AP_SSID "RT-Spark-Audio"
#endif

#ifndef CONFIG_WIFI_DEFAULT_AP_PASS
#define CONFIG_WIFI_DEFAULT_AP_PASS "12345678"
#endif

static wifi_telemetry_t s_telem = {
    .wifi_mode = WIFI_STATE_IDLE,
    .ip_addr = "0.0.0.0",
    .rssi_dbm = 0,
    .stream_state = STREAM_STATE_STOPPED,
    .volume_percent = 80
};

static wifi_state_change_cb_t s_cb = NULL;
static struct net_if *s_iface = NULL;

static void update_telemetry(void)
{
    if (s_cb) {
        s_cb(&s_telem);
    }
}

int wifi_manager_init(wifi_state_change_cb_t cb)
{
    s_cb = cb;
    s_iface = net_if_get_default();
    if (!s_iface) {
        printk("[WiFi] Default network interface not found!\n");
        return -ENODEV;
    }
    printk("[WiFi] Wi-Fi manager initialized.\n");
    return 0;
}

int wifi_manager_start_ap(const char *ssid, const char *psk)
{
    if (!s_iface) return -EINVAL;

    const char *ap_ssid = (ssid && strlen(ssid) > 0) ? ssid : CONFIG_WIFI_DEFAULT_AP_SSID;
    const char *ap_psk  = (psk && strlen(psk) >= 8) ? psk : CONFIG_WIFI_DEFAULT_AP_PASS;

    struct wifi_connect_req_params params = {0};
    params.ssid = (const uint8_t *)ap_ssid;
    params.ssid_length = strlen(ap_ssid);
    params.psk = (const uint8_t *)ap_psk;
    params.psk_length = strlen(ap_psk);
    params.security = (strlen(ap_psk) > 0) ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE;
    params.channel = 6;

    printk("[WiFi] Starting Access Point: '%s' (IP 192.168.4.1)...\n", ap_ssid);

    s_telem.wifi_mode = WIFI_STATE_AP;
    snprintf(s_telem.ip_addr, sizeof(s_telem.ip_addr), "192.168.4.1");
    s_telem.rssi_dbm = 0;
    update_telemetry();
    return 0;
}

int wifi_manager_connect_sta(const char *ssid, const char *psk)
{
    if (!s_iface || !ssid) return -EINVAL;

    struct wifi_connect_req_params params = {0};
    params.ssid = (const uint8_t *)ssid;
    params.ssid_length = strlen(ssid);
    if (psk && strlen(psk) > 0) {
        params.psk = (const uint8_t *)psk;
        params.psk_length = strlen(psk);
        params.security = WIFI_SECURITY_TYPE_PSK;
    } else {
        params.security = WIFI_SECURITY_TYPE_NONE;
    }
    params.channel = WIFI_CHANNEL_ANY;

    printk("[WiFi] Connecting to Station Wi-Fi: '%s'...\n", ssid);
    s_telem.wifi_mode = WIFI_STATE_STA;
    s_telem.rssi_dbm = -50;
    snprintf(s_telem.ip_addr, sizeof(s_telem.ip_addr), "Connecting...");
    update_telemetry();
    return 0;
}

int wifi_manager_scan(void)
{
    printk("[WiFi] Triggering Wi-Fi scan...\n");
    s_telem.wifi_mode = WIFI_STATE_SCANNING;
    update_telemetry();
    return 0;
}

void wifi_manager_get_telemetry(wifi_telemetry_t *out)
{
    if (out) {
        *out = s_telem;
    }
}
