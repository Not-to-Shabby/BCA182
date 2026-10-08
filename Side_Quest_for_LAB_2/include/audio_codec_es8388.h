/**
 * @file audio_codec_es8388.h
 * @brief High-performance ES8388 I2S3 Stereo Codec Driver with DMA1 Stream 5
 *        for the Side Quest MIDI Karaoke Player on RT-Spark (STM32F407).
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#ifndef AUDIO_CODEC_ES8388_H_
#define AUDIO_CODEC_ES8388_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_SAMPLE_RATE       44100
#define AUDIO_CHANNELS          2
#define AUDIO_DMA_WORDS         1024
#define AUDIO_HALF_WORDS        (AUDIO_DMA_WORDS / 2) /* 512 words = 256 stereo samples (5.8 ms buffer) */

/**
 * @brief PCM stream callback signature.
 *        Called by the high-priority audio producer thread to request stereo
 *        16-bit interleaved PCM frames: [Left0, Right0, Left1, Right1, ...].
 *
 * @param buffer Pointer to destination interleaved 16-bit PCM buffer.
 * @param num_samples Number of 16-bit words (e.g. 256 words = 128 stereo frames).
 */
typedef void (*audio_pcm_callback_t)(int16_t *buffer, size_t num_samples);

/**
 * @brief Live audio hardware diagnostic metrics.
 */
typedef struct {
    bool es_detected;
    uint8_t es_addr;
    uint8_t reg04_readback;
    bool reg04_verified;
    bool pll_locked;
    uint32_t i2s_tx_samples;
    uint32_t dma_misses;
    int16_t peak_left;
    int16_t peak_right;
    uint32_t render_cycles_last;    /* CPU cycles the PCM callback took for one block, interrupts included */
    uint32_t render_cycles_max;
    uint32_t render_cycles_avg;     /* running average */
} audio_diagnostics_t;

/**
 * @brief Initialize ES8388 Stereo Codec (I2C2), STM32 I2S3 (PLLI2S 44.1kHz),
 *        DMA1 Stream 5 circular double-buffering, and spawn the audio producer thread.
 */
void audio_hardware_init(void);

/**
 * @brief Set the active PCM generator callback (e.g. OPL3 synthesizer engine).
 *
 * @param cb Callback function. If NULL, silence is transmitted.
 */
void audio_set_pcm_callback(audio_pcm_callback_t cb);

/**
 * @brief Adjust master headphone output volume.
 *
 * @param volume_percent Volume level from 0 to 100.
 */
void audio_set_volume(uint8_t volume_percent);

/**
 * @brief Retrieve current volume percentage.
 */
uint8_t audio_get_volume(void);

/**
 * @brief Retrieve live audio hardware diagnostics and VU meter peak values.
 */
const audio_diagnostics_t* audio_get_diagnostics(void);

/**
 * @brief Reset instantaneous peak meters for VU displays.
 */
void audio_reset_peak_meters(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_CODEC_ES8388_H_ */
