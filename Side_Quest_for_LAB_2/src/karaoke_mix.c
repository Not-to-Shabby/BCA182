/**
 * @file karaoke_mix.c
 * @brief The mixer's data model, see karaoke_mix.h.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include "karaoke_mix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIX_STEP_PERCENT    5

static const char *const SECTION_NAMES[MIX_SECTIONS] = {
    "Piano / Keys", "Organ", "Guitar", "Bass", "Strings / Choir", "Brass", "Reeds / Pipes",
    "Synth Lead", "Pads / FX", "Other", "Kick", "Snare / Clap", "Hi-Hats", "Cymbals", "Toms / Perc"
};

static const char *const GM_NAMES[128] = {
    "Grand Piano",
    "Bright Piano",
    "E.Grand Piano",
    "Honky-tonk",
    "E.Piano 1",
    "E.Piano 2",
    "Harpsichord",
    "Clavinet",
    "Celesta",
    "Glockenspiel",
    "Music Box",
    "Vibraphone",
    "Marimba",
    "Xylophone",
    "Tubular Bells",
    "Dulcimer",
    "Drawbar Organ",
    "Perc. Organ",
    "Rock Organ",
    "Church Organ",
    "Reed Organ",
    "Accordion",
    "Harmonica",
    "Tango Accord.",
    "Nylon Guitar",
    "Steel Guitar",
    "Jazz Guitar",
    "Clean Guitar",
    "Muted Guitar",
    "Overdrive Gtr",
    "Distortion Gtr",
    "Gtr Harmonics",
    "Acoustic Bass",
    "Finger Bass",
    "Pick Bass",
    "Fretless Bass",
    "Slap Bass 1",
    "Slap Bass 2",
    "Synth Bass 1",
    "Synth Bass 2",
    "Violin",
    "Viola",
    "Cello",
    "Contrabass",
    "Trem. Strings",
    "Pizzicato",
    "Harp",
    "Timpani",
    "Strings 1",
    "Strings 2",
    "Synth Strings 1",
    "Synth Strings 2",
    "Choir Aahs",
    "Voice Oohs",
    "Synth Voice",
    "Orchestra Hit",
    "Trumpet",
    "Trombone",
    "Tuba",
    "Muted Trumpet",
    "French Horn",
    "Brass Section",
    "Synth Brass 1",
    "Synth Brass 2",
    "Soprano Sax",
    "Alto Sax",
    "Tenor Sax",
    "Baritone Sax",
    "Oboe",
    "English Horn",
    "Bassoon",
    "Clarinet",
    "Piccolo",
    "Flute",
    "Recorder",
    "Pan Flute",
    "Blown Bottle",
    "Shakuhachi",
    "Whistle",
    "Ocarina",
    "Square Lead",
    "Saw Lead",
    "Calliope Lead",
    "Chiff Lead",
    "Charang Lead",
    "Voice Lead",
    "Fifths Lead",
    "Bass+Lead",
    "New Age Pad",
    "Warm Pad",
    "Polysynth Pad",
    "Choir Pad",
    "Bowed Pad",
    "Metallic Pad",
    "Halo Pad",
    "Sweep Pad",
    "Rain FX",
    "Soundtrack FX",
    "Crystal FX",
    "Atmosphere FX",
    "Brightness FX",
    "Goblins FX",
    "Echoes FX",
    "Sci-Fi FX",
    "Sitar",
    "Banjo",
    "Shamisen",
    "Koto",
    "Kalimba",
    "Bagpipe",
    "Fiddle",
    "Shanai",
    "Tinkle Bell",
    "Agogo",
    "Steel Drums",
    "Woodblock",
    "Taiko",
    "Melodic Tom",
    "Synth Drum",
    "Reverse Cymbal",
    "Fret Noise",
    "Breath Noise",
    "Seashore",
    "Bird Tweet",
    "Telephone",
    "Helicopter",
    "Applause",
    "Gunshot"
};

void karaoke_mix_defaults(karaoke_mix_t *mix)
{
    memset(mix, 0, sizeof(*mix));
    for (size_t i = 0; i < MIX_SECTIONS; i++) {
        mix->section[i] = 100;
    }
    for (size_t i = 0; i < MIX_CHANNELS; i++) {
        mix->channel[i] = 100;
    }
    mix->reverb = 100;
    mix->chorus = 100;
}

bool karaoke_mix_equal(const karaoke_mix_t *a, const karaoke_mix_t *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

mix_item_kind_t karaoke_mix_item_kind(uint8_t item)
{
    if (item < MIX_SECTIONS) return MIX_KIND_SECTION;
    if (item == MIX_ITEM_REVERB || item == MIX_ITEM_CHORUS) return MIX_KIND_EFFECT;
    if (item == MIX_ITEM_BASS || item == MIX_ITEM_TREBLE) return MIX_KIND_TONE;
    if (item >= MIX_ITEM_CHANNEL0 && item < MIX_ITEM_RESET) return MIX_KIND_CHANNEL;
    return MIX_KIND_ACTION;
}

const char *karaoke_mix_item_name(uint8_t item, char *scratch, size_t scratch_size)
{
    if (item < MIX_SECTIONS) return SECTION_NAMES[item];
    switch (item) {
    case MIX_ITEM_REVERB: return "Reverb";
    case MIX_ITEM_CHORUS: return "Chorus";
    case MIX_ITEM_BASS: return "Bass tone";
    case MIX_ITEM_TREBLE: return "Treble tone";
    case MIX_ITEM_RESET: return "Reset this song";
    case MIX_ITEM_SAVE_ALL: return "Set as default";
    case MIX_ITEM_DONE: return "Done";
    default: break;
    }
    if (karaoke_mix_item_kind(item) == MIX_KIND_CHANNEL && scratch != NULL && scratch_size > 0U) {
        snprintf(scratch, scratch_size, "Ch %u", (unsigned)(item - MIX_ITEM_CHANNEL0 + 1U));
        return scratch;
    }
    return "";
}

int karaoke_mix_item_value(const karaoke_mix_t *mix, uint8_t item)
{
    if (item < MIX_SECTIONS) return mix->section[item];
    switch (item) {
    case MIX_ITEM_REVERB: return mix->reverb;
    case MIX_ITEM_CHORUS: return mix->chorus;
    case MIX_ITEM_BASS: return mix->bass;
    case MIX_ITEM_TREBLE: return mix->treble;
    default: break;
    }
    if (karaoke_mix_item_kind(item) == MIX_KIND_CHANNEL) return mix->channel[item - MIX_ITEM_CHANNEL0];
    return 0;
}

int karaoke_mix_item_max(uint8_t item)
{
    return (karaoke_mix_item_kind(item) == MIX_KIND_TONE) ? MIX_TONE_MAX_DB : (int)MIX_PERCENT_MAX;
}

int karaoke_mix_item_min(uint8_t item)
{
    return (karaoke_mix_item_kind(item) == MIX_KIND_TONE) ? -MIX_TONE_MAX_DB : 0;
}

static void set_value(karaoke_mix_t *mix, uint8_t item, int v)
{
    int lo = karaoke_mix_item_min(item);
    int hi = karaoke_mix_item_max(item);

    if (v < lo) v = lo;
    if (v > hi) v = hi;
    if (item < MIX_SECTIONS) {
        mix->section[item] = (uint8_t)v;
    } else if (item == MIX_ITEM_REVERB) {
        mix->reverb = (uint8_t)v;
    } else if (item == MIX_ITEM_CHORUS) {
        mix->chorus = (uint8_t)v;
    } else if (item == MIX_ITEM_BASS) {
        mix->bass = (int8_t)v;
    } else if (item == MIX_ITEM_TREBLE) {
        mix->treble = (int8_t)v;
    } else if (karaoke_mix_item_kind(item) == MIX_KIND_CHANNEL) {
        mix->channel[item - MIX_ITEM_CHANNEL0] = (uint8_t)v;
    }
}

void karaoke_mix_item_adjust(karaoke_mix_t *mix, uint8_t item, int direction)
{
    mix_item_kind_t k = karaoke_mix_item_kind(item);

    if (k == MIX_KIND_ACTION || direction == 0) {
        return;
    }
    int step = (k == MIX_KIND_TONE) ? 1 : MIX_STEP_PERCENT;
    int v = karaoke_mix_item_value(mix, item);

    if (k != MIX_KIND_TONE) {
        /* land on multiples of the step even if the value came from an edited file */
        v = (direction > 0) ? ((v / step) + 1) * step : (((v + step - 1) / step) - 1) * step;
    } else {
        v += (direction > 0) ? step : -step;
    }
    set_value(mix, item, v);
}

