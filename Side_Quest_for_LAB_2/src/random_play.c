/**
 * @file random_play.c
 * @brief Random song choice that avoids repeats, and the idle rule for starting one by itself.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include "random_play.h"
#include <string.h>

void random_play_reset(random_play_t *st)
{
    memset(st, 0, sizeof(*st));
}

/* Slot of the k-th latest entry (k = 0 is the latest). */
static uint8_t slot_of(const random_play_t *st, uint8_t k)
{
    return (uint8_t)((st->next + RANDOM_PLAY_HISTORY - 1U - k) % RANDOM_PLAY_HISTORY);
}

void random_play_note(random_play_t *st, uint32_t index)
{
    if (st->count > 0U && st->recent[slot_of(st, 0)] == index) {
        return;
    }
    st->recent[st->next] = index;
    st->next = (uint8_t)((st->next + 1U) % RANDOM_PLAY_HISTORY);
    if (st->count < RANDOM_PLAY_HISTORY) {
        st->count++;
    }
}

static bool is_recent(const random_play_t *st, uint8_t window, uint32_t index)
{
    for (uint8_t k = 0; k < window; k++) {
        if (st->recent[slot_of(st, k)] == index) {
            return true;
        }
    }
    return false;
}

uint32_t random_play_pick_filtered(const random_play_t *st, uint32_t total, uint32_t r, random_play_filter_fn filter)
{
    if (total == 0U) {
        return 0U;
    }

    uint32_t window = st->count;
    if (window > total - 1U) {
        window = total - 1U;
    }

    uint32_t candidate = r % total;
    uint32_t attempts = 0;
    while (attempts < total) {
        if (!is_recent(st, (uint8_t)window, candidate)) {
            if (filter == NULL || filter(candidate)) {
                return candidate;
            }
        }
        candidate = (candidate + 1U) % total;
        attempts++;
    }

    /* Fallback if no song matched filter: pick first non-recent song */
    candidate = r % total;
    while (is_recent(st, (uint8_t)window, candidate)) {
        candidate = (candidate + 1U) % total;
    }
    return candidate;
}

uint32_t random_play_pick(const random_play_t *st, uint32_t total, uint32_t r)
{
    return random_play_pick_filtered(st, total, r, NULL);
}

bool random_play_idle_due(uint32_t now_ms, uint32_t last_input_ms, bool song_finished)
{
    return song_finished && (uint32_t)(now_ms - last_input_ms) >= RANDOM_PLAY_IDLE_MS;
}
