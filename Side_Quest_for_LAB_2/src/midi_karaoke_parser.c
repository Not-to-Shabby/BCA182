/**
 * @file midi_karaoke_parser.c
 * @brief High-efficiency multi-track Standard MIDI File (SMF Format 0 & 1)
 *        sequencer and synchronized karaoke lyrics parser.
 *
 * The file is never held in RAM as a whole: each track reads through a one-sector window
 * that is refilled from the source (an array, or the SD card) as playback reaches its end.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include "midi_karaoke_parser.h"
#include "dtcm.h"
#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define SECTOR_BYTES        512U
#define STREAM_WINDOW_BYTES 512U    /* per-track read-ahead: one SD sector */
#define SCAN_WINDOW_BYTES   2048U   /* load-time scans walk one track at a time */
#define EVENT_TEXT_BYTES    64U
#define PENDING_LYRICS      32U

typedef struct {
    uint8_t *win;
    uint32_t win_cap;
    uint32_t win_abs;       /* absolute file offset of win[0] */
    uint32_t win_len;
    uint32_t file_off;      /* absolute file offset of the track's first data byte */
    uint32_t length;
    uint32_t pos;           /* track-relative offset of the next unread byte */
    uint32_t tick;
    uint8_t running_status;
    bool is_finished;
} track_cursor_t;

typedef enum {
    MEV_END,             /* no more bytes, or the source failed */
    MEV_END_OF_TRACK,
    MEV_NOTE_OFF,
    MEV_NOTE_ON,
    MEV_CC,
    MEV_PROGRAM,
    MEV_BEND,
    MEV_TEMPO,
    MEV_TEXT,
    MEV_SKIP
} ev_kind_t;

typedef struct {
    uint8_t chan, d1, d2;
    uint8_t text_len;
    uint32_t tempo;
    char text[EVENT_TEXT_BYTES];
} midi_event_t;

static struct {
    midi_synth_callbacks_t synth;
    midi_source_t src;
    track_cursor_t tracks[MIDI_MAX_TRACKS];
    uint16_t num_tracks;
    uint16_t format;
    uint16_t ppqn;
    uint32_t tempo_us;
    uint32_t current_tick;
    uint32_t elapsed_us_accum;
    uint32_t tick_us;               /* song time at the start of current_tick, in microseconds */
    uint32_t elapsed_ms;
    uint32_t total_duration_ms;
    bool is_playing;
    bool is_paused;

    /* Lyrics wait here until the notes they belong to are heard (MIDI_EVENT_LEAD_MS later) */
    struct {
        char text[48];
        bool is_newline;
        uint32_t due_us;
    } pending[PENDING_LYRICS];
    uint8_t pend_head;
    uint8_t pend_count;

    /* Lyric buffering */
    karaoke_lyric_msg_t lyric_queue[KARAOKE_LYRIC_QUEUE_SIZE];
    uint8_t lyric_head;
    uint8_t lyric_tail;
    uint8_t lyric_count;

    char current_line[KARAOKE_MAX_LINE_CHARS + 1];
    char upcoming_line[KARAOKE_MAX_LINE_CHARS + 1];
    char previous_line[KARAOKE_MAX_LINE_CHARS + 1];
    bool has_lyrics_started;
    bool line_ended;

    midi_stream_stats_t stats;

    struct k_mutex lock;
} s_midi;

#define SEQ_PERIOD_MS       10
#define SEQ_MAX_CATCHUP_US  100000U
#define SEQ_STACK_BYTES     2560    /* a failed read restarts the SD driver on this thread */

static struct k_timer s_midi_timer;
static int64_t s_last_tick_ticks;
K_SEM_DEFINE(s_midi_sem, 0, 1);

/* The timer only wakes the sequencer thread: the tick takes a mutex and calls into the
 * synthesizer, neither of which is allowed in interrupt context. */
static void midi_timer_handler(struct k_timer *timer_id)
{
    ARG_UNUSED(timer_id);
    k_sem_give(&s_midi_sem);
}

static void start_sequencer_clock(void)
{
    s_last_tick_ticks = k_uptime_ticks();
    k_timer_start(&s_midi_timer, K_MSEC(SEQ_PERIOD_MS), K_MSEC(SEQ_PERIOD_MS));
}

