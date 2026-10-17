/**
 * @file main.cpp
 * @brief ESP32-S3 Super Mini Wi-Fi Audio Streaming & DLNA Co-Processor with Captive Portal.
 *
 * Board: ESP32-S3 Super Mini (ESP32-S3FH4R2, 4MB Flash, 2MB PSRAM)
 * Features:
 *   - Auto Hotspot with Captive Portal (DNS redirect & Web config interface)
 *   - Persistent NVS storage for Home Wi-Fi credentials (survives power off)
 *   - UPnP / DLNA MediaRenderer SSDP discovery
 *   - UDP Low-Latency 44.1 kHz 16-bit stereo PCM audio receiver on port 44100
 *   - High-Speed UART1 output to RT-Spark (GPIO 43 TX / GPIO 44 RX @ 2.0 Mbps)
 *
 * Connections:
 *   - Pin labeled "TX" (GPIO 43) -> RT-Spark Pin 10 (PA3: USART2_RX)
 *   - Pin labeled "RX" (GPIO 44) -> RT-Spark Pin 8  (PA2: USART2_TX)
 *   - Pin labeled "GND"          -> RT-Spark GND
 *   - Pin labeled "5V"           -> RT-Spark Pin 2 (+5V_SYS)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include "wifi_protocol.h"
#include "upnp_renderer.h"
#include "airplay_bridge.h"
#include "ariacast_server.h"

#define PIN_UART_TX             43  /* Header pin labeled "TX" */
#define PIN_UART_RX             44  /* Header pin labeled "RX" */
#define PIN_RGB_LED             48  /* Onboard WS2812 RGB LED */
#define UART_BAUD_RATE          2000000

#define UDP_AUDIO_PORT          44100
#define SSDP_PORT               1900
#define DNS_PORT                53

static void set_led_color(uint8_t r, uint8_t g, uint8_t b)
{
    neopixelWrite(PIN_RGB_LED, r, g, b);
}

static WiFiUDP s_udp_audio;
static DNSServer s_dns_server;
static WebServer s_web_server(80);
static Preferences s_prefs;

static wifi_track_meta_t s_meta = {};
static wifi_telemetry_t s_telem = {};

static bool s_spark_detected = false;
static uint32_t s_last_pong_ms = 0;
static bool s_portal_active = false;
static bool s_sta_connect_requested = false;
static String s_target_ssid = "";
static String s_target_pass = "";

static SemaphoreHandle_t s_uart_mutex = nullptr;

/* Send structured packet to RT-Spark over High-Speed Serial1 (callable from any task) */
void send_spark_packet(uint8_t type, const uint8_t *payload, uint16_t len)
{
    uint8_t hdr[5];
    hdr[0] = WIFI_PROTO_SYNC1;
    hdr[1] = WIFI_PROTO_SYNC2;
    hdr[2] = type;
    hdr[3] = (uint8_t)(len & 0xFFU);
    hdr[4] = (uint8_t)((len >> 8) & 0xFFU);
    uint8_t cks = wifi_protocol_checksum(type, len, payload);

    if (s_uart_mutex != nullptr) {
        xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
    }
    Serial1.write(hdr, 5);
    if (payload != nullptr && len > 0) {
        Serial1.write(payload, len);
    }
    Serial1.write(cks);
    if (s_uart_mutex != nullptr) {
        xSemaphoreGive(s_uart_mutex);
    }
}

void spark_set_stream_state(uint8_t state)
{
    s_telem.stream_state = state;
}

void spark_set_volume(uint8_t percent)
{
    s_telem.volume_percent = percent;
}

/* Forward declarations */
static void start_access_point(void);
static void connect_station(const char *ssid, const char *pass);
static void handle_portal_root(void);
static void handle_portal_save(void);
static void handle_portal_scan(void);

