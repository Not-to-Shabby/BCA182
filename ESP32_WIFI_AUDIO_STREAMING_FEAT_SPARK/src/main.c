/**
 * @file main.c
 * @brief ESP32-S3 Wi-Fi Audio Streaming Co-Processor Main Entry Point (Zephyr RTOS).
 */

#include "wifi_protocol.h"
#include "wifi_manager.h"
#include "audio_receiver.h"
#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>

/* Forward declarations */
int protocol_bridge_init(void (*cb)(uint8_t cmd_type, const uint8_t *payload, uint16_t len));
void protocol_bridge_send_frame(uint8_t type, const uint8_t *payload, uint16_t len);
void protocol_bridge_send_pcm(const uint8_t *pcm_bytes, uint16_t len);
void protocol_bridge_send_meta(const wifi_track_meta_t *meta);
void protocol_bridge_send_telemetry(const wifi_telemetry_t *telem);
void protocol_bridge_send_ping(void);
bool protocol_bridge_is_spark_detected(void);
void protocol_bridge_process_rx(void);
int upnp_renderer_init(void);

static void on_pcm_chunk(const uint8_t *pcm, uint16_t len)
{
    /* Forward uncompressed 16-bit 44.1kHz stereo PCM block over 2.0 Mbps UART link to RT-Spark */
    protocol_bridge_send_pcm(pcm, len);
}

static void on_meta_update(const wifi_track_meta_t *meta)
{
    /* Forward track metadata (Title, Artist, Album) to RT-Spark LCD */
    protocol_bridge_send_meta(meta);
}

static void on_telemetry_update(const wifi_telemetry_t *telem)
{
    /* Forward Wi-Fi telemetry to RT-Spark */
    protocol_bridge_send_telemetry(telem);
}

static void on_spark_command(uint8_t cmd_type, const uint8_t *payload, uint16_t len)
{
    if (cmd_type == WIFI_CMD_BUTTON && len >= 1) {
        audio_receiver_handle_command(payload[0]);
    } else if (cmd_type == WIFI_CMD_SET_WIFI && len >= 1) {
        uint8_t sub = payload[0];
        if (sub == 1) {
            wifi_manager_start_ap("RT-Spark-Audio", "12345678");
        } else if (sub == 2 && len >= 34) {
            char ssid[33] = {0};
            char pass[65] = {0};
            memcpy(ssid, payload + 1, 32);
            memcpy(pass, payload + 33, len - 33);
            wifi_manager_connect_sta(ssid, pass);
        } else if (sub == 3) {
            wifi_manager_scan();
        }
    }
}

int main(void)
{
    printk("\n=======================================================\n");
    printk(" ESP32-S3 Wi-Fi Audio Streaming Co-Processor\n");
    printk(" Zephyr RTOS v3.x | Target: RT-Spark STM32F407 Link\n");
    printk("=======================================================\n");

    protocol_bridge_init(on_spark_command);
    wifi_manager_init(on_telemetry_update);
    audio_receiver_init(on_pcm_chunk, on_meta_update);

    /* 1. Boot Handshake: Ping RT-Spark over UART for up to 2 seconds */
    printk("[Boot] Sending Handshake Ping to RT-Spark...\n");
    bool spark_found = false;
    for (int i = 0; i < 20; i++) {
        protocol_bridge_send_ping();
        k_msleep(100);
        protocol_bridge_process_rx();
        if (protocol_bridge_is_spark_detected()) {
            spark_found = true;
            break;
        }
    }

    if (spark_found) {
        printk("[Boot] RT-Spark Detected! Entering Spark-Controlled Mode.\n");
    } else {
        printk("[Boot] Standalone Mode: Starting Auto Hotspot ('RT-Spark-Audio')...\n");
        wifi_manager_start_ap("RT-Spark-Audio", "12345678");
    }

    upnp_renderer_init();

    /* 2. Main Run Loop */
    uint32_t telem_timer = 0;
    while (1) {
        protocol_bridge_process_rx();

        k_msleep(20);
        telem_timer += 20;
        if (telem_timer >= 1000) {
            telem_timer = 0;
            wifi_telemetry_t telem;
            wifi_manager_get_telemetry(&telem);
            protocol_bridge_send_telemetry(&telem);
        }
    }

    return 0;
}
