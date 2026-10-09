/**
 * @file jitter.c
 * @brief Measures how far the audible start of each drum hit strays from its place in the
 *        song when the sequencer thread wakes up late, the way it does on the board while
 *        an SD read or another thread holds the CPU.
 *
 * A MIDI file with one click every 24 ticks is played through the real sequencer and synth.
 * The sequencer is woken on a 10 ms grid plus a random scheduling delay; the audio is rendered
 * in the 128-frame pieces the audio thread asks for. The first loud sample of every click is
 * compared with where the click belongs; a constant latency does not matter, only the
 * variation around it.
 *
 *   jitter [seed]            build with -DNEW_ENGINE for the timestamped engine
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zephyr/kernel.h"
#include "zephyr/sys/printk.h"
#ifdef NEW_ENGINE
#include "../../src/synth_dsp.c"
#endif
#include "../../src/yamaha_fm_synth.c"
#include "../../src/midi_karaoke_parser.c"

#define RATE        44100
#define PPQN        96
#define STEP_TICKS  24
#define CLICKS      180
#define BLOCK       128
#define TICK_US     5208U           /* 500000 / 96, as the sequencer computes it */

static uint8_t g_smf[8192];
static uint32_t g_len;

static void put(uint8_t b) { g_smf[g_len++] = b; }

static void put_vlq(uint32_t v)
{
    uint8_t t[4];
    int n = 0;
    t[n++] = (uint8_t)(v & 0x7FU);
    while ((v >>= 7) != 0U) {
        t[n++] = (uint8_t)((v & 0x7FU) | 0x80U);
    }
    while (n > 0) {
        put(t[--n]);
    }
}

static void build_midi(void)
{
    memcpy(g_smf, "MThd\0\0\0\6\0\0\0\1", 12);
    g_len = 12;
    put(0); put(PPQN);
    memcpy(g_smf + g_len, "MTrk\0\0\0\0", 8);
    g_len += 8;
    uint32_t start = g_len;
    for (int i = 0; i < CLICKS; i++) {
        put_vlq(i == 0 ? 0U : STEP_TICKS);
        put(0x99); put(37); put(110);           /* side stick: a short sharp click */
    }
    put(0); put(0xFF); put(0x2F); put(0);
    uint32_t n = g_len - start;
    g_smf[start - 4] = (uint8_t)(n >> 24); g_smf[start - 3] = (uint8_t)(n >> 16);
    g_smf[start - 2] = (uint8_t)(n >> 8);  g_smf[start - 1] = (uint8_t)n;
}

static uint32_t g_rng = 1;
static double frand(void)
{
    g_rng = g_rng * 1664525U + 1013904223U;
    return (double)(g_rng >> 8) / 16777216.0;
}

int main(int argc, char **argv)
{
    uint32_t seed = (argc > 1) ? (uint32_t)atoi(argv[1]) : 1U;
    g_rng = seed * 2654435761U + 12345U;

    midi_synth_callbacks_t cb = {
        .note_on = yamaha_fm_note_on, .note_off = yamaha_fm_note_off,
        .program_change = yamaha_fm_program_change, .control_change = yamaha_fm_control_change,
        .pitch_bend = yamaha_fm_pitch_bend, .all_notes_off = yamaha_fm_all_notes_off,
#ifdef NEW_ENGINE
        .song_clock = yamaha_fm_song_time_anchor, .event_time = yamaha_fm_song_time_event,
#endif
    };

    yamaha_fm_synth_init(RATE);
#ifdef NEW_ENGINE
    yamaha_fm_set_effects_level(0);
#endif
    midi_karaoke_init(&cb);
    build_midi();
    if (!midi_karaoke_load_memory(g_smf, g_len)) {
        fprintf(stderr, "bad midi\n");
        return 1;
    }

    const double song_s = (double)CLICKS * STEP_TICKS * TICK_US * 1e-6 + 0.5;
    const size_t frames = (size_t)(song_s * RATE);
    int16_t *pcm = calloc(frames * 2, sizeof(int16_t));

    /* Sequencer wake-up times: a 10 ms grid, plus the delay a busy CPU adds. */
    double wake_prev = 0.0;
    double grid = 0.010;
    double next_wake = grid;
    uint64_t last_us = 0;
    bool started = false;

    for (size_t at = 0; at + BLOCK <= frames; at += BLOCK) {
        double now = (double)at / RATE;

        while (next_wake <= now) {
            if (!started) {
                midi_karaoke_play();
                started = true;
                last_us = (uint64_t)(next_wake * 1e6);
            } else {
                uint64_t us = (uint64_t)(next_wake * 1e6);
                midi_karaoke_tick((uint32_t)(us - last_us));
                last_us = us;
            }
            wake_prev = next_wake;
            grid += 0.010;
            double delay = frand() * 0.0012;                   /* ordinary scheduling latency */
            double r = frand();
            if (r < 0.06) {
                delay += 0.003 + frand() * 0.005;              /* an SD read ahead of us */
            } else if (r < 0.065) {
                delay += 0.015 + frand() * 0.010;              /* a long stall */
            }
            next_wake = grid + delay;
            if (next_wake < wake_prev + 0.0002) {
                next_wake = wake_prev + 0.0002;
            }
        }
        yamaha_fm_synth_render(pcm + at * 2, BLOCK * 2);
    }

    /* Find the first loud sample of each click: the first sample over the threshold after a gap
     * in which nothing was over it. The threshold is a fifth of the loudest click. */
    int peak = 0;
    for (size_t i = 0; i < frames; i++) {
        int a = abs(pcm[i * 2]);
        if (a > peak) peak = a;
    }
    const int thresh = peak / 5;
    double dev_ms[CLICKS];
    int found = 0;
    size_t gap = 100000;
    double sum = 0.0;
    for (size_t i = 0; i < frames && found < CLICKS; i++) {
        if (abs(pcm[i * 2]) < thresh) {
            if (gap < 100000) gap++;
        } else {
            if (gap >= 1500) {
                double ideal = (double)found * STEP_TICKS * TICK_US * 1e-6 * RATE;
                dev_ms[found] = ((double)i - ideal) * 1000.0 / RATE;
                sum += dev_ms[found];
                found++;
            }
            gap = 0;
        }
    }
    if (found < CLICKS * 9 / 10) {
        fprintf(stderr, "only %d of %d clicks found\n", found, CLICKS);
        return 1;
    }

    double mean = sum / found, var = 0.0, worst = 0.0;
    for (int i = 0; i < found; i++) {
        double d = dev_ms[i] - mean;
        var += d * d;
        if (fabs(d) > worst) worst = fabs(d);
    }
    /* Sort a copy of |d| for the 95th percentile. */
    double ad[CLICKS];
    for (int i = 0; i < found; i++) ad[i] = fabs(dev_ms[i] - mean);
    for (int i = 1; i < found; i++) {
        double v = ad[i];
        int j = i - 1;
        while (j >= 0 && ad[j] > v) { ad[j + 1] = ad[j]; j--; }
        ad[j + 1] = v;
    }
    printf("%s seed %u: clicks %d, mean latency %.2f ms, jitter rms %.3f ms, p95 %.3f ms, worst %.3f ms\n",
#ifdef NEW_ENGINE
           "new", seed,
#else
           "old", seed,
#endif
           found, mean, sqrt(var / found), ad[(found * 95) / 100], worst);
    free(pcm);
    return 0;
}
