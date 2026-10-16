/**
 * @file main.cpp
 * @brief ESP32-S3 Super Mini Wi-Fi Audio Streaming & DLNA Co-Processor.
 *
 * Board: ESP32-S3 Super Mini (ESP32-S3FH4R2, 4MB Flash, 2MB PSRAM)
 * Connections:
 *   - Pin labeled "TX" (GPIO 43) -> RT-Spark Pin 10 (PA3: USART2_RX)
 *   - Pin labeled "RX" (GPIO 44) -> RT-Spark Pin 8  (PA2: USART2_TX)
 *   - Pin labeled "GND"          -> RT-Spark GND
 *   - Pin labeled "5V"           -> RT-Spark Pin 2 (+5V_SYS)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "wifi_protocol.h"

#define PIN_UART_TX             43  /* Header pin labeled "TX" */
#define PIN_UART_RX             44  /* Header pin labeled "RX" */
#define UART_BAUD_RATE          2000000

#define UDP_AUDIO_PORT          44100
#define SSDP_PORT               1900

static WiFiUDP s_udp_audio;
static WiFiUDP s_udp_ssdp;

static wifi_track_meta_t s_meta = {};
static wifi_telemetry_t s_telem = {};

static bool s_spark_detected = false;
static uint32_t s_last_pong_ms = 0;

static const char *SSDP_NOTIFY =
    "NOTIFY * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:1900\r\n"
    "CACHE-CONTROL: max-age=1800\r\n"
    "LOCATION: http://192.168.4.1:8080/description.xml\r\n"
    "NT: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
    "NTS: ssdp:alive\r\n"
    "SERVER: ESP32-S3/1.0 UPnP/1.1 RT-Spark-Audio/1.0\r\n"
    "USN: uuid:12345678-1234-1234-1234-123456789abc::urn:schemas-upnp-org:device:MediaRenderer:1\r\n\r\n";

/* Send structured packet to RT-Spark over High-Speed Serial1 */
static void send_spark_packet(uint8_t type, const uint8_t *payload, uint16_t len)
{
    uint8_t hdr[5];
    hdr[0] = WIFI_PROTO_SYNC1;
    hdr[1] = WIFI_PROTO_SYNC2;
    hdr[2] = type;
    hdr[3] = (uint8_t)(len & 0xFFU);
    hdr[4] = (uint8_t)((len >> 8) & 0xFFU);
    uint8_t cks = wifi_protocol_checksum(type, len, payload);

    Serial1.write(hdr, 5);
    if (payload != nullptr && len > 0) {
        Serial1.write(payload, len);
    }
    Serial1.write(cks);
}

/* Wi-Fi Setup Functions */
static void start_access_point(void)
{
    Serial.println("[WiFi] Starting Access Point 'RT-Spark-Audio'...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(CONFIG_WIFI_DEFAULT_AP_SSID, CONFIG_WIFI_DEFAULT_AP_PASS);
    IPAddress ip = WiFi.softAPIP();

    s_telem.wifi_mode = WIFI_STATE_AP;
    snprintf(s_telem.ip_addr, sizeof(s_telem.ip_addr), "%s", ip.toString().c_str());
    s_telem.rssi_dbm = 0;

    Serial.printf("[WiFi] AP Started! IP Address: %s\n", s_telem.ip_addr);
}

static void connect_station(const char *ssid, const char *pass)
{
    Serial.printf("[WiFi] Connecting to Station '%s'...\n", ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0 < 8000)) {
        delay(200);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        IPAddress ip = WiFi.localIP();
        s_telem.wifi_mode = WIFI_STATE_STA;
        snprintf(s_telem.ip_addr, sizeof(s_telem.ip_addr), "%s", ip.toString().c_str());
        s_telem.rssi_dbm = (int8_t)WiFi.RSSI();
        Serial.printf("[WiFi] Connected! IP Address: %s (RSSI %d dBm)\n", s_telem.ip_addr, s_telem.rssi_dbm);
    } else {
        Serial.println("[WiFi] Connection failed! Falling back to Access Point...");
        start_access_point();
    }
}