static void setup_captive_portal(void)
{
    /* Start DNS redirect server: maps every domain to 192.168.4.1 */
    s_dns_server.setErrorReplyCode(DNSReplyCode::NoError);
    s_dns_server.start(DNS_PORT, "*", WiFi.softAPIP());

    /* Web Server routing */
    s_web_server.on("/", HTTP_GET, handle_portal_root);
    s_web_server.on("/save", HTTP_POST, handle_portal_save);
    s_web_server.on("/scan", HTTP_GET, handle_portal_scan);

    /* Captive portal detection hooks for Android, iOS, Windows */
    s_web_server.on("/generate_204", HTTP_GET, handle_portal_root);
    s_web_server.on("/gen_204", HTTP_GET, handle_portal_root);
    s_web_server.on("/hotspot-detect.html", HTTP_GET, handle_portal_root);
    s_web_server.on("/canonical.html", HTTP_GET, handle_portal_root);
    s_web_server.on("/ncsi.txt", HTTP_GET, handle_portal_root);
    s_web_server.onNotFound([]() {
        s_web_server.sendHeader("Location", "http://192.168.4.1/", true);
        s_web_server.send(302, "text/plain", "Redirecting to Captive Portal...");
    });

    s_web_server.begin();
    s_portal_active = true;
    Serial.println("[Portal] Captive Portal & DNS redirect server running on port 80/53!");
}

static void start_access_point(void)
{
    Serial.println("[WiFi] Starting Access Point 'RT-Spark-Audio'...");
    set_led_color(40, 20, 0); /* Orange/Yellow: Captive Portal AP mode */
    WiFi.mode(WIFI_AP);
    WiFi.softAP(CONFIG_WIFI_DEFAULT_AP_SSID, CONFIG_WIFI_DEFAULT_AP_PASS);
    IPAddress ip = WiFi.softAPIP();

    s_telem.wifi_mode = WIFI_STATE_AP;
    snprintf(s_telem.ip_addr, sizeof(s_telem.ip_addr), "%s", ip.toString().c_str());
    s_telem.rssi_dbm = 0;

    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    Serial.printf("[WiFi] AP Started! IP Address: %s (TX Power: 19.5 dBm)\n", s_telem.ip_addr);
    setup_captive_portal();
}

static void connect_station(const char *ssid, const char *pass)
{
    Serial.printf("[WiFi] Connecting to Station '%s'...\n", ssid);
    set_led_color(30, 30, 0); /* Yellow: Connecting... */
    s_portal_active = false;
    s_dns_server.stop();
    s_web_server.stop();

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
        set_led_color(0, 40, 0); /* Solid Green: Connected to Home Wi-Fi! */

        /* Maximize RF output to 19.5 dBm for the small ceramic antenna while keeping modem sleep */
        WiFi.setTxPower(WIFI_POWER_19_5dBm);
        WiFi.setSleep(WIFI_PS_MIN_MODEM);

        /* Save successful credentials to NVS */
        s_prefs.putString("ssid", ssid);
        s_prefs.putString("pass", pass);
    } else {
        Serial.println("[WiFi] Connection failed! Falling back to Captive Portal...");
        set_led_color(40, 0, 0); /* Red: Failed, starting AP */
        delay(500);
        start_access_point();
    }
}

