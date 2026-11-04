/**
 * @file audio_codec_es8388.h
 * @brief Driver for Everest Semiconductor ES8388 Stereo Audio Codec
 *        and STM32F407 12-Bit Analog DAC1 on PA4.
 *
 * Adapted from Laboratory Activity 2 (Personal MP3 Player) for Project 1:
 * Smart Baby Monitoring System. Project 1 changes:
 *   - I2S3 system sample rate retimed from 44.1 kHz to ~16 kHz (16 039 Hz)
 *     so the ES8388 ADC (onboard electret mic) and DAC share one 16 kHz
 *     I2S frame plan for the audio capture path.
 *   - ES8388 ADC digital unmute (reg 0x0F) added after the ADC setup block.
 *   - Microphone PGA gain (reg 0x09) made runtime-tunable via
 *     es8388_adc_set_pga() with an electret-safe default (~+18 dB).
 *   - External PCM playback source added: audio_pcm_write()/start()/stop()
 *     feed stereo L,R words into the I2S3 TX DMA staging path (the WAV
 *     player dependency of Lab 2 was removed).
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
 * @brief Selectable synthesis waveforms for the analog DAC and ES8388 codec.
 */
typedef enum {
    AUDIO_WAVE_SINE = 0,
    AUDIO_WAVE_TRIANGLE,
    AUDIO_WAVE_SAWTOOTH,
    AUDIO_WAVE_SQUARE,
    AUDIO_WAVE_COUNT
} audio_waveform_t;

/**
 * @brief Set the active audio synthesizer waveform.
 */
void audio_hardware_dac_set_waveform(audio_waveform_t wave);

/**
 * @brief Get the current audio synthesizer waveform.
 */
audio_waveform_t audio_hardware_dac_get_waveform(void);

/**
 * @brief Cycle to the next waveform (SINE -> TRIANGLE -> SAWTOOTH -> SQUARE).
 * @return New active waveform
 */
audio_waveform_t audio_hardware_dac_cycle_waveform(void);

/**
 * @brief Get human-readable name of the current waveform.
 */
const char* audio_hardware_dac_get_waveform_name(void);

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
 * @brief Count of DMA halves replayed because the producer was not ready in time.
 */
uint32_t audio_stage_misses(void);

/**
 * @brief Get a short diagnostic string describing codec/I2S init status.
 *
 * @return Pointer to a static NUL-terminated buffer (e.g. "ES8388:0x10 PLL:OK")
 */
const char *audio_hardware_dac_status(void);

/**
 * @brief Set the ES8388 ADC microphone PGA gain (ADCCONTROL1, register 0x09).
 *
 * Both channels take the same 3-bit gain field: 0x66 selects ~+18 dB on the
 * LIN1/RIN1 differential electret input (Lab 2 used 0xBB). Safe to call again
 * at runtime to retune capture gain.
 *
 * @param reg09_value Raw value written to ES8388 register 0x09.
 */
void es8388_adc_set_pga(uint8_t reg09_value);

/**
 * @brief Queue stereo PCM words (interleaved L,R, signed 16-bit) into the
 *        I2S3 TX staging FIFO. The DMA producer thread drains the FIFO into
 *        the I2S3 TX DMA buffer at the I2S sample rate (~16 kHz word pairs).
 *
 * This is the real PCM feed API of the TX engine: call audio_pcm_start()
 * first, then push chunks here, then wait for audio_pcm_buffered() == 0 and
 * call audio_pcm_stop().
 *
 * @param stereo_words Interleaved L,R PCM words (stereo_words[0] = Left).
 * @param word_count   Number of words to queue.
 * @return Number of words actually accepted; when less than word_count the
 *         FIFO is full and the caller should retry the remainder after a
 *         short delay.
 */
uint32_t audio_pcm_write(const int16_t *stereo_words, uint32_t word_count);

/**
 * @brief Start pulling from the PCM FIFO into the I2S3 TX DMA path (silence
 *        while the FIFO is empty) and reset the FIFO pointers.
 */
void audio_pcm_start(void);

/**
 * @brief Stop pulling from the PCM FIFO; any still-buffered PCM is discarded.
 */
void audio_pcm_stop(void);

/**
 * @brief Number of stereo words currently waiting in the PCM FIFO.
 */
uint32_t audio_pcm_buffered(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_CODEC_ES8388_H_ */
