/**
 * @file ariacast_server.cpp
 * @brief Native AriaCast audio receiver & discovery server for ESP32-S3.
 *
 * Implements:
 *   - UDP Discovery on port 12888 ("DISCOVER_AUDIOCAST" query -> JSON response)
 *   - RFC6455 WebSocket streaming on port 12889:
 *       /audio    : Binary WebSocket frames (3840 bytes raw 16-bit 48kHz stereo PCM)
 *       /metadata : Track title, artist, album sync (WebSocket or HTTP POST)
 *       /control  : Remote volume control and playback actions (Play/Pause/Skip)
 */

#include "ariacast_server.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <mbedtls/sha1.h>
#include <mbedtls/base64.h>
#include "upnp_renderer.h"

#define MAX_WS_CLIENTS        4
#define WS_AUDIO_FRAME_BYTES  3840U

enum ClientRole {
    ROLE_NONE,
    ROLE_AUDIO,
    ROLE_CONTROL,
    ROLE_METADATA,
    ROLE_STATS
};

struct AriaClient {
    int fd;
    ClientRole role;
    bool is_websocket;
    uint32_t last_activity;
};

static TaskHandle_t s_aria_task = nullptr;
static volatile bool s_aria_running = false;
static int s_udp_fd = -1;
static int s_listen_fd = -1;
static AriaClient s_clients[MAX_WS_CLIENTS];
static int s_control_fd = -1;

/* ---------------------------------------------------------------- WebSocket Helpers */

static void compute_ws_accept(const char *key, char *out, size_t max_out)
{
    char combined[128];
    snprintf(combined, sizeof(combined), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);

    uint8_t hash[20];
    mbedtls_sha1((const unsigned char *)combined, strlen(combined), hash);

    size_t olen = 0;
    mbedtls_base64_encode((unsigned char *)out, max_out, &olen, hash, 20);
    out[olen] = '\0';
}

static void ws_send_text(int fd, const char *text)
{
    size_t len = strlen(text);
    uint8_t hdr[10];
    size_t hlen = 0;
    hdr[0] = 0x81; // FIN + text opcode

    if (len < 126) {
        hdr[1] = (uint8_t)len;
        hlen = 2;
    } else {
        hdr[1] = 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)(len & 0xFF);
        hlen = 4;
    }

    send(fd, hdr, hlen, 0);
    send(fd, text, len, 0);
}

static int recv_exact(int fd, uint8_t *buf, size_t len, uint32_t timeout_ms = 3000)
{
    size_t got = 0;
    uint32_t t0 = millis();

    while (got < len && s_aria_running) {
        int r = recv(fd, buf + got, len - got, 0);
        if (r > 0) {
            got += r;
            t0 = millis();
        } else if (r == 0) {
            return -1; // peer closed
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (millis() - t0 > timeout_ms) {
                    return -1;
                }
                vTaskDelay(pdMS_TO_TICKS(1));
            } else {
                return -1;
            }
        }
    }
    return (int)got;
}

/* ---------------------------------------------------------------- Client Management */

static void close_client(int idx)
{
    if (s_clients[idx].fd >= 0) {
        if (s_clients[idx].role == ROLE_AUDIO) {
            ariacast_stop_stream();
            Serial.println("[AriaCast] Audio streaming client disconnected");
        } else if (s_clients[idx].role == ROLE_CONTROL) {
            if (s_control_fd == s_clients[idx].fd) {
                s_control_fd = -1;
            }
            Serial.println("[AriaCast] Control client disconnected");
        }
        close(s_clients[idx].fd);
        s_clients[idx].fd = -1;
        s_clients[idx].role = ROLE_NONE;
        s_clients[idx].is_websocket = false;
    }
}

/* ---------------------------------------------------------------- Protocol Parsing */

