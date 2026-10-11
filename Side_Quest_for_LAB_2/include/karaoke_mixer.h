/**
 * @file karaoke_mixer.h
 * @brief Applies the mixer to the synthesizer and remembers it: a default mix for every song
 *        (SD:/karaoke_mix.cfg) and an own mix per song (SD:/mix/<code>.cfg).
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#ifndef KARAOKE_MIXER_H_
#define KARAOKE_MIXER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "karaoke_mix.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIXER_DEFAULT_PATH  "SD:/karaoke_mix.cfg"

/* Storage the mixer reads and writes through. karaoke_mix_store.c implements it with FatFs on
 * the board; the host tests supply a fake. */
bool mix_store_read(const char *path, char *buf, size_t size, size_t *len);
bool mix_store_write(const char *path, const char *buf, size_t len);
void mix_store_delete(const char *path);

/** Pushes every value of @p mix to the synthesizer. */
void karaoke_mixer_apply(const karaoke_mix_t *mix);

/** Loads the default mix (if the card has one) and applies it. Call once the SD card is mounted. */
void karaoke_mixer_init(void);

/** A new song starts: its own mix if it has one, otherwise the default. Applied at once. */
void karaoke_mixer_song_started(uint32_t song_code);

const karaoke_mix_t *karaoke_mixer_get(void);
const karaoke_mix_t *karaoke_mixer_get_default(void);

/** True if the current song plays with a mix of its own. */
bool karaoke_mixer_song_has_own_mix(void);

/** Moves a row one step (+1 or -1) and applies it. */
void karaoke_mixer_adjust(uint8_t item, int direction);

/** Click on a row: channel mute/solo cycle, reset of a value row, or one of the two action rows. */
void karaoke_mixer_click(uint8_t item);

/**
 * Saves the song's mix if it differs from the default, or removes its file. Does nothing if the mix
 * has not changed since it was loaded or last saved, so it is cheap to call on every song change.
 */
void karaoke_mixer_leave(void);

#ifdef __cplusplus
}
#endif

#endif /* KARAOKE_MIXER_H_ */
