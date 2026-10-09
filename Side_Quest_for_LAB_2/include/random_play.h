/**
 * @file random_play.h
 * @brief Random song choice that avoids repeats, and the idle rule for starting one by itself.
 *        Pure logic with no hardware access, so it runs in the host tests.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#ifndef RANDOM_PLAY_H_
#define RANDOM_PLAY_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** How many of the latest songs a random pick steers around. */
#define RANDOM_PLAY_HISTORY 8

/** Without a button press for this long, a finished song is followed by a random one. */
#ifndef RANDOM_PLAY_IDLE_MS
#define RANDOM_PLAY_IDLE_MS 15000U
#endif

typedef struct {
    uint32_t recent[RANDOM_PLAY_HISTORY];
    uint8_t count;      /* valid entries in recent[] */
    uint8_t next;       /* slot the next entry is written to */
} random_play_t;

void random_play_reset(random_play_t *st);

/**
 * @brief Remember that a song was played (picked at random or chosen by hand), so that a
 *        random pick does not come back to it soon. Playing the same song twice in a row
 *        takes one slot.
 */
void random_play_note(random_play_t *st, uint32_t index);

/**
 * @brief Turn a raw random number into a catalog index.
 *
 * Skips the latest songs when the catalog is big enough to leave a choice: at most
 * total - 1 of them, so there is always a song left to pick. @p r is used as-is, so the
 * caller supplies the entropy.
 *
 * @return an index below @p total, or 0 when @p total is 0.
 */
uint32_t random_play_pick(const random_play_t *st, uint32_t total, uint32_t r);

/**
 * @brief True when a finished song should be followed by a random one: the song is over and
 *        no button was touched for RANDOM_PLAY_IDLE_MS. Safe across the 32-bit millisecond
 *        counter wrapping.
 */
bool random_play_idle_due(uint32_t now_ms, uint32_t last_input_ms, bool song_finished);

#ifdef __cplusplus
}
#endif

#endif /* RANDOM_PLAY_H_ */
