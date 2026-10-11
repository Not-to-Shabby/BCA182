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
#include "synth_sections.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FM_MAX_VOICES       70
#define FM_MIDI_CHANNELS    16
#define FM_DRUM_CHANNEL     9  /* 0-indexed Channel 10 */

typedef enum {
    SYNTH_BANK_YAMAHA_OPL4 = 0,
    SYNTH_BANK_ROLAND_SC55 = 1
} synth_sound_bank_t;

/**
 * @brief Select active WaveTable sound bank (Yamaha OPL4 YRW801 vs. Roland Sound Canvas SC-55).
 */
void yamaha_fm_set_sound_bank(synth_sound_bank_t bank);
synth_sound_bank_t yamaha_fm_get_sound_bank(void);

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
 * @brief Set the level of the lead melody channel in percent (20% to 250%, default 100%).
 *        It scales only the channel chosen with yamaha_fm_set_melody_channel().
 */
void yamaha_fm_set_melody_gain(uint8_t percent);

/**
 * @brief Get the lead melody level in percent.
 */
uint8_t yamaha_fm_get_melody_gain(void);

/**
 * @brief Choose which MIDI channel carries the sung melody, or -1 for none.
 *        Out-of-range values and the drum channel are treated as none.
 */
void yamaha_fm_set_melody_channel(int8_t channel);

/**
 * @brief Channel the synth currently treats as the tune, or -1 if none is clear yet. It starts as
 *        the channel given to yamaha_fm_set_melody_channel() (the one following the lyrics) and
 *        changes while the song plays: it is found from the notes alone, so an instrument that
 *        takes the tune from the singer in a break becomes the lead, and the singer gets it back.
 */
int8_t yamaha_fm_get_lead_channel(void);

/**
 * @brief Song clock, from the sequencer (see midi_synth_callbacks_t::song_clock).
 */
void yamaha_fm_song_time_anchor(uint32_t song_us);

/**
 * @brief Song time of the event the sequencer is about to deliver (see
 *        midi_synth_callbacks_t::event_time). The next note, controller, program or bend call
 *        is queued for the audio frame that matches this time.
 */
void yamaha_fm_song_time_event(uint32_t song_us);

/**
 * @brief Level of the reverb and chorus in percent (0 to 200, default 100). 0 switches both off
 *        and saves their processing time.
 */
void yamaha_fm_set_effects_level(uint8_t percent);
uint8_t yamaha_fm_get_effects_level(void);

/* The mixer sections (synth_sections.h) and the percent limit that every mixer level shares. */

#define SYNTH_MIX_MAX_PERCENT   200

/** Level of one mixer section in percent (0 to 200, default 100). Out-of-range sections are ignored. */
void yamaha_fm_set_section_gain(uint8_t section, uint8_t percent);
uint8_t yamaha_fm_get_section_gain(uint8_t section);

/** Section a General MIDI program or a drum key belongs to. */
uint8_t yamaha_fm_section_of_program(uint8_t program);
uint8_t yamaha_fm_section_of_drum(uint8_t note);

/** Level, mute and solo of one MIDI channel (0-15). While any channel is soloed, the others are silent. */
void yamaha_fm_set_channel_gain(uint8_t channel, uint8_t percent);
uint8_t yamaha_fm_get_channel_gain(uint8_t channel);
void yamaha_fm_set_channel_mute(uint8_t channel, bool mute);
bool yamaha_fm_get_channel_mute(uint8_t channel);
void yamaha_fm_set_channel_solo(uint8_t channel, bool solo);
bool yamaha_fm_get_channel_solo(uint8_t channel);

/** Sets every channel back to 100 %, unmuted, not soloed (done for each new song). Sections are kept. */
void yamaha_fm_reset_channel_mix(void);

/** Bit n set = MIDI channel n currently plays drums (channel 10, and any part a GS message made one). */
uint16_t yamaha_fm_get_drum_parts(void);

/** Reverb and chorus on their own, in percent of the effects level (0 to 200, default 100). */
void yamaha_fm_set_reverb_level(uint8_t percent);
void yamaha_fm_set_chorus_level(uint8_t percent);
uint8_t yamaha_fm_get_reverb_level(void);
uint8_t yamaha_fm_get_chorus_level(void);

/** Bass and treble tone in dB (-6 to +6, default 0) on top of the built-in mastering curve. */
void yamaha_fm_set_tone(int8_t bass_db, int8_t treble_db);
int8_t yamaha_fm_get_tone_bass(void);
int8_t yamaha_fm_get_tone_treble(void);

/**
 * @brief Third operator for piano, electric piano and the string, choir, pad and synth patches
 *        (default on). Notes that start while many voices are sounding stay two-operator, so
 *        the extra work never lands on the busiest moments.
 */
void yamaha_fm_set_third_operator(bool on);
bool yamaha_fm_get_third_operator(void);

/**
 * @brief Enable or disable the Yamaha OPL4 (YMF278B) 16-bit PCM WaveTable drum engine
 *        on MIDI Channel 10 (default on).
 */
void yamaha_fm_set_opl4_drums(bool on);
bool yamaha_fm_get_opl4_drums(void);

/**
 * @brief Enable or disable hybrid sampled attack transients (Yamaha SY77 / OPL4 RCM style).
 */
void yamaha_fm_set_hybrid_attack(bool on);
bool yamaha_fm_get_hybrid_attack(void);

/**
 * @brief Enable or disable per-voice dynamic resonant low-pass filter (VCF).
 */
void yamaha_fm_set_vcf(bool on);
bool yamaha_fm_get_vcf(void);

/**
 * @brief Notes that got a third operator, and notes that could have but started while the
 *        player was too busy.
 */
void yamaha_fm_get_third_operator_stats(uint32_t *started, uint32_t *skipped_busy);

/**
 * @brief The compressor in front of the limiter (default on).
 */
void yamaha_fm_set_compressor(bool on);
bool yamaha_fm_get_compressor(void);

/**
 * @brief Timing and level counters of the event path and the master stage.
 */
typedef struct {
    uint32_t late_events;           /**< events that reached the audio thread after their frame */
    int32_t min_margin_frames;      /**< smallest lead an event had when it was queued (frames) */
    uint32_t ring_peak;             /**< most events ever waiting at once */
    uint32_t dropped_events;        /**< events lost because the ring was full */
    uint32_t limiter_samples;       /**< samples for which the limiter was pulling gain down */
    int max_reduction_db10;         /**< deepest compressor gain reduction since the last call, 0.1 dB */
} yamaha_fm_event_stats_t;

void yamaha_fm_get_event_stats(yamaha_fm_event_stats_t *out);
void yamaha_fm_reset_event_stats(void);

/**
 * @brief Silence all notes immediately.
 */
void yamaha_fm_all_notes_off(void);

/** Marks a MIDI channel as a drum part (Roland GS rhythm part). Channel 10 is always one at reset. */
void yamaha_fm_set_rhythm_part(uint8_t channel, bool is_drum);

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

/**
 * @brief Retrieve active GM program (0-127) and envelope level (0-255) for all 16 MIDI channels,
 *        plus the loudest active drum note on Channel 10 (for RGB LED Matrix visualization).
 */
void yamaha_fm_get_channel_activity(uint8_t out_programs[16], uint8_t out_levels[16], uint8_t *out_drum_note);

#ifdef __cplusplus
}
#endif

#endif /* YAMAHA_FM_SYNTH_H_ */
