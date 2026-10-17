#include "airplay_bridge.h"
#include <Arduino.h>
#include <WiFi.h>
#include "esp_mac.h"
#include "esp_log.h"
#include "airplay/raop.h"
#include "airplay/mdns_service.h"

static bool s_airplay_running = false;

static void on_raop_state_event(raop_event_t ev)
{
    if (ev == RAOP_EV_STREAMING) {
        Serial.println("[AirPlay] Streaming session active");
    } else {
        Serial.println("[AirPlay] Streaming session ended");
    }
}

void airplay_begin(void)
{
    if (s_airplay_running) {
        return;
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char instance_name[64];
    snprintf(instance_name, sizeof(instance_name), "%02X%02X%02X%02X%02X%02X@RT-Spark Hi-Fi",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    Serial.printf("[AirPlay] Advertising RAOP '%s' on port %d\n", instance_name, RAOP_RTSP_PORT);

    // Advertise via mDNS
    mdns_advertise_raop("rtspark", instance_name, RAOP_RTSP_PORT);

    // Register session event callback
    raop_set_event_cb(on_raop_state_event);

    // Start RTSP control server (port 5000)
    raop_server_start();
    s_airplay_running = true;
    Serial.println("[AirPlay] RAOP server active on port 5000! Discovered as 'RT-Spark Hi-Fi'.");
}

void airplay_stop(void)
{
    if (!s_airplay_running) {
        return;
    }
    raop_server_stop();
    s_airplay_running = false;
}
