/**
 * @file player_logic.h
 * @brief Pure decision and state transition logic for the Personal MP3 Player.
 *        Hardware-independent for testability and portability.
 *
 * Course: BCA182 Embedded Systems Programming
 * Laboratory Activity 2: Personal MP3 Player
 */

#ifndef PLAYER_LOGIC_H_
#define PLAYER_LOGIC_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Operational states of the audio player.
 */
typedef enum {
    PLAYER_STATE_STOPPED = 0,    /**< Audio stopped / idle (Red LED ON) */
    PLAYER_STATE_PLAYING,        /**< Song actively playing (Blue LED ON) */
    PLAYER_STATE_PAUSED,         /**< Song paused at current note (Red LED ON) */
    PLAYER_STATE_CONFIRMING      /**< Awaiting 5-second confirmation (Green LED ON) */
} player_state_t;

/**
 * @brief Song metadata record matching song_def.h split naming convention.
 */
typedef struct {
    uint8_t id;
    const char *name1;
    const char *name2;
    float tempo;
    int length;
} song_info_t;

/**
 * @brief Decode the 3-bit binary button state into a song index (0 to 7).
 *
 * Buttons 2-4 represent binary bits:
 * - Button 2: Bit 0 (LSB, weight 1)
 * - Button 3: Bit 1 (weight 2)
 * - Button 4: Bit 2 (MSB, weight 4)
 *
 * @param b2 Button 2 pressed state (true if pressed / active)
 * @param b3 Button 3 pressed state (true if pressed / active)
 * @param b4 Button 4 pressed state (true if pressed / active)
 * @return Decoded song index in the range [0, 7]
 */
uint8_t decode_binary_song_index(bool b2, bool b3, bool b4);

/**
 * @brief Check if the 5-second confirmation window has expired.
 *
 * @param start_time_ms Timestamp when confirmation began (ms)
 * @param current_time_ms Current system timestamp (ms)
 * @param timeout_duration_ms Timeout duration in ms (typically 5000 ms)
 * @return true if elapsed time >= timeout_duration_ms, false otherwise
 */
bool check_confirmation_timeout(uint32_t start_time_ms, uint32_t current_time_ms, uint32_t timeout_duration_ms);

/**
 * @brief Toggle between Play and Pause states on USER_BUTTON event.
 *
 * @param current_state Current player state
 * @return New player state
 */
player_state_t toggle_play_pause(player_state_t current_state);

/**
 * @brief Normalize raw ADC reading from the potentiometer to a 0-100% volume level.
 *
 * @param raw_adc Raw ADC value
 * @param min_raw Minimum expected raw reading (typically 0)
 * @param max_raw Maximum expected raw reading (typically 4095 for 12-bit ADC)
 * @return Normalized volume percentage in range [0, 100]
 */
uint8_t normalize_adc_volume(uint16_t raw_adc, uint16_t min_raw, uint16_t max_raw);

/**
 * @brief Retrieve song metadata for a given index.
 *
 * @param song_index Index in range [0, 7]
 * @return Pointer to song_info_t record
 */
const song_info_t* get_song_info(uint8_t song_index);

/**
 * @brief Get human-readable string representation of player state.
 *
 * @param state Player state enum
 * @return Constant string pointer
 */
const char* get_player_state_str(player_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* PLAYER_LOGIC_H_ */
