/**
 * @file midi_karaoke_parser.h
 * @brief High-efficiency Standard MIDI File (SMF Format 0 and 1) sequencer
 *        and synchronized karaoke lyrics parser for RT-Spark (STM32F407).
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#ifndef MIDI_KARAOKE_PARSER_H_
#define MIDI_KARAOKE_PARSER_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIDI_MAX_TRACKS             32
#define KARAOKE_MAX_LINE_CHARS      40
#define KARAOKE_LYRIC_QUEUE_SIZE    16

/**
 * @brief Synchronized karaoke lyric event for display rendering.
 */
typedef struct {
    char text[48];
    bool is_newline;
    uint32_t song_time_ms;
} karaoke_lyric_msg_t;

/**
 * @brief Callback invoked when MIDI channel events occur.
 */
typedef struct {
    void (*note_on)(uint8_t channel, uint8_t note, uint8_t velocity);
    void (*note_off)(uint8_t channel, uint8_t note, uint8_t velocity);
    void (*program_change)(uint8_t channel, uint8_t program);
    void (*control_change)(uint8_t channel, uint8_t control, uint8_t value);
    void (*pitch_bend)(uint8_t channel, uint16_t bend);
    void (*all_notes_off)(void);
} midi_synth_callbacks_t;

/**
 * @brief State descriptor for MIDI playback engine.
 */
typedef struct {
    bool is_playing;
    bool is_paused;
    uint16_t format;
    uint16_t num_tracks;
    uint16_t ppqn;              /* Pulses / Ticks per Quarter Note */
    uint32_t tempo_us;          /* Microseconds per Quarter Note (BPM = 60,000,000 / tempo_us) */
    uint32_t current_tick;
    uint32_t total_ticks;
    uint32_t elapsed_ms;
    uint32_t duration_ms;
    uint16_t bpm;
    bool has_lyrics_started;
    char current_lyric_line[KARAOKE_MAX_LINE_CHARS + 1];
    char upcoming_lyric_line[KARAOKE_MAX_LINE_CHARS + 1];
    char previous_lyric_line[KARAOKE_MAX_LINE_CHARS + 1];
} midi_player_status_t;

/**
 * @brief Initialize the MIDI sequencer engine.
 *
 * @param synth_cb Pointer to synthesizer event callbacks.
 */
void midi_karaoke_init(const midi_synth_callbacks_t *synth_cb);

/**
 * @brief Load a Standard MIDI file from memory (RAM or ROM array).
 *
 * @param data Pointer to MIDI file binary data.
 * @param length Total length in bytes.
 * @return true if successfully parsed and loaded.
 */
bool midi_karaoke_load_memory(const uint8_t *data, uint32_t length);

/**
 * @brief Start or resume playback.
 */
void midi_karaoke_play(void);

/**
 * @brief Pause playback.
 */
void midi_karaoke_pause(void);

/**
 * @brief Stop playback and silence all active notes.
 */
void midi_karaoke_stop(void);

/**
 * @brief Advance sequencer timing by elapsed microseconds.
 *        Called periodically by timer or audio tick worker.
 *
 * @param elapsed_us Time delta in microseconds.
 */
void midi_karaoke_tick(uint32_t elapsed_us);

/**
 * @brief Retrieve current playback status and lyrics.
 */
void midi_karaoke_get_status(midi_player_status_t *out_status);

/**
 * @brief Pop next pending lyric message, if any.
 *
 * @param out_msg Pointer to output struct.
 * @return true if a new lyric message was popped.
 */
bool midi_karaoke_pop_lyric(karaoke_lyric_msg_t *out_msg);

#ifdef __cplusplus
}
#endif

#endif /* MIDI_KARAOKE_PARSER_H_ */