static void sequencer_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    for (;;) {
        k_sem_take(&s_midi_sem, K_FOREVER);
        int64_t now = k_uptime_ticks();
        uint64_t us = k_ticks_to_us_floor64(now - s_last_tick_ticks);
        s_last_tick_ticks = now;
        if (us > SEQ_MAX_CATCHUP_US) {
            us = SEQ_MAX_CATCHUP_US;
        }
        midi_karaoke_tick((uint32_t)us);
    }
}
K_THREAD_DEFINE(midi_seq_task, SEQ_STACK_BYTES, sequencer_thread, NULL, NULL, NULL, 2, 0, 0);

/* Windows are filled by SD DMA, so they live in ordinary SRAM, not in core-coupled RAM. */
static uint8_t s_track_win[MIDI_MAX_TRACKS][STREAM_WINDOW_BYTES] __attribute__((aligned(4)));
static uint8_t s_scan_win[SCAN_WINDOW_BYTES] __attribute__((aligned(4)));

static uint16_t read_be16(const uint8_t *data)
{
    return (uint16_t)((data[0] << 8) | data[1]);
}

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

/* Set when the source gave up on a read. A track cut short in the middle would leave its notes
 * sounding for ever and its instrument silent, so the whole song stops instead. */
static bool s_source_failed;

static bool source_read(uint32_t off, uint8_t *dst, uint32_t len)
{
    if (len > s_midi.src.size || off > s_midi.src.size - len) {
        return false;
    }
    if (s_midi.src.mem != NULL) {
        memcpy(dst, s_midi.src.mem + off, len);
        return true;
    }
    if (s_midi.src.read == NULL) {
        return false;
    }

    uint32_t t0 = k_cycle_get_32();
    bool ok = s_midi.src.read(s_midi.src.ctx, off, dst, len);
    uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32() - t0);

    s_midi.stats.reads++;
    s_midi.stats.bytes += len;
    if (us > s_midi.stats.max_read_us) {
        s_midi.stats.max_read_us = us;
    }
    return ok;
}

/* Reloads the window so that it holds absolute offset @p abs. The window starts on a sector
 * boundary, so a refill is an aligned read. */
static bool cursor_fill(track_cursor_t *c, uint32_t abs)
{
    uint32_t base = abs & ~(SECTOR_BYTES - 1U);
    uint32_t n = c->win_cap;

    if (n > s_midi.src.size - base) {
        n = s_midi.src.size - base;
    }
    if (!source_read(base, c->win, n)) {
        printk("[MIDI] Source read failed at offset %u\n", (unsigned)base);
        s_source_failed = true;
        return false;
    }
    c->win_abs = base;
    c->win_len = n;
    return true;
}

/* Next byte of the track. False at the end of the track, or when the source fails (the track
 * is then ended so that playback goes on with the others). */
static bool cursor_next(track_cursor_t *c, uint8_t *out)
{
    if (c->pos >= c->length) {
        return false;
    }
    uint32_t abs = c->file_off + c->pos;

    if (s_midi.src.mem != NULL) {
        *out = s_midi.src.mem[abs];
        c->pos++;
        return true;
    }
    if (abs < c->win_abs || abs - c->win_abs >= c->win_len) {
        if (!cursor_fill(c, abs)) {
            c->pos = c->length;
            return false;
        }
    }
    *out = c->win[abs - c->win_abs];
    c->pos++;
    return true;
}

static void cursor_skip(track_cursor_t *c, uint32_t n)
{
    c->pos += (n > c->length - c->pos) ? c->length - c->pos : n;
}

static uint32_t cursor_vlq(track_cursor_t *c)
{
    uint32_t val = 0;
    uint8_t b;

    for (int i = 0; i < 4 && cursor_next(c, &b); i++) {
        val = (val << 7) | (b & 0x7FU);
        if (!(b & 0x80U)) {
            break;
        }
    }
    return val;
}

/* Copies @p n bytes of the track into @p dst and ends the string; false if the track runs out. */
static bool cursor_read_text(track_cursor_t *c, char *dst, uint32_t n)
{
    uint8_t b = 0;

    dst[0] = '\0';
    for (uint32_t i = 0; i < n; i++) {
        if (!cursor_next(c, &b)) {
            return false;
        }
        dst[i] = (char)b;
        dst[i + 1U] = '\0';
    }
    return true;
}

/* Decodes the event that follows an already-read delta time. Meta text keeps its first
 * EVENT_TEXT_BYTES - 1 bytes; everything else is skipped without being read. */
