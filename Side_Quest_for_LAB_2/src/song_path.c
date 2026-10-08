/**
 * @file song_path.c
 * @brief SD card location of a song's MIDI file.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include "song_path.h"
#include <stdio.h>

bool song_midi_path(char *out, size_t out_size, uint32_t song_code,
                    const char *filename, bool flat)
{
    int len;

    if (flat) {
        len = snprintf(out, out_size, "SD:/midi/%s", filename);
    } else {
        len = snprintf(out, out_size, "SD:/midi/%03u/%s",
                       (unsigned)(song_code / 1000U), filename);
    }

    return len >= 0 && (size_t)len < out_size;
}
