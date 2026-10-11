/**
 * @file karaoke_mix.h
 * @brief The mixer's data model: one value per mixer row, how a row is adjusted, and the text
 *        form used to save a mix on the SD card. It has no hardware dependencies, so the host
 *        tests cover it; karaoke_mixer.c applies a mix to the synthesizer and stores it.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#ifndef KARAOKE_MIX_H_
#define KARAOKE_MIX_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIX_SECTIONS        15U     /* equals SYNTH_SECT_COUNT (checked in karaoke_mixer.c) */
#define MIX_CHANNELS        16U
#define MIX_PERCENT_MAX     200U
#define MIX_TONE_MAX_DB     6

/** Rows of the mixer screen, top to bottom. */
#define MIX_ITEM_SECTION0   0U                          /* 15 instrument and drum sections */
#define MIX_ITEM_REVERB     (MIX_SECTIONS)
#define MIX_ITEM_CHORUS     (MIX_SECTIONS + 1U)
#define MIX_ITEM_BASS       (MIX_SECTIONS + 2U)
#define MIX_ITEM_TREBLE     (MIX_SECTIONS + 3U)
#define MIX_ITEM_CHANNEL0   (MIX_SECTIONS + 4U)         /* 16 MIDI channels */
#define MIX_ITEM_RESET      (MIX_ITEM_CHANNEL0 + MIX_CHANNELS)
#define MIX_ITEM_SAVE_ALL   (MIX_ITEM_RESET + 1U)
#define MIX_ITEM_DONE       (MIX_ITEM_SAVE_ALL + 1U)    /* saves and closes the mixer */
#define MIX_ITEM_COUNT      (MIX_ITEM_DONE + 1U)

typedef enum {
    MIX_KIND_SECTION,
    MIX_KIND_EFFECT,        /* reverb, chorus: percent */
    MIX_KIND_TONE,          /* bass, treble: dB */
    MIX_KIND_CHANNEL,       /* gain, with mute and solo */
    MIX_KIND_ACTION
} mix_item_kind_t;

typedef struct {
    uint8_t section[MIX_SECTIONS];  /* percent, 100 = unchanged */
    uint8_t channel[MIX_CHANNELS];  /* percent */
    uint16_t mute;                  /* bit n = channel n muted */
    uint16_t solo;                  /* bit n = channel n soloed */
    uint8_t reverb;                 /* percent */
    uint8_t chorus;                 /* percent */
    int8_t bass;                    /* dB */
    int8_t treble;                  /* dB */
} karaoke_mix_t;

void karaoke_mix_defaults(karaoke_mix_t *mix);
bool karaoke_mix_equal(const karaoke_mix_t *a, const karaoke_mix_t *b);

mix_item_kind_t karaoke_mix_item_kind(uint8_t item);

/** Name of a row that does not depend on the song ("Kick", "Reverb", ..., "Ch 3"). */
const char *karaoke_mix_item_name(uint8_t item, char *scratch, size_t scratch_size);

/** Value of a row: percent for sections, effects and channels, dB for tone. 0 for actions. */
int karaoke_mix_item_value(const karaoke_mix_t *mix, uint8_t item);

/** Largest and smallest value of a row. */
int karaoke_mix_item_max(uint8_t item);
int karaoke_mix_item_min(uint8_t item);

/** Moves a row one step: +1 or -1 (5 % for percent rows, 1 dB for tone rows). Clamps at the ends. */
void karaoke_mix_item_adjust(karaoke_mix_t *mix, uint8_t item, int direction);

/** Puts one row back to its default (a channel row also clears its mute and solo). */
void karaoke_mix_item_reset(karaoke_mix_t *mix, uint8_t item);

/** What a click on a row does: a channel steps normal -> muted -> soloed -> normal, any other row resets. */
void karaoke_mix_item_click(karaoke_mix_t *mix, uint8_t item);

/** 0 normal, 1 muted, 2 soloed. */
int karaoke_mix_channel_state(const karaoke_mix_t *mix, uint8_t channel);

/** True if the row differs from its default. */
bool karaoke_mix_item_changed(const karaoke_mix_t *mix, uint8_t item);

/**
 * @brief Writes the mix as text ("SEC=100,...\n" and so on).
 * @return length written, or 0 if @p out_size is too small.
 */
size_t karaoke_mix_format(const karaoke_mix_t *mix, char *out, size_t out_size);

/**
 * @brief Reads text written by karaoke_mix_format(). Unknown lines are ignored and out-of-range
 *        values are clamped, so an edited or damaged file cannot give a value the mixer refuses.
 *        Anything the text does not mention keeps the value @p mix had.
 * @return true if at least one line was understood.
 */
bool karaoke_mix_parse(karaoke_mix_t *mix, const char *text);

/**
 * The rows the mixer shows for the song that is playing: only the instrument sections and the
 * channels the song actually uses, plus the effect and tone rows and the three action rows.
 * Entries are MIX_ITEM_* numbers, in screen order.
 */
typedef struct {
    uint8_t count;
    uint8_t item[MIX_ITEM_COUNT];
} mix_view_t;

/**
 * @param usage_valid false if the song could not be scanned: then every section and channel is listed.
 * @param sections    bit s = section s (synth_section_t) has a note.
 * @param channels    bit n = channel n plays at least one note.
 */
void karaoke_mix_build_view(mix_view_t *view, bool usage_valid, uint16_t sections, uint16_t channels);

/** General MIDI instrument name (at most 15 characters) and the name of a drum kit by its program. */
const char *karaoke_mix_program_name(uint8_t program);
const char *karaoke_mix_kit_name(uint8_t program);

/** SD path of a song's mix file: SD:/mix/<code>.cfg. */
bool karaoke_mix_song_path(char *out, size_t out_size, uint32_t song_code);

#ifdef __cplusplus
}
#endif

#endif /* KARAOKE_MIX_H_ */
