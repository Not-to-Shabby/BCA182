/**
 * @file audio_engine.h
 * @brief Real-time audio engine and musical note synthesizer for the Personal MP3 Player.
 *        Implements hardware TIM3_CH3 PWM note synthesis on PB0 and Zephyr k_timer
 *        ticker for drift-free note progression.
 *
 * Course: BCA182 Embedded Systems Programming
 * Laboratory Activity 2: Personal MP3 Player
 */

#ifndef AUDIO_ENGINE_H_
#define AUDIO_ENGINE_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Musical piece metadata record.
 */
typedef struct {
    uint8_t id;
    const char *title;
    const char *composer;
    const float *notes;
    const float *beats;
    float tempo;
    uint16_t length;
} musical_piece_t;

/**
 * @brief Initialize hardware timer TIM3 Channel 3 on PB0 for PWM buzzer audio,
 *        and configure the Zephyr k_timer ticker for note progression.
 */
void audio_engine_init(void);

/**
 * @brief Start or restart playing the specified song from note index 0.
 *
 * @param song_index Song index in range [0, 7]
 */
void audio_engine_start_song(uint8_t song_index);

/**
 * @brief Pause audio playback at the current note position.
 */
void audio_engine_pause(void);

/**
 * @brief Resume audio playback from the current note position.
 */
void audio_engine_resume(void);

/**
 * @brief Stop audio playback and reset note position to beginning.
 */
void audio_engine_stop(void);

/**
 * @brief Update audio playback volume duty cycle (0% to 100%).
 *
 * @param volume_percent Volume level in range [0, 100]
 */
void audio_engine_set_volume(uint8_t volume_percent);

/**
 * @brief Get the current playing note index of the active song.
 */
uint16_t audio_engine_get_note_index(void);

/**
 * @brief Check if audio playback is actively playing sound.
 */
bool audio_engine_is_playing(void);

/**
 * @brief Retrieve musical piece metadata for a given track.
 *
 * @param song_index Track index in range [0, 7]
 */
const musical_piece_t* audio_engine_get_piece(uint8_t song_index);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_ENGINE_H_ */
