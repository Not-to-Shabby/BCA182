/**
 * @file midi_karaoke_parser.c
 * @brief High-efficiency multi-track Standard MIDI File (SMF Format 0 & 1)
 *        sequencer and synchronized karaoke lyrics parser.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include "midi_karaoke_parser.h"
#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

typedef struct {
    const uint8_t *data;
    uint32_t length;
    uint32_t pos;
    uint32_t tick;
    uint8_t running_status;
    bool is_finished;
} track_cursor_t;

static struct {
    midi_synth_callbacks_t synth;
    track_cursor_t tracks[MIDI_MAX_TRACKS];
    uint16_t num_tracks;
    uint16_t format;
    uint16_t ppqn;
    uint32_t tempo_us;
    uint32_t current_tick;
    uint32_t elapsed_us_accum;
    uint32_t elapsed_ms;
    uint32_t total_duration_ms;
    bool is_playing;
    bool is_paused;

    /* Lyric buffering */
    karaoke_lyric_msg_t lyric_queue[KARAOKE_LYRIC_QUEUE_SIZE];
    uint8_t lyric_head;
    uint8_t lyric_tail;
    uint8_t lyric_count;

    char current_line[KARAOKE_MAX_LINE_CHARS + 1];
    char upcoming_line[KARAOKE_MAX_LINE_CHARS + 1];
    char previous_line[KARAOKE_MAX_LINE_CHARS + 1];
    bool has_lyrics_started;

    struct k_mutex lock;
} s_midi;

static struct k_timer s_midi_timer;

static void midi_timer_handler(struct k_timer *timer_id)
{
    ARG_UNUSED(timer_id);
    midi_karaoke_tick(10000); /* 10 ms = 10,000 microseconds */
}

/* Helper: Read Variable Length Quantity (VLQ) */
static uint32_t read_vlq(const uint8_t *data, uint32_t len, uint32_t *pos)
{
    uint32_t val = 0;
    while (*pos < len) {
        uint8_t b = data[(*pos)++];
        val = (val << 7) | (b & 0x7FU);
        if (!(b & 0x80U)) {
            break;
        }
    }
    return val;
}

static uint16_t read_be16(const uint8_t *data)
{
    return (uint16_t)((data[0] << 8) | data[1]);
}

static uint32_t read_be32(const uint8_t *data)
{
    return (uint32_t)((data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3]);
}

void midi_karaoke_init(const midi_synth_callbacks_t *synth_cb)
{
    k_mutex_init(&s_midi.lock);
    k_mutex_lock(&s_midi.lock, K_FOREVER);

    if (synth_cb) {
        s_midi.synth = *synth_cb;
    } else {
        memset(&s_midi.synth, 0, sizeof(s_midi.synth));
    }

    s_midi.num_tracks = 0;
    s_midi.format = 0;
    s_midi.ppqn = 120;
    s_midi.tempo_us = 500000; /* 120 BPM */
    s_midi.current_tick = 0;
    s_midi.elapsed_us_accum = 0;
    s_midi.elapsed_ms = 0;
    s_midi.total_duration_ms = 0;
    s_midi.is_playing = false;
    s_midi.is_paused = false;

    s_midi.lyric_head = 0;
    s_midi.lyric_tail = 0;
    s_midi.lyric_count = 0;

    memset(s_midi.current_line, 0, sizeof(s_midi.current_line));
    memset(s_midi.upcoming_line, 0, sizeof(s_midi.upcoming_line));
    memset(s_midi.previous_line, 0, sizeof(s_midi.previous_line));
    s_midi.has_lyrics_started = false;

    k_timer_init(&s_midi_timer, midi_timer_handler, NULL);

    k_mutex_unlock(&s_midi.lock);
    printk("[MIDI] Sequencer initialized.\n");
}