static void handle_new_http_request(int idx)
{
    int fd = s_clients[idx].fd;
    char req_buf[1024];
    int n = recv(fd, req_buf, sizeof(req_buf) - 1, 0);
    if (n <= 0) {
        close_client(idx);
        return;
    }
    req_buf[n] = '\0';
    s_clients[idx].last_activity = millis();

    String req(req_buf);
    int first_line_end = req.indexOf("\r\n");
    if (first_line_end < 0) {
        close_client(idx);
        return;
    }

    String first_line = req.substring(0, first_line_end);

    // 1. Check for HTTP POST /metadata
    if (first_line.startsWith("POST /metadata")) {
        int body_pos = req.indexOf("\r\n\r\n");
        String body = "";
        if (body_pos >= 0) {
            body = req.substring(body_pos + 4);
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, body);
        if (!err) {
            const char *title = doc["data"]["title"] | "";
            const char *artist = doc["data"]["artist"] | "";
            const char *album = doc["data"]["album"] | "";
            if (strlen(title) > 0 || strlen(artist) > 0) {
                ariacast_set_metadata(title, artist, album);
                Serial.printf("[AriaCast] Metadata via POST: '%s' by '%s'\n", title, artist);
            }
        }

        const char *resp = "HTTP/1.1 200 OK\r\n"
                           "Content-Type: application/json\r\n"
                           "Connection: close\r\n"
                           "Content-Length: 29\r\n\r\n"
                           "{\"type\":\"ack\",\"success\":true}";
        send(fd, resp, strlen(resp), 0);
        close_client(idx);
        return;
    }

    // 2. Check for WebSocket Upgrade
    if (req.indexOf("Upgrade: websocket") >= 0 || req.indexOf("Upgrade: WebSocket") >= 0) {
        // Extract Sec-WebSocket-Key
        int key_pos = req.indexOf("Sec-WebSocket-Key:");
        if (key_pos < 0) {
            key_pos = req.indexOf("sec-websocket-key:");
        }
        if (key_pos < 0) {
            close_client(idx);
            return;
        }

        int key_val_start = key_pos + 18;
        while (req_buf[key_val_start] == ' ') key_val_start++;
        int key_val_end = req.indexOf("\r\n", key_val_start);
        String ws_key = req.substring(key_val_start, key_val_end);
        ws_key.trim();

        char accept_str[36];
        compute_ws_accept(ws_key.c_str(), accept_str, sizeof(accept_str));

        char resp[256];
        snprintf(resp, sizeof(resp),
                 "HTTP/1.1 101 Switching Protocols\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: %s\r\n\r\n",
                 accept_str);
        send(fd, resp, strlen(resp), 0);

        s_clients[idx].is_websocket = true;

        if (first_line.indexOf("/audio") >= 0) {
            s_clients[idx].role = ROLE_AUDIO;
            Serial.println("[AriaCast] WebSocket /audio connected! Starting audio stream...");
            ariacast_start_stream();

            // Send initial handshake JSON
            const char *ready_msg = "{\"status\":\"READY\",\"sample_rate\":48000,\"channels\":2,\"frame_size\":3840}";
            ws_send_text(fd, ready_msg);

        } else if (first_line.indexOf("/control") >= 0) {
            s_clients[idx].role = ROLE_CONTROL;
            s_control_fd = fd;
            Serial.println("[AriaCast] WebSocket /control connected!");

            const char *ctrl_msg = "{\"status\":\"READY\",\"volume_available\":true,\"current_volume\":80}";
            ws_send_text(fd, ctrl_msg);

        } else if (first_line.indexOf("/metadata") >= 0) {
            s_clients[idx].role = ROLE_METADATA;
            Serial.println("[AriaCast] WebSocket /metadata connected!");

            const char *meta_msg = "{\"type\":\"metadata\",\"data\":{\"title\":\"Ready\",\"artist\":\"AriaCast\",\"album\":\"\",\"duration_ms\":0,\"position_ms\":0,\"is_playing\":false}}";
            ws_send_text(fd, meta_msg);

        } else if (first_line.indexOf("/stats") >= 0) {
            s_clients[idx].role = ROLE_STATS;
            const char *stats_msg = "{\"bufferedFrames\":0,\"receivedFrames\":0}";
            ws_send_text(fd, stats_msg);

        } else {
            s_clients[idx].role = ROLE_NONE;
        }

        return;
    }

    // Default: unrecognized request
    const char *bad = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
    send(fd, bad, strlen(bad), 0);
    close_client(idx);
}