void karaoke_mix_item_reset(karaoke_mix_t *mix, uint8_t item)
{
    mix_item_kind_t k = karaoke_mix_item_kind(item);

    if (k == MIX_KIND_ACTION) {
        return;
    }
    set_value(mix, item, (k == MIX_KIND_TONE) ? 0 : 100);
    if (k == MIX_KIND_CHANNEL) {
        uint16_t bit = (uint16_t)(1U << (item - MIX_ITEM_CHANNEL0));
        mix->mute &= (uint16_t)~bit;
        mix->solo &= (uint16_t)~bit;
    }
}

int karaoke_mix_channel_state(const karaoke_mix_t *mix, uint8_t channel)
{
    if (channel >= MIX_CHANNELS) return 0;
    if ((mix->solo & (1U << channel)) != 0U) return 2;
    if ((mix->mute & (1U << channel)) != 0U) return 1;
    return 0;
}

void karaoke_mix_item_click(karaoke_mix_t *mix, uint8_t item)
{
    if (karaoke_mix_item_kind(item) != MIX_KIND_CHANNEL) {
        karaoke_mix_item_reset(mix, item);
        return;
    }
    uint8_t ch = (uint8_t)(item - MIX_ITEM_CHANNEL0);
    uint16_t bit = (uint16_t)(1U << ch);
    int state = karaoke_mix_channel_state(mix, ch);

    mix->mute &= (uint16_t)~bit;
    mix->solo &= (uint16_t)~bit;
    if (state == 0) {
        mix->mute |= bit;
    } else if (state == 1) {
        mix->solo |= bit;
    }
}

