/**
 * @file yamaha_fm_synth.h
 * @brief Embedded 18-Voice Yamaha OPL/DX Style FM Synthesizer with General MIDI
 *        and Rhythm Kit for RT-Spark (STM32F407) driving the ES8388 3.5mm Output.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#ifndef YAMAHA_FM_SYNTH_H_
#define YAMAHA_FM_SYNTH_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "midi_karaoke_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FM_MAX_VOICES       30
#define FM_MIDI_CHANNELS    16
#define FM_DRUM_CHANNEL     9  /* 0-indexed Channel 10 */

/**
 * @brief Initialize the Yamaha FM synthesizer engine and patch tables.
 *
 * @param sample_rate Audio output sample rate (44100 Hz).
 */
void yamaha_fm_synth_init(uint32_t sample_rate);

/**
 * @brief Reset all active voices, envelope generators, and controllers.
 */
void yamaha_fm_synth_reset(void);

/**
 * @brief Note On event.
 *
 * @param channel MIDI channel (0-15).
 * @param note MIDI note number (0-127).
 * @param velocity Velocity (1-127).
 */
void yamaha_fm_note_on(uint8_t channel, uint8_t note, uint8_t velocity);

/**
 * @brief Note Off event.
 *
 * @param channel MIDI channel (0-15).
 * @param note MIDI note number (0-127).
 * @param velocity Release velocity (0-127).
 */
void yamaha_fm_note_off(uint8_t channel, uint8_t note, uint8_t velocity);

/**
 * @brief Program Change event (Instrument patch selection).
 *
 * @param channel MIDI channel (0-15).
 * @param program GM Program number (0-127).
 */
void yamaha_fm_program_change(uint8_t channel, uint8_t program);

/**
 * @brief Control Change event (Volume, Pan, Sustain, Modulation).
 *
 * @param channel MIDI channel (0-15).
 * @param control Controller ID (7 = Volume, 10 = Pan, 64 = Sustain, etc.).
 * @param value Controller value (0-127).
 */
void yamaha_fm_control_change(uint8_t channel, uint8_t control, uint8_t value);

/**
 * @brief Pitch Bend event.
 *
 * @param channel MIDI channel (0-15).
 * @param bend 14-bit bend value (0 to 16383, 8192 = center).
 */
void yamaha_fm_pitch_bend(uint8_t channel, uint16_t bend);

/**
 * @brief Set instrument / synth master gain in percent (50% to 200%, default 100%).
 */
void yamaha_fm_set_instrument_gain(uint8_t percent);

/**
 * @brief Get instrument / synth master gain in percent.
 */
uint8_t yamaha_fm_get_instrument_gain(void);

/**
 * @brief Set drum / rhythm gain in percent (50% to 200%, default 100%).
 */
void yamaha_fm_set_drum_gain(uint8_t percent);

/**
 * @brief Get drum / rhythm gain in percent.
 */
uint8_t yamaha_fm_get_drum_gain(void);

/**
 * @brief Silence all notes immediately.
 */
void yamaha_fm_all_notes_off(void);

/**
 * @brief Render stereo 16-bit interleaved PCM samples directly into the audio buffer.
 *
 * @param buffer Interleaved stereo buffer [L0, R0, L1, R1, ...].
 * @param num_samples Total number of 16-bit sample words (e.g. 256 = 128 stereo pairs).
 */
void yamaha_fm_synth_render(int16_t *buffer, size_t num_samples);

/**
 * @brief Return synthesizer callbacks struct ready to plug into the MIDI sequencer.
 */
const midi_synth_callbacks_t* yamaha_fm_get_callbacks(void);

/**
 * @brief Get the count of currently audible voices (for diagnostic HUD).
 */
uint8_t yamaha_fm_get_active_voice_count(void);

/**
 * @brief Number of times a new note found every voice busy and took one over.
 */
uint32_t yamaha_fm_get_steal_count(void);

/**
 * @brief The subset of takeovers that cut off a held note or a tail louder than about -30 dB.
 */
uint32_t yamaha_fm_get_audible_steal_count(void);

/**
 * @brief Number of note events lost because the event ring was full.
 */
uint32_t yamaha_fm_get_dropped_events(void);

#ifdef __cplusplus
}
#endif

#endif /* YAMAHA_FM_SYNTH_H_ */