static ev_kind_t read_event(track_cursor_t *c, midi_event_t *ev)
{
    uint8_t b;
    uint8_t status;
    uint8_t d1 = 0;
    uint8_t d2 = 0;
    bool have_d1 = false;

    ev->chan = 0;
    ev->d1 = 0;
    ev->d2 = 0;
    ev->tempo = 0;
    ev->text_len = 0;
    ev->text[0] = '\0';
    if (!cursor_next(c, &b)) {
        return MEV_END;
    }
    if ((b & 0x80U) != 0U) {
        status = b;
        if (status < 0xF0U) {
            c->running_status = status;
        }
    } else {
        status = c->running_status;
        d1 = b;
        have_d1 = true;
        if (status == 0U) {
            return MEV_END;
        }
    }

    if (status == 0xFFU) {
        uint8_t mt;
        if (!cursor_next(c, &mt)) {
            return MEV_END;
        }
        uint32_t ml = cursor_vlq(c);
        if (ml > c->length - c->pos) {
            ml = c->length - c->pos;
        }
        if (mt == 0x2FU) {
            cursor_skip(c, ml);
            return MEV_END_OF_TRACK;
        }
        if (mt == 0x51U && ml >= 3U) {
            uint8_t t[3];
            for (int i = 0; i < 3; i++) {
                if (!cursor_next(c, &t[i])) {
                    return MEV_END;
                }
            }
            ev->tempo = ((uint32_t)t[0] << 16) | ((uint32_t)t[1] << 8) | (uint32_t)t[2];
            cursor_skip(c, ml - 3U);
            return MEV_TEMPO;
        }
        if (mt == 0x01U || mt == 0x05U) {
            uint32_t n = (ml < EVENT_TEXT_BYTES - 1U) ? ml : EVENT_TEXT_BYTES - 1U;
            if (!cursor_read_text(c, ev->text, n)) {
                return MEV_END;
            }
            ev->text_len = (uint8_t)n;
            cursor_skip(c, ml - n);
            return MEV_TEXT;
        }
        cursor_skip(c, ml);
        return MEV_SKIP;
    }
    if (status == 0xF0U || status == 0xF7U) {
        cursor_skip(c, cursor_vlq(c));
        return MEV_SKIP;
    }
    if (status >= 0xF0U) {
        return MEV_SKIP;
    }

    uint8_t cmd = (uint8_t)(status & 0xF0U);

    if (!have_d1 && !cursor_next(c, &d1)) {
        return MEV_END;
    }
    if (cmd != 0xC0U && cmd != 0xD0U && !cursor_next(c, &d2)) {
        return MEV_END;
    }
    ev->chan = (uint8_t)(status & 0x0FU);
    ev->d1 = d1;
    ev->d2 = d2;
    switch (cmd) {
    case 0x80U: return MEV_NOTE_OFF;
    case 0x90U: return MEV_NOTE_ON;
    case 0xB0U: return MEV_CC;
    case 0xC0U: return MEV_PROGRAM;
    case 0xE0U: return MEV_BEND;
    default:    return MEV_SKIP;
    }
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
    s_midi.tick_us = 0;
    s_midi.pend_head = 0;
    s_midi.pend_count = 0;
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
    s_midi.line_ended = false;

    k_timer_init(&s_midi_timer, midi_timer_handler, NULL);

    k_mutex_unlock(&s_midi.lock);
    printk("[MIDI] Sequencer initialized.\n");
}

#define MAX_KARAOKE_LINES 256

typedef struct {
    uint32_t start_tick;
    char text[KARAOKE_MAX_LINE_CHARS + 1];
} lyric_line_entry_t;

static DTCM_BSS lyric_line_entry_t s_lyric_lines[MAX_KARAOKE_LINES];

static uint16_t s_total_lyric_lines = 0;
static uint16_t s_active_line_idx = 0;
static int s_lyric_track = -1;
static int8_t s_detected_melody = -1;

