/**
 * @file bench.c
 * @brief Renders the real yamaha_fm_synth.c and midi_karaoke_parser.c on the PC to a WAV
 *        file and prints measurements, so the FM voice can be judged without the board.
 *
 *   bench note <program> <midi_note> <velocity> <seconds> <out.wav> [channel]
 *   bench song <file.mid> <seconds> <out.wav> [volume_percent]
 *
 * Build and run through tools/synth_bench/run.py.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zephyr/kernel.h"
#include "zephyr/sys/printk.h"
#include "../../src/synth_dsp.c"
#include "../../src/yamaha_fm_synth.c"
#include "../../src/midi_karaoke_parser.c"

#define RATE        44100
#define BLOCK_WORDS 256          /* same block the audio producer asks for */

static void write_wav(const char *path, const int16_t *pcm, size_t frames)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    uint32_t data_bytes = (uint32_t)(frames * 4U);
    uint32_t riff = 36U + data_bytes, rate = RATE, byte_rate = RATE * 4U, fmt_len = 16U;
    uint16_t pcm_tag = 1, channels = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_len, 4, 1, f); fwrite(&pcm_tag, 2, 1, f); fwrite(&channels, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
    fwrite(pcm, 4, frames, f);
    fclose(f);
}

static void report(const int16_t *pcm, size_t frames, const char *label)
{
    double sum_sq = 0.0;
    long clipped = 0;
    int peak = 0;
    for (size_t i = 0; i < frames * 2; i++) {
        int v = pcm[i];
        int a = v < 0 ? -v : v;
        if (a > peak) peak = a;
        if (a >= 32767) clipped++;
        sum_sq += (double)v * (double)v;
    }
    double rms = sqrt(sum_sq / (double)(frames * 2));
    printf("%s: peak=%d rms_dbfs=%.1f clipped_samples=%ld (%.2f%%)\n", label, peak,
           rms > 0 ? 20.0 * log10(rms / 32768.0) : -999.0, clipped,
           100.0 * (double)clipped / (double)(frames * 2));
}

static void render_frames(int16_t *dst, size_t frames)
{
    for (size_t done = 0; done < frames; done += BLOCK_WORDS / 2) {
        size_t n = frames - done < BLOCK_WORDS / 2 ? frames - done : BLOCK_WORDS / 2;
        yamaha_fm_synth_render(dst + done * 2, n * 2);
    }
}

/* ---- song mode: wrap the synth callbacks to observe what the sequencer does ---- */

static bool g_held[FM_MIDI_CHANNELS][128];
static long g_steals, g_note_ons;

static void w_note_on(uint8_t c, uint8_t n, uint8_t v)
{
    int busy = 0;
    for (int i = 0; i < FM_MAX_VOICES; i++) busy += s_voices[i].active ? 1 : 0;
    if (busy >= FM_MAX_VOICES) g_steals++;
    g_note_ons++;
    g_held[c][n] = true;
    yamaha_fm_note_on(c, n, v);
}

static void w_note_off(uint8_t c, uint8_t n, uint8_t v)
{
    g_held[c][n] = false;
    yamaha_fm_note_off(c, n, v);
}

/* A voice that thinks its key is down while the sequencer says the note ended = a lost note-off. */
static int count_hung(void)
{
    int hung = 0;
    for (int i = 0; i < FM_MAX_VOICES; i++) {
        const fm_voice_t *v = &s_voices[i];
        if (v->active && !v->is_drum && v->key_down && !g_held[v->channel][v->note]) {
            hung++;
        }
    }
    return hung;
}

static int run_song(const char *path, double seconds, const char *out, int volume)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    static uint8_t midi[1 << 20];
    size_t len = fread(midi, 1, sizeof(midi), f);
    fclose(f);

    midi_synth_callbacks_t cb = {
        .note_on = w_note_on, .note_off = w_note_off,
        .program_change = yamaha_fm_program_change, .control_change = yamaha_fm_control_change,
        .pitch_bend = yamaha_fm_pitch_bend, .all_notes_off = yamaha_fm_all_notes_off,
        .song_clock = yamaha_fm_song_time_anchor, .event_time = yamaha_fm_song_time_event,
    };
    yamaha_fm_synth_init(RATE);
    midi_karaoke_init(&cb);
    if (!midi_karaoke_load_memory(midi, (uint32_t)len)) { fprintf(stderr, "bad MIDI\n"); return 1; }
    midi_karaoke_play();

    size_t frames = (size_t)(seconds * RATE);
    int16_t *pcm = calloc(frames * 2, sizeof(int16_t));
    uint64_t done = 0, last_us = 0;
    int max_voices = 0, max_hung = 0;
    long over_limit_blocks = 0;

    while (done + BLOCK_WORDS / 2 <= frames) {
        uint64_t us = (done + BLOCK_WORDS / 2) * 1000000ULL / RATE;
        midi_karaoke_tick((uint32_t)(us - last_us));
        last_us = us;
        yamaha_fm_synth_render(pcm + done * 2, BLOCK_WORDS);
        done += BLOCK_WORDS / 2;

        int busy = 0;
        for (int i = 0; i < FM_MAX_VOICES; i++) busy += s_voices[i].active ? 1 : 0;
        if (busy > max_voices) max_voices = busy;
        if (busy > 24) over_limit_blocks++;
        if (done % RATE < BLOCK_WORDS / 2) {
            int h = count_hung();
            if (h > max_hung) max_hung = h;
        }
    }

    report(pcm, frames, "synth output");
    for (size_t i = 0; i < frames * 2; i++) pcm[i] = (int16_t)((int)pcm[i] * volume / 100);
    report(pcm, frames, "after volume  ");
    printf("note_ons=%ld voice_steals=%ld max_voices_in_use=%d blocks_with_>24_voices=%ld max_hung_notes=%d\n",
           g_note_ons, g_steals, max_voices, over_limit_blocks, max_hung);
    printf("synth counters: steals=%u audible_steals=%u dropped_events=%u\n",
           (unsigned)yamaha_fm_get_steal_count(), (unsigned)yamaha_fm_get_audible_steal_count(),
           (unsigned)yamaha_fm_get_dropped_events());
    write_wav(out, pcm, frames);
    free(pcm);
    return 0;
}

static int run_note(int program, int note, int vel, double seconds, const char *out, int channel)
{
    yamaha_fm_synth_init(RATE);
    yamaha_fm_program_change((uint8_t)channel, (uint8_t)program);
    size_t frames = (size_t)(seconds * RATE);
    int16_t *pcm = calloc(frames * 2, sizeof(int16_t));
    size_t hold = frames * 2 / 3;
    yamaha_fm_note_on((uint8_t)channel, (uint8_t)note, (uint8_t)vel);
    render_frames(pcm, hold);
    yamaha_fm_note_off((uint8_t)channel, (uint8_t)note, 0);
    render_frames(pcm + hold * 2, frames - hold);
    report(pcm, frames, "note");
    write_wav(out, pcm, frames);
    free(pcm);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 7 && strcmp(argv[1], "note") == 0) {
        return run_note(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atof(argv[5]), argv[6],
                        argc >= 8 ? atoi(argv[7]) : 0);
    }
    if (argc >= 5 && strcmp(argv[1], "song") == 0) {
        return run_song(argv[2], atof(argv[3]), argv[4], argc >= 6 ? atoi(argv[5]) : 80);
    }
    fprintf(stderr, "usage: bench note <program> <note> <vel> <seconds> <out.wav> [channel]\n"
                    "       bench song <file.mid> <seconds> <out.wav> [volume]\n");
    return 2;
}
