/**
 * @file song_path.h
 * @brief SD card location of a song's MIDI file, shared by the catalog and the host tests.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#ifndef SONG_PATH_H_
#define SONG_PATH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build the SD path of a song's MIDI file.
 *
 * The sharded layout puts a song in the folder named after its code in
 * thousands (code 36527 -> SD:/midi/036/036527.mid), so no FAT32 folder holds
 * more than 1000 files. The flat layout (SD:/midi/036527.mid) is what cards
 * prepared by the earlier tool used. tools/prepare_karaoke_sd.py must use the
 * same folder rule.
 *
 * @param out Destination buffer.
 * @param out_size Size of @p out in bytes.
 * @param song_code Numeric song code from the index record.
 * @param filename File name from the index record (for example "036527.mid").
 * @param flat true for the flat layout, false for the sharded layout.
 * @return true if the whole path fit in @p out.
 */
bool song_midi_path(char *out, size_t out_size, uint32_t song_code,
                    const char *filename, bool flat);

#ifdef __cplusplus
}
#endif

#endif /* SONG_PATH_H_ */