static void push_lyric_event(const char *text, bool is_newline)
{
    if (s_midi.lyric_count >= KARAOKE_LYRIC_QUEUE_SIZE) {
        /* Drop oldest if full */
        s_midi.lyric_head = (s_midi.lyric_head + 1) % KARAOKE_LYRIC_QUEUE_SIZE;
        s_midi.lyric_count--;
    }

    const char *safe_text = (text != NULL) ? text : "";

    karaoke_lyric_msg_t *msg = &s_midi.lyric_queue[s_midi.lyric_tail];
    snprintf(msg->text, sizeof(msg->text), "%s", safe_text);
    msg->is_newline = is_newline;
    msg->song_time_ms = s_midi.elapsed_ms;

    s_midi.lyric_tail = (s_midi.lyric_tail + 1) % KARAOKE_LYRIC_QUEUE_SIZE;
    s_midi.lyric_count++;

    if (is_newline) {
        if (safe_text[0] != '\0') {
            size_t cur_len = strlen(s_midi.current_line);
            if (cur_len + strlen(safe_text) < sizeof(s_midi.current_line)) {
                strncat(s_midi.current_line, safe_text, sizeof(s_midi.current_line) - cur_len - 1);
            }
        }
        s_midi.line_ended = true;
    } else {
        if (safe_text[0] == '\0') {
            return;
        }

        s_midi.has_lyrics_started = true;

        if (s_midi.line_ended) {
            /* Start of a new line: rotate the completed line to previous_line */
            if (s_midi.current_line[0] != '\0') {
                snprintf(s_midi.previous_line, sizeof(s_midi.previous_line), "%s", s_midi.current_line);
            }
            s_midi.current_line[0] = '\0';
            s_midi.line_ended = false;

            if (s_active_line_idx + 1 < s_total_lyric_lines) {
                s_active_line_idx++;
            }
        }

        /* Append syllable word-by-word to current active line */
        size_t cur_len = strlen(s_midi.current_line);
        if (cur_len + strlen(safe_text) < sizeof(s_midi.current_line)) {
            strncat(s_midi.current_line, safe_text, sizeof(s_midi.current_line) - cur_len - 1);
        }
    }
}

/* Lyrics are shown when the sound of their syllable reaches the speaker, not when the
 * sequencer reads them: the synthesizer plays notes MIDI_EVENT_LEAD_MS after they are read. */
static void queue_lyric(const char *text, bool is_newline)
{
    if (s_midi.pend_count >= PENDING_LYRICS) {
        push_lyric_event(s_midi.pending[s_midi.pend_head].text, s_midi.pending[s_midi.pend_head].is_newline);
        s_midi.pend_head = (uint8_t)((s_midi.pend_head + 1U) % PENDING_LYRICS);
        s_midi.pend_count--;
    }
    uint8_t slot = (uint8_t)((s_midi.pend_head + s_midi.pend_count) % PENDING_LYRICS);

    snprintf(s_midi.pending[slot].text, sizeof(s_midi.pending[slot].text), "%s", text);
    s_midi.pending[slot].is_newline = is_newline;
    s_midi.pending[slot].due_us = s_midi.tick_us + (uint32_t)MIDI_EVENT_LEAD_MS * 1000U;
    s_midi.pend_count++;
}

/* Shows every waiting lyric that is due at song time @p now_us (all of them when @p all). */
static void release_lyrics(uint32_t now_us, bool all)
{
    while (s_midi.pend_count > 0U) {
        uint8_t h = s_midi.pend_head;

        if (!all && (int32_t)(now_us - s_midi.pending[h].due_us) < 0) {
            break;
        }
        push_lyric_event(s_midi.pending[h].text, s_midi.pending[h].is_newline);
        s_midi.pend_head = (uint8_t)((h + 1U) % PENDING_LYRICS);
        s_midi.pend_count--;
    }
}

#define MELODY_TICK_WINDOW      12U
#define MELODY_MAX_SYLLABLES    768U
#define MELODY_MIN_HIT_PERCENT  25U

static DTCM_BSS uint32_t s_syllable_ticks[MELODY_MAX_SYLLABLES];
static uint32_t s_nsyl;

static void scan_cursor_init(track_cursor_t *c, uint16_t t)
{
    memset(c, 0, sizeof(*c));
    c->win = s_scan_win;
    c->win_cap = SCAN_WINDOW_BYTES;
    c->file_off = s_midi.tracks[t].file_off;
    c->length = s_midi.tracks[t].length;
}

static bool is_lyric_text(ev_kind_t k, const midi_event_t *ev)
{
    return k == MEV_TEXT && ev->text_len > 0U && ev->text[0] != '@';
}

/*
 * Walks the file once more: the track holding the most lyric events gives the lyric lines for
 * the display and the tick of every syllable for the melody search. Runs without the
 * sequencer lock (the sequencer is stopped); the display only ever reads finished lines.
 */
