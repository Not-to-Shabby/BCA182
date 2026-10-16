/**
 * @file wifi_audio_receiver.h
 * @brief High-speed UART2 DMA receiver for incoming Wi-Fi audio stream from ESP32-S3.
 */

#ifndef WIFI_AUDIO_RECEIVER_H_
#define WIFI_AUDIO_RECEIVER_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "wifi_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize USART2 hardware & circular DMA receiver (PA2 TX / PA3 RX).
 */
int wifi_audio_receiver_init(void);

/**
 * @brief Check if ESP32-S3 co-processor is detected and actively synchronized.
 */
bool wifi_audio_receiver_is_connected(void);

/**
 * @brief Get latest track metadata (Title, Artist, Format) sent by phone.
 */
void wifi_audio_receiver_get_metadata(wifi_track_meta_t *out);

/**
 * @brief Get latest Wi-Fi telemetry (IP address, RSSI, status).
 */
void wifi_audio_receiver_get_telemetry(wifi_telemetry_t *out);

/**
 * @brief Send playback control command to ESP32-S3 (Play/Pause, Skip, Volume).
 */
void wifi_audio_send_command(uint8_t cmd);

/**
 * @brief Send Wi-Fi configuration command to ESP32-S3 (AP mode, Station mode, Scan).
 */
void wifi_audio_send_wifi_config(uint8_t mode, const char *ssid, const char *pass);

/**
 * @brief Pull decoded PCM audio frames into the output buffer for I2S3 DAC.
 */
void wifi_audio_render(int16_t *out_pcm, size_t frames);

/**
 * @brief Get current Left and Right peak amplitudes for VU meter display.
 */
void wifi_audio_get_peaks(int16_t *peak_l, int16_t *peak_r);

/**
 * @brief Process incoming UART stream and dispatch parsed packets.
 */
void wifi_audio_process(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_AUDIO_RECEIVER_H_ */
