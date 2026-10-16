/**
 * @file audio_receiver.h
 * @brief High-throughput network audio socket receiver & ring buffer for ESP32-S3.
 */

#ifndef AUDIO_RECEIVER_H_
#define AUDIO_RECEIVER_H_

#include <stdint.h>
#include <stdbool.h>
#include "wifi_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*audio_chunk_cb_t)(const uint8_t *pcm_bytes, uint16_t len);
typedef void (*meta_update_cb_t)(const wifi_track_meta_t *meta);

/**
 * @brief Initialize audio network streaming servers (UDP 44100 & TCP 8080).
 */
int audio_receiver_init(audio_chunk_cb_t pcm_cb, meta_update_cb_t meta_cb);

/**
 * @brief Push control command received from RT-Spark to active stream (Pause/Resume/Next/Prev).
 */
void audio_receiver_handle_command(uint8_t cmd);

/**
 * @brief Get current playback track metadata.
 */
void audio_receiver_get_meta(wifi_track_meta_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_RECEIVER_H_ */