static void pre_parse_karaoke_lyrics(void)
{
    int best_track = -1;
    uint32_t best_count = 0;
    midi_event_t ev;
    track_cursor_t c;

    /* Reverse probe: in karaoke SMF files, lyrics are grouped on the last track.
     * Instrument tracks with no lyrics in their first 48 events are skipped immediately. */
    for (int t = (int)s_midi.num_tracks - 1; t >= 0; t--) {
        uint32_t cnt = 0;
        uint32_t ev_cnt = 0;

        scan_cursor_init(&c, (uint16_t)t);
        for (;;) {
            (void)cursor_vlq(&c);
            ev_kind_t k = read_event(&c, &ev);
            if (k == MEV_END || k == MEV_END_OF_TRACK) {
                break;
            }
            ev_cnt++;
            if (is_lyric_text(k, &ev)) {
                cnt++;
            }
            if (ev_cnt >= 48U && cnt == 0U) {
                break; /* Instrument track: skip */
            }
        }
        if (cnt > best_count) {
            best_count = cnt;
            best_track = t;
            if (cnt >= 16U) {
                break; /* Found primary lyric track: stop probing */
            }
        }
    }

    if (best_track < 0 || best_count < 2) {
        return;
    }
    s_lyric_track = best_track;

    scan_cursor_init(&c, (uint16_t)best_track);
    char line_buf[KARAOKE_MAX_LINE_CHARS + 1] = "";
    uint32_t line_start = 0;
    uint32_t cur_tick = 0;

    for (;;) {
        cur_tick += cursor_vlq(&c);
        ev_kind_t k = read_event(&c, &ev);
        if (k == MEV_END || k == MEV_END_OF_TRACK) {
            break;
        }
        if (!is_lyric_text(k, &ev)) {
            continue;
        }

        if (s_nsyl < MELODY_MAX_SYLLABLES &&
            (s_nsyl == 0U || s_syllable_ticks[s_nsyl - 1U] != cur_tick)) {
            s_syllable_ticks[s_nsyl++] = cur_tick;
        }
        if (s_total_lyric_lines >= MAX_KARAOKE_LINES) {
            if (s_nsyl >= MELODY_MAX_SYLLABLES) {
                break;
            }
            continue;
        }

        char *chunk = ev.text;
        bool has_nl = false;
        for (uint32_t i = 0; i < ev.text_len; i++) {
            if (chunk[i] == '\n' || chunk[i] == '\r' || chunk[i] == '/') {
                has_nl = true;
                chunk[i] = '\0';
                break;
            }
        }
        if (has_nl) {
            if (chunk[0] != '\0') {
                size_t cl = strlen(line_buf);
                if (cl + strlen(chunk) < sizeof(line_buf)) {
                    strncat(line_buf, chunk, sizeof(line_buf) - cl - 1);
                }
            }
            if (line_buf[0] != '\0') {
                lyric_line_entry_t *e = &s_lyric_lines[s_total_lyric_lines];
                e->start_tick = line_start;
                snprintf(e->text, sizeof(e->text), "%s", line_buf);
                s_total_lyric_lines++;
                line_buf[0] = '\0';
                line_start = 0;
            }
        } else {
            if (line_buf[0] == '\0') line_start = cur_tick;
            size_t cl = strlen(line_buf);
            if (cl + strlen(chunk) < sizeof(line_buf)) {
                strncat(line_buf, chunk, sizeof(line_buf) - cl - 1);
            }
        }
    }
    if (line_buf[0] != '\0' && s_total_lyric_lines < MAX_KARAOKE_LINES) {
        lyric_line_entry_t *e = &s_lyric_lines[s_total_lyric_lines];
        e->start_tick = line_start;
        snprintf(e->text, sizeof(e->text), "%s", line_buf);
        s_total_lyric_lines++;
    }
}

static int32_t nearest_syllable(uint32_t tick, uint32_t count)
{
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2U;
        if (s_syllable_ticks[mid] + MELODY_TICK_WINDOW < tick) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    return (lo < count && s_syllable_ticks[lo] <= tick + MELODY_TICK_WINDOW) ? (int32_t)lo : -1;
}

/*
 * The lead is the channel whose note onsets coincide with the lyric syllables. Each syllable
 * counts once per channel, so chords do not inflate a pad. The score is syllables^2 / notes,
 * so a busy accompaniment track that overlaps many syllables by chance loses to a sparse
 * track that follows them closely.
 */
