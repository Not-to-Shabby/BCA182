/**
 * @file audio_codec_es8388.h
 * @brief Driver for Everest Semiconductor ES8388 Stereo Audio Codec
 *        and STM32F407 12-Bit Analog DAC1 on PA4 for the Personal MP3 Player.
 *
 * Course: BCA182 Embedded Systems Programming
 * Laboratory Activity 2: Personal MP3 Player
 */

#ifndef AUDIO_CODEC_ES8388_H_
#define AUDIO_CODEC_ES8388_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize both the on-board ES8388 Stereo Codec (3.5mm Jack CN3)
 *        via I2C2 + I2S3, and the internal 12-bit Analog DAC1 on PA4.
 */
void audio_hardware_dac_init(void);

/**
 * @brief Output an analog musical tone waveform at the specified note period.
 *
 * @param note_period_ms Note period in milliseconds (0.0f = silence)
 * @param volume_percent Volume level (0% to 100%)
 */
void audio_hardware_dac_set_tone(float note_period_ms, uint8_t volume_percent);

/**
 * @brief Silence the analog DAC and codec audio outputs.
 */
void audio_hardware_dac_stop(void);

/**
 * @brief Adjust master volume attenuation on the ES8388 codec and DAC output.
 *
 * @param volume_percent Volume level (0% to 100%)
 */
void audio_hardware_dac_set_volume(uint8_t volume_percent);

/**
 * @brief Live hardware audio diagnostic metrics.
 */
typedef struct {
    bool es_detected;
    uint8_t es_addr;
    uint8_t reg04_readback;
    bool reg04_verified;
    bool pll_locked;
    uint32_t i2s_tx_samples;
    bool dac_active;
} audio_diagnostics_t;

/**
 * @brief Retrieve live audio hardware diagnostics.
 */
const audio_diagnostics_t* audio_get_diagnostics(void);

/**
 * @brief Get a short diagnostic string describing codec/I2S init status.
 *
 * @return Pointer to a static NUL-terminated buffer (e.g. "ES8388:0x10 PLL:OK")
 */
const char *audio_hardware_dac_status(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_CODEC_ES8388_H_ */