static void push_lyric_event(const char *text, bool is_newline)
{
    if (s_midi.lyric_count >= KARAOKE_LYRIC_QUEUE_SIZE) {
        /* Drop oldest if full */
        s_midi.lyric_head = (s_midi.lyric_head + 1) % KARAOKE_LYRIC_QUEUE_SIZE;
        s_midi.lyric_count--;
    }

    karaoke_lyric_msg_t *msg = &s_midi.lyric_queue[s_midi.lyric_tail];
    snprintf(msg->text, sizeof(msg->text), "%s", text);
    msg->is_newline = is_newline;
    msg->song_time_ms = s_midi.elapsed_ms;

    s_midi.lyric_tail = (s_midi.lyric_tail + 1) % KARAOKE_LYRIC_QUEUE_SIZE;
    s_midi.lyric_count++;

    if (is_newline) {
        /* Rotate lyric lines */
        snprintf(s_midi.previous_line, sizeof(s_midi.previous_line), "%s", s_midi.current_line);
        snprintf(s_midi.current_line, sizeof(s_midi.current_line), "%s", s_midi.upcoming_line);
        memset(s_midi.upcoming_line, 0, sizeof(s_midi.upcoming_line));
    } else {
        if (text[0] != '\0') {
            s_midi.has_lyrics_started = true;
        }
        /* Append syllable to current line if space permits */
        size_t cur_len = strlen(s_midi.current_line);
        if (cur_len + strlen(text) < sizeof(s_midi.current_line)) {
            strncat(s_midi.current_line, text, sizeof(s_midi.current_line) - cur_len - 1);
        }
    }
}

bool midi_karaoke_load_memory(const uint8_t *data, uint32_t length)
{
    if (!data || length < 14) {
        return false;
    }

    /* Check MThd header */
    if (memcmp(data, "MThd", 4) != 0) {
        printk("[MIDI] Invalid header magic!\n");
        return false;
    }

    uint32_t header_len = read_be32(data + 4);
    if (header_len < 6) {
        return false;
    }

    k_mutex_lock(&s_midi.lock, K_FOREVER);

    s_midi.format = read_be16(data + 8);
    uint16_t tracks_in_file = read_be16(data + 10);
    s_midi.ppqn = read_be16(data + 12);
    if (s_midi.ppqn == 0) s_midi.ppqn = 120;

    s_midi.num_tracks = 0;
    uint32_t pos = 8 + header_len;

    while (pos + 8 <= length && s_midi.num_tracks < MIDI_MAX_TRACKS) {
        if (memcmp(data + pos, "MTrk", 4) != 0) {
            /* Skip unknown chunk */
            uint32_t chunk_len = read_be32(data + pos + 4);
            pos += 8 + chunk_len;
            continue;
        }

        uint32_t track_len = read_be32(data + pos + 4);
        pos += 8;

        if (pos + track_len > length) {
            track_len = length - pos;
        }

        track_cursor_t *cur = &s_midi.tracks[s_midi.num_tracks];
        cur->data = data + pos;
        cur->length = track_len;
        cur->pos = 0;
        cur->running_status = 0;
        cur->is_finished = false;

        /* Read initial delta tick */
        cur->tick = read_vlq(cur->data, cur->length, &cur->pos);

        s_midi.num_tracks++;
        pos += track_len;
    }

    s_midi.current_tick = 0;
    s_midi.elapsed_us_accum = 0;
    s_midi.elapsed_ms = 0;
    s_midi.tempo_us = 500000; /* 120 BPM */
    s_midi.is_playing = false;
    s_midi.is_paused = false;

    s_midi.lyric_head = 0;
    s_midi.lyric_tail = 0;
    s_midi.lyric_count = 0;
    memset(s_midi.current_line, 0, sizeof(s_midi.current_line));
    memset(s_midi.upcoming_line, 0, sizeof(s_midi.upcoming_line));
    memset(s_midi.previous_line, 0, sizeof(s_midi.previous_line));
    s_midi.has_lyrics_started = false;

    k_mutex_unlock(&s_midi.lock);

    printk("[MIDI] Loaded SMF Format %u, %u tracks (file specified %u), PPQN=%u\n",
           s_midi.format, s_midi.num_tracks, tracks_in_file, s_midi.ppqn);
    return true;
}

void midi_karaoke_play(void)
{
    k_mutex_lock(&s_midi.lock, K_FOREVER);
    s_midi.is_playing = true;
    s_midi.is_paused = false;
    k_timer_start(&s_midi_timer, K_MSEC(10), K_MSEC(10));
    k_mutex_unlock(&s_midi.lock);
}

void midi_karaoke_pause(void)
{
    k_mutex_lock(&s_midi.lock, K_FOREVER);
    s_midi.is_paused = !s_midi.is_paused;
    if (s_midi.is_paused) {
        k_timer_stop(&s_midi_timer);
        if (s_midi.synth.all_notes_off) {
            s_midi.synth.all_notes_off();
        }
    } else {
        k_timer_start(&s_midi_timer, K_MSEC(10), K_MSEC(10));
    }
    k_mutex_unlock(&s_midi.lock);
}