/* Captive Portal HTML UI */
static void handle_portal_root(void)
{
    String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>RT-Spark Wi-Fi Audio Setup</title><style>";
    html += "body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;margin:0;padding:20px;text-align:center;}";
    html += ".card{background:#161b22;border:1px solid #30363d;border-radius:12px;padding:24px;max-width:380px;margin:auto;box-shadow:0 8px 24px rgba(0,0,0,0.5);}";
    html += "h1{color:#ffd33d;font-size:20px;margin-bottom:6px;}";
    html += "p{color:#8b949e;font-size:14px;margin-top:0;}";
    html += ".status{background:#1f242c;padding:8px;border-radius:6px;font-size:13px;color:#58a6ff;margin-bottom:18px;}";
    html += "input,select{width:100%;box-sizing:border-box;padding:12px;margin:8px 0;background:#0d1117;border:1px solid #30363d;border-radius:6px;color:#fff;font-size:15px;}";
    html += "button{width:100%;padding:13px;margin-top:14px;background:#238636;border:none;border-radius:6px;color:#fff;font-weight:bold;font-size:16px;cursor:pointer;}";
    html += "button:hover{background:#2ea043;}";
    html += ".net-list{text-align:left;max-height:160px;overflow-y:auto;border:1px solid #30363d;border-radius:6px;padding:4px;margin-bottom:12px;background:#0d1117;}";
    html += ".net-item{padding:8px 10px;cursor:pointer;border-bottom:1px solid #21262d;font-size:14px;display:flex;justify-content:space-between;}";
    html += ".net-item:hover{background:#1f242c;color:#ffd33d;}";
    html += "</style></head><body><div class='card'>";
    html += "<h1>RT-Spark Wi-Fi Audio</h1>";
    html += "<p>ESP32-S3 Super Mini Co-Processor</p>";
    html += "<div class='status'>IP: " + String(s_telem.ip_addr) + "</div>";
    
    html += "<div class='net-list'>";
    int n = WiFi.scanNetworks();
    if (n == 0) {
        html += "<div style='padding:10px;color:#8b949e;'>No Wi-Fi networks found</div>";
    } else {
        for (int i = 0; i < n; ++i) {
            String s = WiFi.SSID(i);
            int r = WiFi.RSSI(i);
            html += "<div class='net-item' onclick='selectSSID(\"" + s + "\")'>";
            html += "<span>" + s + "</span><span style='color:#8b949e;'>" + String(r) + " dBm</span></div>";
        }
    }
    html += "</div>";

    html += "<form action='/save' method='POST'>";
    html += "<input type='text' id='ssid' name='ssid' placeholder='Wi-Fi Network Name' required>";
    html += "<input type='password' id='pass' name='password' placeholder='Wi-Fi Password'>";
    html += "<button type='submit'>Connect & Save</button>";
    html += "</form>";

    html += "<script>";
    html += "function selectSSID(s){document.getElementById('ssid').value=s;document.getElementById('pass').focus();}";
    html += "</script>";

    html += "</div></body></html>";
    s_web_server.send(200, "text/html", html);
}

static void handle_portal_scan(void)
{
    WiFi.scanNetworks(true);
    s_web_server.sendHeader("Location", "/", true);
    s_web_server.send(302, "text/plain", "Scanning...");
}