static void detect_melody_channel(void)
{
    if (s_lyric_track < 0 || s_nsyl < 2U) {
        return;
    }

    static DTCM_BSS uint8_t reached[16][MELODY_MAX_SYLLABLES / 8U];
    uint32_t hits[16] = {0}, notes[16] = {0};
    midi_event_t ev;
    track_cursor_t c;

    uint32_t max_tick = s_syllable_ticks[s_nsyl - 1U] + MELODY_TICK_WINDOW;

    memset(reached, 0, sizeof(reached));
    for (uint16_t t = 0; t < s_midi.num_tracks; t++) {
        if (t == (uint16_t)s_lyric_track && s_midi.num_tracks > 1U) {
            continue; /* Skip lyric track: no musical notes */
        }
        uint32_t tick = 0;
        uint32_t ev_cnt = 0;
        uint32_t t_notes = 0;

        scan_cursor_init(&c, t);
        for (;;) {
            tick += cursor_vlq(&c);
            if (tick > max_tick) {
                break; /* Past the end of all lyrics in the song */
            }
            ev_kind_t k = read_event(&c, &ev);
            if (k == MEV_END || k == MEV_END_OF_TRACK) {
                break;
            }
            ev_cnt++;
            if (k != MEV_NOTE_ON || ev.d2 == 0U) {
                if (ev_cnt >= 48U && t_notes == 0U) {
                    break; /* Conductor / tempo / metadata track: skip */
                }
                continue;
            }
            t_notes++;
            uint8_t ch = ev.chan;
            notes[ch]++;
            int32_t si = nearest_syllable(tick, s_nsyl);
            if (si >= 0 && !(reached[ch][si >> 3] & (1U << (si & 7)))) {
                reached[ch][si >> 3] |= (uint8_t)(1U << (si & 7));
                hits[ch]++;
            }
        }
    }

    uint64_t best_score = 0;
    for (int ch = 0; ch < 16; ch++) {
        if (ch == 9 || notes[ch] == 0U || hits[ch] * 100U < s_nsyl * MELODY_MIN_HIT_PERCENT) {
            continue;
        }
        uint64_t score = ((uint64_t)hits[ch] * hits[ch] * 1000U) / notes[ch];
        if (score > best_score) {
            best_score = score;
            s_detected_melody = (int8_t)ch;
        }
    }
}

int8_t midi_karaoke_get_melody_channel(void)
{
    return s_detected_melody;
}

void midi_karaoke_get_stream_stats(midi_stream_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    k_mutex_lock(&s_midi.lock, K_FOREVER);
    *out = s_midi.stats;
    k_mutex_unlock(&s_midi.lock);
#if defined(__arm__)
    size_t unused = 0;
    out->seq_stack_unused = (k_thread_stack_space_get(midi_seq_task, &unused) == 0) ? (uint32_t)unused : 0U;
#else
    out->seq_stack_unused = 0;
#endif
}

bool midi_karaoke_load(const midi_source_t *src)
{
    if (src == NULL || src->size < 14U || (src->mem == NULL && src->read == NULL)) {
        return false;
    }

    k_mutex_lock(&s_midi.lock, K_FOREVER);

    s_midi.src = *src;
    s_midi.num_tracks = 0;
    s_midi.is_playing = false;
    s_midi.is_paused = false;
    s_source_failed = false;
    memset(&s_midi.stats, 0, sizeof(s_midi.stats));

    uint8_t hdr[14];
    if (!source_read(0, hdr, sizeof(hdr)) || memcmp(hdr, "MThd", 4) != 0) {
        printk("[MIDI] Invalid header magic!\n");
        k_mutex_unlock(&s_midi.lock);
        return false;
    }
    uint32_t header_len = read_be32(hdr + 4);
    if (header_len < 6U) {
        k_mutex_unlock(&s_midi.lock);
        return false;
    }

    s_midi.format = read_be16(hdr + 8);
    uint16_t tracks_in_file = read_be16(hdr + 10);
    s_midi.ppqn = read_be16(hdr + 12);
    if (s_midi.ppqn == 0) s_midi.ppqn = 120;

    uint32_t pos = 8U + header_len;
    uint8_t chunk[8];

    while (pos <= src->size - 8U && s_midi.num_tracks < MIDI_MAX_TRACKS) {
        if (!source_read(pos, chunk, sizeof(chunk))) {
            break;
        }
        uint32_t chunk_len = read_be32(chunk + 4);
        pos += 8U;
        if (chunk_len > src->size - pos) {
            chunk_len = src->size - pos;
        }
        if (memcmp(chunk, "MTrk", 4) == 0) {
            track_cursor_t *cur = &s_midi.tracks[s_midi.num_tracks];
            memset(cur, 0, sizeof(*cur));
            cur->win = s_track_win[s_midi.num_tracks];
            cur->win_cap = STREAM_WINDOW_BYTES;
            cur->file_off = pos;
            cur->length = chunk_len;
            cur->tick = cursor_vlq(cur);
            s_midi.num_tracks++;
        }
        pos += chunk_len;
    }

    if (s_midi.num_tracks == 0) {
        printk("[MIDI] No tracks found\n");
        k_mutex_unlock(&s_midi.lock);
        return false;
    }

    s_midi.current_tick = 0;
    s_midi.elapsed_us_accum = 0;
    s_midi.tick_us = 0;
    s_midi.pend_head = 0;
    s_midi.pend_count = 0;
    s_midi.elapsed_ms = 0;
    s_midi.tempo_us = 500000; /* 120 BPM */

    s_midi.lyric_head = 0;
    s_midi.lyric_tail = 0;
    s_midi.lyric_count = 0;
    memset(s_midi.current_line, 0, sizeof(s_midi.current_line));
    memset(s_midi.upcoming_line, 0, sizeof(s_midi.upcoming_line));
    memset(s_midi.previous_line, 0, sizeof(s_midi.previous_line));
    s_midi.has_lyrics_started = false;
    s_midi.line_ended = false;

    s_total_lyric_lines = 0;
    s_active_line_idx = 0;
    s_lyric_track = -1;
    s_detected_melody = -1;
    s_nsyl = 0;

    k_mutex_unlock(&s_midi.lock);

    /* The scans read the whole file from the card; keep the display responsive meanwhile. */
    pre_parse_karaoke_lyrics();
    detect_melody_channel();

    if (s_source_failed) {
        printk("[MIDI] The song could not be read completely; not playing it\n");
        s_midi.num_tracks = 0;
        return false;
    }

    printk("[MIDI] Loaded SMF Format %u, %u tracks (file specified %u), PPQN=%u\n",
           s_midi.format, s_midi.num_tracks, tracks_in_file, s_midi.ppqn);
    if (tracks_in_file > s_midi.num_tracks) {
        printk("[MIDI] Playing the first %u tracks only\n", (unsigned)s_midi.num_tracks);
    }
    return true;
}