void midi_karaoke_stop(void)
{
    k_mutex_lock(&s_midi.lock, K_FOREVER);
    s_midi.is_playing = false;
    s_midi.is_paused = false;
    k_timer_stop(&s_midi_timer);
    if (s_midi.synth.all_notes_off) {
        s_midi.synth.all_notes_off();
    }
    k_mutex_unlock(&s_midi.lock);
}

/* Process next event on a track */
static void process_track_event(track_cursor_t *cur)
{
    if (cur->pos >= cur->length) {
        cur->is_finished = true;
        return;
    }

    uint8_t status = cur->data[cur->pos++];
    if (!(status & 0x80U)) {
        /* Running status: byte is actually the first data byte */
        cur->pos--;
        status = cur->running_status;
    } else {
        cur->running_status = status;
    }

    if (status == 0xFF) {
        /* Meta Event */
        if (cur->pos >= cur->length) { cur->is_finished = true; return; }
        uint8_t meta_type = cur->data[cur->pos++];
        uint32_t meta_len = read_vlq(cur->data, cur->length, &cur->pos);
        if (cur->pos + meta_len > cur->length) {
            meta_len = cur->length - cur->pos;
        }

        const uint8_t *meta_data = cur->data + cur->pos;
        cur->pos += meta_len;

        if (meta_type == 0x2F) {
            /* End of Track */
            cur->is_finished = true;
            return;
        } else if (meta_type == 0x51 && meta_len >= 3) {
            /* Set Tempo */
            uint32_t tempo = ((uint32_t)meta_data[0] << 16) |
                             ((uint32_t)meta_data[1] << 8)  |
                             ((uint32_t)meta_data[2]);
            if (tempo > 0) {
                s_midi.tempo_us = tempo;
            }
        } else if (meta_type == 0x01 || meta_type == 0x05) {
            /* Text or Lyric */
            char buf[48];
            uint32_t cpy_len = meta_len < sizeof(buf) - 1 ? meta_len : sizeof(buf) - 1;
            memcpy(buf, meta_data, cpy_len);
            buf[cpy_len] = '\0';

            /* Check for line break markers */
            bool is_nl = false;
            for (uint32_t i = 0; i < cpy_len; i++) {
                if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == '/') {
                    is_nl = true;
                    buf[i] = '\0';
                    break;
                }
            }

            if (strlen(buf) > 0 || is_nl) {
                push_lyric_event(buf, is_nl);
            }
        }
    } else if (status == 0xF0 || status == 0xF7) {
        /* Sysex: skip */
        uint32_t sysex_len = read_vlq(cur->data, cur->length, &cur->pos);
        cur->pos += sysex_len;
    } else {
        /* MIDI Channel Voice Message */
        uint8_t cmd  = (uint8_t)(status & 0xF0U);
        uint8_t chan = (uint8_t)(status & 0x0FU);

        if (cmd == 0x80) {
            /* Note Off */
            if (cur->pos + 2 <= cur->length) {
                uint8_t note = cur->data[cur->pos++];
                uint8_t vel  = cur->data[cur->pos++];
                if (s_midi.synth.note_off) s_midi.synth.note_off(chan, note, vel);
            }
        } else if (cmd == 0x90) {
            /* Note On */
            if (cur->pos + 2 <= cur->length) {
                uint8_t note = cur->data[cur->pos++];
                uint8_t vel  = cur->data[cur->pos++];
                if (vel == 0) {
                    if (s_midi.synth.note_off) s_midi.synth.note_off(chan, note, 0);
                } else {
                    if (s_midi.synth.note_on) s_midi.synth.note_on(chan, note, vel);
                }
            }
        } else if (cmd == 0xA0) {
            /* Polyphonic Key Pressure */
            cur->pos += 2;
        } else if (cmd == 0xB0) {
            /* Control Change */
            if (cur->pos + 2 <= cur->length) {
                uint8_t ctrl = cur->data[cur->pos++];
                uint8_t val  = cur->data[cur->pos++];
                if (ctrl == 123 || ctrl == 120) {
                    if (s_midi.synth.all_notes_off) s_midi.synth.all_notes_off();
                } else if (s_midi.synth.control_change) {
                    s_midi.synth.control_change(chan, ctrl, val);
                }
            }
        } else if (cmd == 0xC0) {
            /* Program Change */
            if (cur->pos + 1 <= cur->length) {
                uint8_t prog = cur->data[cur->pos++];
                if (s_midi.synth.program_change) s_midi.synth.program_change(chan, prog);
            }
        } else if (cmd == 0xD0) {
            /* Channel Pressure */
            cur->pos += 1;
        } else if (cmd == 0xE0) {
            /* Pitch Bend */
            if (cur->pos + 2 <= cur->length) {
                uint8_t lsb = cur->data[cur->pos++];
                uint8_t msb = cur->data[cur->pos++];
                uint16_t bend = (uint16_t)(((uint16_t)msb << 7) | (uint16_t)lsb);
                if (s_midi.synth.pitch_bend) s_midi.synth.pitch_bend(chan, bend);
            }
        }
    }

    /* Advance to next delta tick */
    if (!cur->is_finished && cur->pos < cur->length) {
        uint32_t delta = read_vlq(cur->data, cur->length, &cur->pos);
        cur->tick += delta;
    } else {
        cur->is_finished = true;
    }
}