/* Core 0 FreeRTOS Thread for UDP Audio Packet Receiving */
static void audio_network_task(void *param)
{
    (void)param;
    s_udp_audio.begin(UDP_AUDIO_PORT);
    Serial.printf("[Audio] UDP streaming listener active on port %d\n", UDP_AUDIO_PORT);

    static uint8_t packet_buffer[1024];

    while (true) {
        int pkt_size = s_udp_audio.parsePacket();
        if (pkt_size > 0) {
            int len = s_udp_audio.read(packet_buffer, sizeof(packet_buffer));
            if (len > 0) {
                if (len >= 4 && memcmp(packet_buffer, "META", 4) == 0 && len >= (4 + (int)sizeof(wifi_track_meta_t))) {
                    memcpy(&s_meta, packet_buffer + 4, sizeof(s_meta));
                    send_spark_packet(WIFI_MSG_TRACK_META, (const uint8_t *)&s_meta, sizeof(s_meta));
                    Serial.printf("[Audio] Track Meta: '%s' by '%s'\n", s_meta.title, s_meta.artist);
                } else {
                    /* Forward uncompressed 16-bit 44.1kHz stereo PCM block to RT-Spark */
                    send_spark_packet(WIFI_MSG_AUDIO_PCM, packet_buffer, (uint16_t)len);
                    s_telem.stream_state = STREAM_STATE_PLAYING;
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

/* Parse incoming commands from RT-Spark */
static void process_spark_input(void)
{
    static uint8_t state = 0;
    static uint8_t msg_type = 0;
    static uint16_t msg_len = 0;
    static uint8_t payload[128];
    static uint16_t rx_idx = 0;

    while (Serial1.available()) {
        uint8_t b = (uint8_t)Serial1.read();
        switch (state) {
        case 0:
            if (b == WIFI_PROTO_SYNC1) state = 1;
            break;
        case 1:
            if (b == WIFI_PROTO_SYNC2) state = 2;
            else state = 0;
            break;
        case 2:
            msg_type = b;
            state = 3;
            break;
        case 3:
            msg_len = b;
            state = 4;
            break;
        case 4:
            msg_len |= ((uint16_t)b << 8);
            if (msg_len > sizeof(payload)) {
                state = 0;
            } else if (msg_len == 0) {
                state = 6;
            } else {
                rx_idx = 0;
                state = 5;
            }
            break;
        case 5:
            payload[rx_idx++] = b;
            if (rx_idx >= msg_len) {
                state = 6;
            }
            break;
        case 6: {
            uint8_t cks = wifi_protocol_checksum(msg_type, msg_len, payload);
            if (cks == b) {
                if (msg_type == WIFI_MSG_PONG) {
                    s_spark_detected = true;
                    s_last_pong_ms = millis();
                } else if (msg_type == WIFI_CMD_BUTTON && msg_len >= 1) {
                    uint8_t cmd = payload[0];
                    Serial.printf("[Control] Button Command from RT-Spark: 0x%02X\n", cmd);
                    if (cmd == BTN_CMD_PLAY_PAUSE) {
                        s_telem.stream_state = (s_telem.stream_state == STREAM_STATE_PLAYING) ? STREAM_STATE_PAUSED : STREAM_STATE_PLAYING;
                    }
                } else if (msg_type == WIFI_CMD_SET_WIFI && msg_len >= 1) {
                    if (payload[0] == 1) {
                        start_access_point();
                    }
                }
            }
            state = 0;
            break;
        }
        default:
            state = 0;
            break;
        }
    }

    if (s_spark_detected && (millis() - s_last_pong_ms > 4000)) {
        s_spark_detected = false;
    }
}

void setup()
{
    /* Initialize default metadata and telemetry */
    strncpy(s_meta.title, "Ready to Stream", sizeof(s_meta.title) - 1);
    strncpy(s_meta.artist, "Connect to Wi-Fi", sizeof(s_meta.artist) - 1);
    strncpy(s_meta.album, "RT-Spark Wireless", sizeof(s_meta.album) - 1);
    strncpy(s_meta.format, "WAV 16-bit 44.1k", sizeof(s_meta.format) - 1);

    s_telem.wifi_mode = WIFI_STATE_IDLE;
    strncpy(s_telem.ip_addr, "0.0.0.0", sizeof(s_telem.ip_addr) - 1);
    s_telem.rssi_dbm = 0;
    s_telem.stream_state = STREAM_STATE_STOPPED;
    s_telem.volume_percent = 80;

    /* Native USB CDC Serial for PC Monitor & Debugging */
    Serial.begin(115200);

    /* Hardware High-Speed UART1 to RT-Spark (GPIO 43 TX / GPIO 44 RX @ 2.0 Mbps) */
    Serial1.begin(UART_BAUD_RATE, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);

    delay(500);
    Serial.println("\n=======================================================");
    Serial.println(" ESP32-S3 Super Mini Wi-Fi Audio Streaming Co-Processor");
    Serial.println(" Output: High-Speed UART1 @ 2.0 Mbps (GPIO43 TX/GPIO44 RX)");
    Serial.println("=======================================================");

    /* Boot Handshake: Ping RT-Spark for up to 2 seconds */
    Serial.println("[Boot] Sending Handshake Ping to RT-Spark...");
    uint32_t start_ms = millis();
    while (millis() - start_ms < 2000) {
        send_spark_packet(WIFI_MSG_PING, nullptr, 0);
        delay(100);
        process_spark_input();
        if (s_spark_detected) {
            break;
        }
    }

    if (s_spark_detected) {
        Serial.println("[Boot] RT-Spark Board Detected! Synchronized.");
    } else {
        Serial.println("[Boot] Standalone / Auto Mode: Launching Hotspot...");
        start_access_point();
    }

    /* Start SSDP UPnP Advertiser */
    s_udp_ssdp.begin(SSDP_PORT);

    /* Spawn High-Priority Dedicated Network Audio Task on Core 0 */
    xTaskCreatePinnedToCore(audio_network_task, "audio_net", 4096, nullptr, 5, nullptr, 0);
}

void loop()
{
    process_spark_input();

    /* Send telemetry and SSDP announcements periodically */
    static uint32_t last_telem_ms = 0;
    if (millis() - last_telem_ms >= 1000) {
        last_telem_ms = millis();

        if (s_telem.wifi_mode == WIFI_STATE_STA && WiFi.status() == WL_CONNECTED) {
            s_telem.rssi_dbm = (int8_t)WiFi.RSSI();
        }

        /* Forward telemetry to RT-Spark ST7789 screen */
        send_spark_packet(WIFI_MSG_TELEMETRY, (const uint8_t *)&s_telem, sizeof(s_telem));

        /* SSDP Broadcast for DLNA auto-discovery */
        s_udp_ssdp.beginPacket("239.255.255.250", 1900);
        s_udp_ssdp.write((const uint8_t *)SSDP_NOTIFY, strlen(SSDP_NOTIFY));
        s_udp_ssdp.endPacket();
    }

    delay(10);
}