static void handle_websocket_frame(int idx)
{
    int fd = s_clients[idx].fd;
    uint8_t hdr[2];
    if (recv_exact(fd, hdr, 2, 2000) != 2) {
        close_client(idx);
        return;
    }
    s_clients[idx].last_activity = millis();

    uint8_t opcode = hdr[0] & 0x0F;
    bool masked = (hdr[1] & 0x80) != 0;
    uint32_t payload_len = hdr[1] & 0x7F;

    if (payload_len == 126) {
        uint8_t ext[2];
        if (recv_exact(fd, ext, 2, 2000) != 2) {
            close_client(idx);
            return;
        }
        payload_len = ((uint32_t)ext[0] << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8];
        if (recv_exact(fd, ext, 8, 2000) != 8) {
            close_client(idx);
            return;
        }
        payload_len = ((uint32_t)ext[4] << 24) | ((uint32_t)ext[5] << 16) | ((uint32_t)ext[6] << 8) | ext[7];
    }

    uint8_t mask[4] = {0};
    if (masked) {
        if (recv_exact(fd, mask, 4, 2000) != 4) {
            close_client(idx);
            return;
        }
    }

    // Ping frame: reply with Pong
    if (opcode == 0x09) {
        static uint8_t pong_buf[128];
        if (payload_len > 0 && payload_len < sizeof(pong_buf)) {
            recv_exact(fd, pong_buf, payload_len, 1000);
        }
        uint8_t pong_hdr[2] = {0x8A, 0x00};
        send(fd, pong_hdr, 2, 0);
        return;
    }

    // Close frame: close connection
    if (opcode == 0x08) {
        close_client(idx);
        return;
    }

    // 1. Binary Audio Frame (Opcode 0x02)
    if (opcode == 0x02 && s_clients[idx].role == ROLE_AUDIO) {
        static uint8_t audio_buf[4096];
        if (payload_len > sizeof(audio_buf)) {
            // oversized frame, skip
            return;
        }
        if (recv_exact(fd, audio_buf, payload_len, 3000) != (int)payload_len) {
            close_client(idx);
            return;
        }

        if (masked) {
            for (size_t i = 0; i < payload_len; i++) {
                audio_buf[i] ^= mask[i & 3];
            }
        }

        // Feed 16-bit 48kHz stereo PCM samples into ring buffer
        // (payload_len / 2 samples total)
        ariacast_feed_pcm((const int16_t *)audio_buf, payload_len / 2U, 2, 48000);
        return;
    }

    // 2. Text Frame (Opcode 0x01)
    if (opcode == 0x01) {
        static char text_buf[1024];
        if (payload_len >= sizeof(text_buf)) {
            return;
        }
        if (recv_exact(fd, (uint8_t *)text_buf, payload_len, 2000) != (int)payload_len) {
            close_client(idx);
            return;
        }
        text_buf[payload_len] = '\0';

        if (masked) {
            for (size_t i = 0; i < payload_len; i++) {
                text_buf[i] ^= mask[i & 3];
            }
        }

        // Process JSON
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, text_buf);
        if (!err) {
            if (s_clients[idx].role == ROLE_CONTROL) {
                const char *cmd = doc["command"] | "";
                if (strcmp(cmd, "volume_set") == 0) {
                    uint8_t lvl = doc["level"] | 80;
                    ariacast_set_volume(lvl);
                } else if (strcmp(cmd, "volume") == 0) {
                    const char *dir = doc["direction"] | "";
                    if (strcmp(dir, "up") == 0) {
                        // volume up
                    } else if (strcmp(dir, "down") == 0) {
                        // volume down
                    }
                }
                ws_send_text(fd, "{\"type\":\"ack\",\"success\":true}");
            } else if (s_clients[idx].role == ROLE_METADATA) {
                const char *type = doc["type"] | "";
                if (strcmp(type, "update") == 0) {
                    const char *title = doc["data"]["title"] | "";
                    const char *artist = doc["data"]["artist"] | "";
                    const char *album = doc["data"]["album"] | "";
                    ariacast_set_metadata(title, artist, album);
                    Serial.printf("[AriaCast] Metadata via WS: '%s' by '%s'\n", title, artist);
                    ws_send_text(fd, "{\"type\":\"ack\",\"success\":true}");
                }
            }
        }
        return;
    }

    // Other frame: discard payload
    static uint8_t discard[256];
    size_t rem = payload_len;
    while (rem > 0) {
        size_t chunk = rem > sizeof(discard) ? sizeof(discard) : rem;
        if (recv_exact(fd, discard, chunk, 1000) != (int)chunk) {
            close_client(idx);
            return;
        }
        rem -= chunk;
    }
}

/* ---------------------------------------------------------------- Server Task Loop */

