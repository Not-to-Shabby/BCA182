/**
 * @file karaoke_mixer.c
 * @brief Applies the mixer to the synthesizer and remembers it, see karaoke_mixer.h.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include "karaoke_mixer.h"
#include "yamaha_fm_synth.h"

#include <string.h>

_Static_assert((unsigned)SYNTH_SECT_COUNT == MIX_SECTIONS, "MIX_SECTIONS must equal SYNTH_SECT_COUNT");

#define MIX_FILE_MAX    384U

/* Shared by loading and saving. Every caller holds the application lock, so they never overlap, and
 * keeping it out of the stack matters: the button thread that saves has only a few KB. */
static char s_file_buf[MIX_FILE_MAX];

static karaoke_mix_t s_mix;             /* what is playing now */
static karaoke_mix_t s_default;         /* what a song without its own mix starts with */
static uint32_t s_song_code;
static bool s_song_known;               /* a song has been started */
static bool s_file_exists;              /* the current song has a file on the card */
static bool s_dirty;                    /* the mix changed since it was loaded or last saved */

void karaoke_mixer_apply(const karaoke_mix_t *mix)
{
    for (uint8_t i = 0; i < MIX_SECTIONS; i++) {
        yamaha_fm_set_section_gain(i, mix->section[i]);
    }
    for (uint8_t ch = 0; ch < MIX_CHANNELS; ch++) {
        yamaha_fm_set_channel_gain(ch, mix->channel[ch]);
        yamaha_fm_set_channel_mute(ch, (mix->mute & (1U << ch)) != 0U);
        yamaha_fm_set_channel_solo(ch, (mix->solo & (1U << ch)) != 0U);
    }
    yamaha_fm_set_reverb_level(mix->reverb);
    yamaha_fm_set_chorus_level(mix->chorus);
    yamaha_fm_set_tone(mix->bass, mix->treble);
}

static bool load_file(const char *path, karaoke_mix_t *out)
{
    size_t len = 0;

    if (!mix_store_read(path, s_file_buf, sizeof(s_file_buf) - 1U, &len) || len == 0U) {
        return false;
    }
    s_file_buf[len] = '\0';
    return karaoke_mix_parse(out, s_file_buf);
}

static void save_file(const char *path, const karaoke_mix_t *mix)
{
    size_t len = karaoke_mix_format(mix, s_file_buf, sizeof(s_file_buf));

    if (len > 0U) {
        (void)mix_store_write(path, s_file_buf, len);
    }
}

void karaoke_mixer_init(void)
{
    karaoke_mix_defaults(&s_default);
    (void)load_file(MIXER_DEFAULT_PATH, &s_default);
    s_mix = s_default;
    s_song_known = false;
    s_file_exists = false;
    s_dirty = false;
    karaoke_mixer_apply(&s_mix);
}

void karaoke_mixer_song_started(uint32_t song_code)
{
    char path[32];

    s_mix = s_default;
    s_song_code = song_code;
    s_song_known = true;
    s_file_exists = false;
    s_dirty = false;
    if (karaoke_mix_song_path(path, sizeof(path), song_code) && load_file(path, &s_mix)) {
        s_file_exists = true;
    }
    karaoke_mixer_apply(&s_mix);
}

const karaoke_mix_t *karaoke_mixer_get(void)
{
    return &s_mix;
}

const karaoke_mix_t *karaoke_mixer_get_default(void)
{
    return &s_default;
}

bool karaoke_mixer_song_has_own_mix(void)
{
    return s_song_known && !karaoke_mix_equal(&s_mix, &s_default);
}

void karaoke_mixer_adjust(uint8_t item, int direction)
{
    karaoke_mix_item_adjust(&s_mix, item, direction);
    s_dirty = true;
    karaoke_mixer_apply(&s_mix);
}

void karaoke_mixer_click(uint8_t item)
{
    if (item == MIX_ITEM_RESET) {
        s_mix = s_default;
    } else if (item == MIX_ITEM_SAVE_ALL) {
        /* Channels differ from song to song, so only the song-independent part becomes the default. */
        s_default = s_mix;
        for (uint8_t ch = 0; ch < MIX_CHANNELS; ch++) {
            s_default.channel[ch] = 100;
        }
        s_default.mute = 0;
        s_default.solo = 0;
        save_file(MIXER_DEFAULT_PATH, &s_default);
    } else {
        karaoke_mix_item_click(&s_mix, item);
    }
    s_dirty = true;
    karaoke_mixer_apply(&s_mix);
}

void karaoke_mixer_leave(void)
{
    char path[32];

    if (!s_dirty || !s_song_known || !karaoke_mix_song_path(path, sizeof(path), s_song_code)) {
        return;
    }
    s_dirty = false;
    if (karaoke_mix_equal(&s_mix, &s_default)) {
        if (s_file_exists) {
            mix_store_delete(path);
            s_file_exists = false;
        }
        return;
    }
    save_file(path, &s_mix);
    s_file_exists = true;
}