bool midi_karaoke_load_memory(const uint8_t *data, uint32_t length)
{
    midi_source_t src = { .mem = data, .size = length };

    return data != NULL && midi_karaoke_load(&src);
}

void midi_karaoke_play(void)
{
    k_mutex_lock(&s_midi.lock, K_FOREVER);
    s_midi.is_playing = true;
    s_midi.is_paused = false;
    start_sequencer_clock();
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
        start_sequencer_clock();
    }
    k_mutex_unlock(&s_midi.lock);
}

void midi_karaoke_stop(void)
{
    k_mutex_lock(&s_midi.lock, K_FOREVER);
    s_midi.is_playing = false;
    s_midi.is_paused = false;
    s_midi.pend_count = 0;
    k_timer_stop(&s_midi_timer);
    if (s_midi.synth.all_notes_off) {
        s_midi.synth.all_notes_off();
    }
    k_mutex_unlock(&s_midi.lock);
}

/* Process next event on a track */
static void announce_event_time(void)
{
    if (s_midi.synth.event_time) {
        s_midi.synth.event_time(s_midi.tick_us);
    }
}

static void process_track_event(track_cursor_t *cur)
{
    midi_event_t ev;

    switch (read_event(cur, &ev)) {
    case MEV_END:
    case MEV_END_OF_TRACK:
        cur->is_finished = true;
        return;
    case MEV_TEMPO:
        if (ev.tempo > 0) {
            s_midi.tempo_us = ev.tempo;
        }
        break;
    case MEV_TEXT: {
        char buf[48];
        uint32_t n = ev.text_len < sizeof(buf) - 1U ? ev.text_len : sizeof(buf) - 1U;
        bool is_nl = false;

        memcpy(buf, ev.text, n);
        buf[n] = '\0';
        for (uint32_t i = 0; i < n; i++) {
            if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == '/') {
                is_nl = true;
                buf[i] = '\0';
                break;
            }
        }
        if (buf[0] != '\0' || is_nl) {
            queue_lyric(buf, is_nl);
        }
        break;
    }
    case MEV_NOTE_OFF:
        if (s_midi.synth.note_off) {
            announce_event_time();
            s_midi.synth.note_off(ev.chan, ev.d1, ev.d2);
        }
        break;
    case MEV_NOTE_ON:
        announce_event_time();
        if (ev.d2 == 0) {
            if (s_midi.synth.note_off) s_midi.synth.note_off(ev.chan, ev.d1, 0);
        } else if (s_midi.synth.note_on) {
            s_midi.synth.note_on(ev.chan, ev.d1, ev.d2);
        }
        break;
    case MEV_CC:
        if (s_midi.synth.control_change) {
            announce_event_time();
            s_midi.synth.control_change(ev.chan, ev.d1, ev.d2);
        }
        break;
    case MEV_PROGRAM:
        if (s_midi.synth.program_change) {
            announce_event_time();
            s_midi.synth.program_change(ev.chan, ev.d1);
        }
        break;
    case MEV_BEND:
        if (s_midi.synth.pitch_bend) {
            announce_event_time();
            s_midi.synth.pitch_bend(ev.chan, (uint16_t)(((uint16_t)ev.d2 << 7) | (uint16_t)ev.d1));
        }
        break;
    case MEV_SKIP:
    default:
        break;
    }

    /* Advance to next delta tick */
    if (cur->pos < cur->length) {
        cur->tick += cursor_vlq(cur);
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

    /* A stop or reload may have run while this tick waited for the lock. */
    if (!s_midi.is_playing || s_midi.is_paused || s_midi.num_tracks == 0) {
        k_mutex_unlock(&s_midi.lock);
        return;
    }

    s_midi.elapsed_us_accum += elapsed_us;
    s_midi.elapsed_ms += elapsed_us / 1000;

    /* Song time now: whole ticks played plus the part of the next one that has passed. */
    if (s_midi.synth.song_clock) {
        s_midi.synth.song_clock(s_midi.tick_us + s_midi.elapsed_us_accum);
    }

    /* Microseconds per MIDI tick: us_per_tick = tempo_us / ppqn */
    uint32_t us_per_tick = s_midi.tempo_us / s_midi.ppqn;
    if (us_per_tick == 0) us_per_tick = 1;

    while (s_midi.elapsed_us_accum >= us_per_tick) {
        s_midi.elapsed_us_accum -= us_per_tick;
        s_midi.current_tick++;
        s_midi.tick_us += us_per_tick;

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

        if (s_source_failed) {
            s_midi.is_playing = false;
            release_lyrics(0, true);
            if (s_midi.synth.all_notes_off) {
                s_midi.synth.all_notes_off();
            }
            printk("[MIDI] Playback stopped at tick %u: the song file became unreadable\n",
                   s_midi.current_tick);
            break;
        }
        if (!any_active) {
            /* Playback finished */
            s_midi.is_playing = false;
            release_lyrics(0, true);
            if (s_midi.synth.all_notes_off) {
                s_midi.synth.all_notes_off();
            }
            printk("[MIDI] Playback completed at tick %u\n", s_midi.current_tick);
            break;
        }
    }
    release_lyrics(s_midi.tick_us + s_midi.elapsed_us_accum, false);

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

    if (s_total_lyric_lines == 0 && !s_midi.has_lyrics_started) {
        snprintf(out_status->current_lyric_line, sizeof(out_status->current_lyric_line), "  [Instrumental Track]  ");
        out_status->upcoming_lyric_line[0] = '\0';
        out_status->previous_lyric_line[0] = '\0';
        out_status->has_lyrics_started = false;
    } else if (!s_midi.has_lyrics_started) {
        /* Intro state: before first line starts */
        snprintf(out_status->current_lyric_line, sizeof(out_status->current_lyric_line), "  [Music / Intro]  ");
        if (s_total_lyric_lines > 0) {
            snprintf(out_status->upcoming_lyric_line, sizeof(out_status->upcoming_lyric_line), "%s", s_lyric_lines[0].text);
        } else {
            out_status->upcoming_lyric_line[0] = '\0';
        }
        out_status->previous_lyric_line[0] = '\0';
        out_status->has_lyrics_started = false;
    } else {
        /* Word-by-word active singing line: grows syllable-by-syllable in real time! */
        snprintf(out_status->current_lyric_line, sizeof(out_status->current_lyric_line), "%s", s_midi.current_line);

        /* Upcoming line from pre-parsed index */
        if (s_active_line_idx + 1 < s_total_lyric_lines) {
            snprintf(out_status->upcoming_lyric_line, sizeof(out_status->upcoming_lyric_line), "%s", s_lyric_lines[s_active_line_idx + 1].text);
        } else {
            snprintf(out_status->upcoming_lyric_line, sizeof(out_status->upcoming_lyric_line), "  [Outro / End]  ");
        }

        /* Previous completed line */
        snprintf(out_status->previous_lyric_line, sizeof(out_status->previous_lyric_line), "%s", s_midi.previous_line);
        out_status->has_lyrics_started = true;
    }

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