void midi_karaoke_tick(uint32_t elapsed_us)
{
    if (!s_midi.is_playing || s_midi.is_paused || s_midi.num_tracks == 0) {
        return;
    }

    k_mutex_lock(&s_midi.lock, K_FOREVER);

    s_midi.elapsed_us_accum += elapsed_us;
    s_midi.elapsed_ms += elapsed_us / 1000;

    /* Microseconds per MIDI tick: us_per_tick = tempo_us / ppqn */
    uint32_t us_per_tick = s_midi.tempo_us / s_midi.ppqn;
    if (us_per_tick == 0) us_per_tick = 1;

    while (s_midi.elapsed_us_accum >= us_per_tick) {
        s_midi.elapsed_us_accum -= us_per_tick;
        s_midi.current_tick++;

        /* Find all tracks that have an event due at or before current_tick */
        bool any_active = false;
        for (uint16_t i = 0; i < s_midi.num_tracks; i++) {
            track_cursor_t *cur = &s_midi.tracks[i];
            if (cur->is_finished) continue;

            any_active = true;
            while (!cur->is_finished && cur->tick <= s_midi.current_tick) {
                process_track_event(cur);
            }
        }

        if (!any_active) {
            /* Playback finished */
            s_midi.is_playing = false;
            if (s_midi.synth.all_notes_off) {
                s_midi.synth.all_notes_off();
            }
            printk("[MIDI] Playback completed at tick %u\n", s_midi.current_tick);
            break;
        }
    }

    k_mutex_unlock(&s_midi.lock);
}

void midi_karaoke_get_status(midi_player_status_t *out_status)
{
    if (!out_status) return;

    k_mutex_lock(&s_midi.lock, K_FOREVER);
    out_status->is_playing = s_midi.is_playing;
    out_status->is_paused = s_midi.is_paused;
    out_status->format = s_midi.format;
    out_status->num_tracks = s_midi.num_tracks;
    out_status->ppqn = s_midi.ppqn;
    out_status->tempo_us = s_midi.tempo_us;
    out_status->current_tick = s_midi.current_tick;
    out_status->total_ticks = s_midi.total_duration_ms;
    out_status->elapsed_ms = s_midi.elapsed_ms;
    out_status->duration_ms = s_midi.total_duration_ms;

    if (s_midi.tempo_us > 0) {
        out_status->bpm = (uint16_t)(60000000UL / s_midi.tempo_us);
    } else {
        out_status->bpm = 120;
    }

    out_status->has_lyrics_started = s_midi.has_lyrics_started;

    snprintf(out_status->current_lyric_line, sizeof(out_status->current_lyric_line), "%s", s_midi.current_line);
    snprintf(out_status->upcoming_lyric_line, sizeof(out_status->upcoming_lyric_line), "%s", s_midi.upcoming_line);
    snprintf(out_status->previous_lyric_line, sizeof(out_status->previous_lyric_line), "%s", s_midi.previous_line);

    k_mutex_unlock(&s_midi.lock);
}

bool midi_karaoke_pop_lyric(karaoke_lyric_msg_t *out_msg)
{
    if (!out_msg) return false;

    k_mutex_lock(&s_midi.lock, K_FOREVER);
    if (s_midi.lyric_count == 0) {
        k_mutex_unlock(&s_midi.lock);
        return false;
    }

    *out_msg = s_midi.lyric_queue[s_midi.lyric_head];
    s_midi.lyric_head = (s_midi.lyric_head + 1) % KARAOKE_LYRIC_QUEUE_SIZE;
    s_midi.lyric_count--;

    k_mutex_unlock(&s_midi.lock);
    return true;
}
