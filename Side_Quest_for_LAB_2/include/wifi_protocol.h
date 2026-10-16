/**
 * @file wifi_protocol.h
 * @brief High-speed binary packet protocol between ESP32-S3 and RT-Spark (STM32F407)
 *        over 2.0 Mbps UART link (PA2/PA3).
 */

#ifndef WIFI_PROTOCOL_H_
#define WIFI_PROTOCOL_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_PROTO_SYNC1        0xAAU
#define WIFI_PROTO_SYNC2        0x55U

#define WIFI_MSG_AUDIO_PCM      0x01U  /* 512-byte raw 16-bit 44.1kHz stereo PCM block */
#define WIFI_MSG_TRACK_META     0x02U  /* Track Title, Artist, Album, Format */
#define WIFI_MSG_TELEMETRY      0x03U  /* Wi-Fi Mode, IP, RSSI, Status */
#define WIFI_MSG_PING           0x04U  /* Handshake Ping */
#define WIFI_MSG_PONG           0x05U  /* Handshake Pong (Ack) */

#define WIFI_CMD_BUTTON         0x10U  /* Button command from Spark to ESP32 */
#define WIFI_CMD_SET_WIFI       0x11U  /* Wi-Fi command from Spark to ESP32 */

/* Button Command IDs */
#define BTN_CMD_PLAY_PAUSE      0x01U
#define BTN_CMD_NEXT            0x02U
#define BTN_CMD_PREV            0x03U
#define BTN_CMD_VOL_UP          0x04U
#define BTN_CMD_VOL_DOWN        0x05U

/* Wi-Fi Modes */
#define WIFI_STATE_IDLE         0x00U
#define WIFI_STATE_AP           0x01U  /* Standalone Hotspot */
#define WIFI_STATE_STA          0x02U  /* Connected to Home Wi-Fi */
#define WIFI_STATE_SCANNING     0x03U

/* Playback States */
#define STREAM_STATE_STOPPED    0x00U
#define STREAM_STATE_PLAYING    0x01U
#define STREAM_STATE_PAUSED     0x02U

#define AUDIO_CHUNK_SAMPLES     128U
#define AUDIO_CHUNK_BYTES       (AUDIO_CHUNK_SAMPLES * 4U) /* 512 bytes */

typedef struct __attribute__((packed)) {
    char title[40];
    char artist[28];
    char album[28];
    char format[16];            /* e.g. "FLAC 44.1kHz" or "Spotify PCM" */
    uint32_t duration_ms;
    uint32_t position_ms;
} wifi_track_meta_t;

typedef struct __attribute__((packed)) {
    uint8_t wifi_mode;          /* 1 = AP, 2 = STA */
    char ip_addr[16];           /* e.g. "192.168.4.1" */
    int8_t rssi_dbm;            /* e.g. -45 dBm */
    uint8_t stream_state;       /* 1 = Playing, 2 = Paused, 0 = Stopped */
    uint8_t volume_percent;     /* 0 to 100 */
} wifi_telemetry_t;

static inline uint8_t wifi_protocol_checksum(uint8_t type, uint16_t len, const uint8_t *data)
{
    uint8_t cks = type ^ (uint8_t)(len & 0xFFU) ^ (uint8_t)((len >> 8) & 0xFFU);
    if (data != 0) {
        for (uint16_t i = 0; i < len; i++) {
            cks ^= data[i];
        }
    }
    return cks;
}

#ifdef __cplusplus
}
#endif

#endif /* WIFI_PROTOCOL_H_ */