bool karaoke_mix_item_changed(const karaoke_mix_t *mix, uint8_t item)
{
    mix_item_kind_t k = karaoke_mix_item_kind(item);

    if (k == MIX_KIND_ACTION) return false;
    if (karaoke_mix_item_value(mix, item) != ((k == MIX_KIND_TONE) ? 0 : 100)) return true;
    return k == MIX_KIND_CHANNEL && karaoke_mix_channel_state(mix, (uint8_t)(item - MIX_ITEM_CHANNEL0)) != 0;
}

/* ------------------------------------------------------------------------- */
/* Text form                                                                 */
/* ------------------------------------------------------------------------- */
size_t karaoke_mix_format(const karaoke_mix_t *mix, char *out, size_t out_size)
{
    size_t n = 0;
    int w;

    w = snprintf(out + n, out_size - n, "SEC=");
    if (w < 0 || (size_t)w >= out_size - n) return 0;
    n += (size_t)w;
    for (size_t i = 0; i < MIX_SECTIONS; i++) {
        w = snprintf(out + n, out_size - n, "%s%u", (i > 0U) ? "," : "", (unsigned)mix->section[i]);
        if (w < 0 || (size_t)w >= out_size - n) return 0;
        n += (size_t)w;
    }
    w = snprintf(out + n, out_size - n, "\nCHN=");
    if (w < 0 || (size_t)w >= out_size - n) return 0;
    n += (size_t)w;
    for (size_t i = 0; i < MIX_CHANNELS; i++) {
        w = snprintf(out + n, out_size - n, "%s%u", (i > 0U) ? "," : "", (unsigned)mix->channel[i]);
        if (w < 0 || (size_t)w >= out_size - n) return 0;
        n += (size_t)w;
    }
    w = snprintf(out + n, out_size - n, "\nMUTE=%u\nSOLO=%u\nREV=%u\nCHO=%u\nBASS=%d\nTREBLE=%d\n",
                 (unsigned)mix->mute, (unsigned)mix->solo, (unsigned)mix->reverb, (unsigned)mix->chorus,
                 (int)mix->bass, (int)mix->treble);
    if (w < 0 || (size_t)w >= out_size - n) return 0;
    return n + (size_t)w;
}

static int clamp_int(long v, int lo, int hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : (int)v);
}

static void parse_list(const char *s, uint8_t *dst, size_t count)
{
    for (size_t i = 0; i < count && *s != '\0'; i++) {
        char *end = NULL;
        long v = strtol(s, &end, 10);

        if (end == s) {
            break;
        }
        dst[i] = (uint8_t)clamp_int(v, 0, (int)MIX_PERCENT_MAX);
        s = (*end == ',') ? end + 1 : end;
    }
}