static void ariacast_task_func(void *param)
{
    (void)param;

    // 1. Setup UDP Discovery socket on port 12888
    s_udp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_udp_fd >= 0) {
        int opt = 1;
        setsockopt(s_udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in udp_addr;
        memset(&udp_addr, 0, sizeof(udp_addr));
        udp_addr.sin_family = AF_INET;
        udp_addr.sin_port = htons(ARIACAST_DISCOVERY_PORT);
        udp_addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(s_udp_fd, (struct sockaddr *)&udp_addr, sizeof(udp_addr)) < 0) {
            Serial.println("[AriaCast] Failed to bind UDP discovery port 12888");
            close(s_udp_fd);
            s_udp_fd = -1;
        } else {
            Serial.printf("[AriaCast] UDP discovery listener active on port %d\n", ARIACAST_DISCOVERY_PORT);
        }
    }

    // 2. Setup TCP Listening socket on port 12889
    s_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_fd >= 0) {
        int opt = 1;
        setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in tcp_addr;
        memset(&tcp_addr, 0, sizeof(tcp_addr));
        tcp_addr.sin_family = AF_INET;
        tcp_addr.sin_port = htons(ARIACAST_STREAMING_PORT);
        tcp_addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(s_listen_fd, (struct sockaddr *)&tcp_addr, sizeof(tcp_addr)) < 0) {
            Serial.println("[AriaCast] Failed to bind TCP streaming port 12889");
            close(s_listen_fd);
            s_listen_fd = -1;
        } else {
            listen(s_listen_fd, 4);
            Serial.printf("[AriaCast] Native Streaming WebSocket server active on port %d\n", ARIACAST_STREAMING_PORT);
        }
    }

    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        s_clients[i].fd = -1;
        s_clients[i].role = ROLE_NONE;
        s_clients[i].is_websocket = false;
        s_clients[i].last_activity = 0;
    }

    s_aria_running = true;

    while (s_aria_running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        int max_fd = -1;

        if (s_udp_fd >= 0) {
            FD_SET(s_udp_fd, &read_fds);
            if (s_udp_fd > max_fd) max_fd = s_udp_fd;
        }

        if (s_listen_fd >= 0) {
            FD_SET(s_listen_fd, &read_fds);
            if (s_listen_fd > max_fd) max_fd = s_listen_fd;
        }

        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            if (s_clients[i].fd >= 0) {
                FD_SET(s_clients[i].fd, &read_fds);
                if (s_clients[i].fd > max_fd) max_fd = s_clients[i].fd;
            }
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000; // 50 ms

        int sel = select(max_fd + 1, &read_fds, nullptr, nullptr, &tv);
        if (sel > 0) {
            // A. Check UDP discovery query
            if (s_udp_fd >= 0 && FD_ISSET(s_udp_fd, &read_fds)) {
                char udp_buf[128];
                struct sockaddr_in from;
                socklen_t from_len = sizeof(from);
                int len = recvfrom(s_udp_fd, udp_buf, sizeof(udp_buf) - 1, 0,
                                   (struct sockaddr *)&from, &from_len);
                if (len > 0) {
                    udp_buf[len] = '\0';
                    if (strstr(udp_buf, "DISCOVER_AUDIOCAST") != nullptr) {
                        IPAddress local_ip = (WiFi.getMode() == WIFI_STA && WiFi.status() == WL_CONNECTED)
                                             ? WiFi.localIP() : WiFi.softAPIP();
                        char resp[160];
                        snprintf(resp, sizeof(resp),
                                 "{\"server_name\":\"RT-Spark Hi-Fi\",\"ip\":\"%s\",\"port\":%d,\"samplerate\":48000,\"channels\":2}",
                                 local_ip.toString().c_str(), ARIACAST_STREAMING_PORT);
                        sendto(s_udp_fd, resp, strlen(resp), 0, (struct sockaddr *)&from, from_len);
                    }
                }
            }

            // B. Check new incoming TCP connection
            if (s_listen_fd >= 0 && FD_ISSET(s_listen_fd, &read_fds)) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(s_listen_fd, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd >= 0) {
                    int slot = -1;
                    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
                        if (s_clients[i].fd < 0) {
                            slot = i;
                            break;
                        }
                    }
                    if (slot >= 0) {
                        s_clients[slot].fd = client_fd;
                        s_clients[slot].role = ROLE_NONE;
                        s_clients[slot].is_websocket = false;
                        s_clients[slot].last_activity = millis();
                    } else {
                        // No free slots
                        const char *busy = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n";
                        send(client_fd, busy, strlen(busy), 0);
                        close(client_fd);
                    }
                }
            }

            // C. Check existing clients
            for (int i = 0; i < MAX_WS_CLIENTS; i++) {
                if (s_clients[i].fd >= 0 && FD_ISSET(s_clients[i].fd, &read_fds)) {
                    if (!s_clients[i].is_websocket) {
                        handle_new_http_request(i);
                    } else {
                        handle_websocket_frame(i);
                    }
                }
            }
        }

        // Idle timeout check for dead connections (30 seconds)
        uint32_t now = millis();
        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            if (s_clients[i].fd >= 0 && (now - s_clients[i].last_activity > 30000)) {
                close_client(i);
            }
        }
    }

    if (s_udp_fd >= 0) { close(s_udp_fd); s_udp_fd = -1; }
    if (s_listen_fd >= 0) { close(s_listen_fd); s_listen_fd = -1; }
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        close_client(i);
    }
    vTaskDelete(nullptr);
}

void ariacast_server_begin(void)
{
    if (s_aria_task != nullptr) {
        return;
    }
    xTaskCreatePinnedToCore(ariacast_task_func, "ariacast", 6144, nullptr, 5, &s_aria_task, 0);
}

void ariacast_server_stop(void)
{
    s_aria_running = false;
    s_aria_task = nullptr;
}

void ariacast_server_button_action(const char *action)
{
    if (s_control_fd >= 0 && action != nullptr) {
        char msg[64];
        snprintf(msg, sizeof(msg), "{\"action\":\"%s\"}", action);
        ws_send_text(s_control_fd, msg);
    }
}