static void handle_portal_save(void)
{
    String ssid = s_web_server.arg("ssid");
    String pass = s_web_server.arg("password");

    String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<style>body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;padding:40px;text-align:center;}</style></head>";
    html += "<body><h2>Connecting to " + ssid + "...</h2>";
    html += "<p>Credentials saved. The ESP32 is now connecting to your home Wi-Fi network.</p>";
    html += "<p>You can now reconnect your phone to <b>" + ssid + "</b> and stream music!</p></body></html>";

    s_web_server.send(200, "text/html", html);
    delay(500);

    s_target_ssid = ssid;
    s_target_pass = pass;
    s_sta_connect_requested = true;
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
                    /* Forward 16-bit 44.1kHz stereo PCM to RT-Spark in frames of at most 512 bytes */
                    len &= ~3;
                    for (int off = 0; off < len; off += AUDIO_CHUNK_BYTES) {
                        int n = len - off;
                        if (n > (int)AUDIO_CHUNK_BYTES) {
                            n = (int)AUDIO_CHUNK_BYTES;
                        }
                        send_spark_packet(WIFI_MSG_AUDIO_PCM, packet_buffer + off, (uint16_t)n);
                    }
                    s_telem.stream_state = STREAM_STATE_PLAYING;
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
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
                    upnp_button(cmd);
                    if (cmd == BTN_CMD_PLAY_PAUSE) {
                        ariacast_server_button_action("play");
                    } else if (cmd == BTN_CMD_NEXT) {
                        ariacast_server_button_action("next");
                    } else if (cmd == BTN_CMD_PREV) {
                        ariacast_server_button_action("previous");
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

    s_uart_mutex = xSemaphoreCreateMutex();

    /* Native USB CDC Serial for PC Monitor & Debugging */
    Serial.begin(115200);

    /* Hardware High-Speed UART1 to RT-Spark (GPIO 43 TX / GPIO 44 RX @ 2.0 Mbps) */
    Serial1.setTxBufferSize(2048);
    Serial1.begin(UART_BAUD_RATE, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);

    delay(500);
    Serial.println("\n=======================================================");
    Serial.println(" ESP32-S3 Super Mini Wi-Fi Audio Streaming Co-Processor");
    Serial.println(" Output: High-Speed UART1 @ 2.0 Mbps (GPIO43 TX/GPIO44 RX)");
    Serial.println(" Captive Portal: Auto-detect & DNS redirect on 192.168.4.1");
    Serial.println("=======================================================");

    /* Initialize persistent NVS memory for Home Wi-Fi credentials */
    s_prefs.begin("wifi_cfg", false);
    String saved_ssid = s_prefs.getString("ssid", "");
    String saved_pass = s_prefs.getString("pass", "");

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
    }

    /* Check if we have saved home Wi-Fi credentials */
    if (saved_ssid.length() > 0) {
        Serial.printf("[Boot] Found saved home Wi-Fi credentials for '%s'. Connecting...\n", saved_ssid.c_str());
        connect_station(saved_ssid.c_str(), saved_pass.c_str());
    } else {
        Serial.println("[Boot] No saved credentials. Starting Captive Portal Hotspot...");
        start_access_point();
    }

    /* Start UPnP/DLNA MediaRenderer (SSDP + SOAP control on port 8080) */
    upnp_begin();

    /* Start Apple AirPlay / RAOP Receiver (mDNS + RTSP control on port 5000) */
    airplay_begin();

    /* Start AriaCast Native Receiver (UDP discovery on 12888 + WebSocket on 12889) */
    ariacast_server_begin();

    /* Spawn High-Priority Dedicated Network Audio Task on Core 0 */
    xTaskCreatePinnedToCore(audio_network_task, "audio_net", 4096, nullptr, 5, nullptr, 0);
}

void loop()
{
    process_spark_input();

    /* Handle Captive Portal DNS and HTTP requests if AP mode is active */
    if (s_portal_active) {
        s_dns_server.processNextRequest();
        s_web_server.handleClient();
    }

    /* Process pending connection request from web portal */
    if (s_sta_connect_requested) {
        s_sta_connect_requested = false;
        connect_station(s_target_ssid.c_str(), s_target_pass.c_str());
        upnp_begin();
        airplay_begin();
        ariacast_server_begin();
    }

    upnp_loop();

    /* Send telemetry periodically */
    static uint32_t last_telem_ms = 0;
    if (millis() - last_telem_ms >= 1000) {
        last_telem_ms = millis();

        if (s_telem.wifi_mode == WIFI_STATE_STA && WiFi.status() == WL_CONNECTED) {
            s_telem.rssi_dbm = (int8_t)WiFi.RSSI();
        }

        /* Forward telemetry to RT-Spark ST7789 screen */
        send_spark_packet(WIFI_MSG_TELEMETRY, (const uint8_t *)&s_telem, sizeof(s_telem));

        /* Thermal & health monitoring every 5 seconds */
        static uint32_t last_temp_ms = 0;
        if (millis() - last_temp_ms >= 5000) {
            last_temp_ms = millis();
            float chip_temp = temperatureRead();
            size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            Serial.printf("[System] Chip Temp: %.1f °C, Free Heap: %u bytes, PSRAM: %u bytes\n",
                          chip_temp, (unsigned)esp_get_free_heap_size(), (unsigned)psram_free);
        }
    }

    delay(5);
}