bool karaoke_mix_parse(karaoke_mix_t *mix, const char *text)
{
    bool any = false;
    const char *p = text;

    while (*p != '\0') {
        const char *eol = p;
        while (*eol != '\0' && *eol != '\n' && *eol != '\r') {
            eol++;
        }
        char line[128];
        size_t len = (size_t)(eol - p);
        if (len < sizeof(line)) {
            memcpy(line, p, len);
            line[len] = '\0';
            const char *eq = strchr(line, '=');
            if (eq != NULL && line[0] != '#') {
                const char *val = eq + 1;
                size_t klen = (size_t)(eq - line);
                long v = strtol(val, NULL, 10);

                if (klen == 3U && strncmp(line, "SEC", 3) == 0) {
                    parse_list(val, mix->section, MIX_SECTIONS);
                    any = true;
                } else if (klen == 3U && strncmp(line, "CHN", 3) == 0) {
                    parse_list(val, mix->channel, MIX_CHANNELS);
                    any = true;
                } else if (klen == 4U && strncmp(line, "MUTE", 4) == 0) {
                    mix->mute = (uint16_t)clamp_int(v, 0, 0xFFFF);
                    any = true;
                } else if (klen == 4U && strncmp(line, "SOLO", 4) == 0) {
                    mix->solo = (uint16_t)clamp_int(v, 0, 0xFFFF);
                    any = true;
                } else if (klen == 3U && strncmp(line, "REV", 3) == 0) {
                    mix->reverb = (uint8_t)clamp_int(v, 0, (int)MIX_PERCENT_MAX);
                    any = true;
                } else if (klen == 3U && strncmp(line, "CHO", 3) == 0) {
                    mix->chorus = (uint8_t)clamp_int(v, 0, (int)MIX_PERCENT_MAX);
                    any = true;
                } else if (klen == 4U && strncmp(line, "BASS", 4) == 0) {
                    mix->bass = (int8_t)clamp_int(v, -MIX_TONE_MAX_DB, MIX_TONE_MAX_DB);
                    any = true;
                } else if (klen == 6U && strncmp(line, "TREBLE", 6) == 0) {
                    mix->treble = (int8_t)clamp_int(v, -MIX_TONE_MAX_DB, MIX_TONE_MAX_DB);
                    any = true;
                }
            }
        }
        p = eol;
        while (*p == '\n' || *p == '\r') {
            p++;
        }
    }
    return any;
}

const char *karaoke_mix_program_name(uint8_t program)
{
    return GM_NAMES[program & 127U];
}

const char *karaoke_mix_kit_name(uint8_t program)
{
    switch (program & 127U) {
    case 8: return "Room";
    case 16: return "Power";
    case 24: return "Electronic";
    case 25: return "TR-808";
    case 32: return "Jazz";
    case 40: return "Brush";
    case 48: return "Orchestra";
    case 56: return "SFX";
    default: return "Standard";
    }
}

void karaoke_mix_build_view(mix_view_t *view, bool usage_valid, uint16_t sections, uint16_t channels)
{
    uint8_t n = 0;

    for (uint8_t i = 0; i < MIX_SECTIONS; i++) {
        if (!usage_valid || (sections & (1U << i)) != 0U) {
            view->item[n++] = i;
        }
    }
    view->item[n++] = MIX_ITEM_REVERB;
    view->item[n++] = MIX_ITEM_CHORUS;
    view->item[n++] = MIX_ITEM_BASS;
    view->item[n++] = MIX_ITEM_TREBLE;
    for (uint8_t ch = 0; ch < MIX_CHANNELS; ch++) {
        if (!usage_valid || (channels & (1U << ch)) != 0U) {
            view->item[n++] = (uint8_t)(MIX_ITEM_CHANNEL0 + ch);
        }
    }
    view->item[n++] = MIX_ITEM_RESET;
    view->item[n++] = MIX_ITEM_SAVE_ALL;
    view->item[n++] = MIX_ITEM_DONE;
    view->count = n;
}

bool karaoke_mix_song_path(char *out, size_t out_size, uint32_t song_code)
{
    int w = snprintf(out, out_size, "SD:/mix/%06lu.cfg", (unsigned long)song_code);

    return w > 0 && (size_t)w < out_size;
}
