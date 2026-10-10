/**
 * @file yamaha_fm_synth.c
 * @brief 30-voice two-operator FM synthesizer with General MIDI program mapping and a
 *        synthesized percussion kit, for RT-Spark (STM32F407) and the ES8388 3.5mm output.
 *
 * Threading: the sequencer calls the note/controller functions from a timer interrupt, so
 * they never touch voice state. Note events go through a small ring and are applied by
 * yamaha_fm_synth_render() on the audio thread, the only code that modifies voices.
 * Channel controllers are single small writes, safe from any context.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include "yamaha_fm_synth.h"
#include "dtcm.h"
#include "synth_dsp.h"
#include "opl4_drum_samples.h"
#include "opl3_wave_tables.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/irq.h>

#define SUB_FRAMES          DSP_BLOCK_FRAMES /* envelope / pitch / gain update interval, and the
                                               * granularity at which events and effects run */
#define PHASE_PER_RAD       683565275.6f    /* 2^32 / (2 pi) */
#define MOD_IDX_UNIT        20860.76f       /* phase per radian per unit of a Q15 modulator */
#ifndef SYNTH_MASTER_Q8
#define SYNTH_MASTER_Q8     256             /* master gain in 1/256 steps, applied before the limiter */
#endif
#ifndef SYNTH_DRUM_Q8
#define SYNTH_DRUM_Q8       166             /* drum level against the melodic voices, in 1/256 steps */
#endif

static volatile uint16_t s_synth_master_q8 = SYNTH_MASTER_Q8;
static volatile uint16_t s_synth_drum_q8   = SYNTH_DRUM_Q8;
static volatile uint8_t s_instrument_percent = 100;
static volatile uint8_t s_drum_percent = 100;
static volatile uint8_t s_melody_percent = 100;
#ifndef THIRD_OP_DEFAULT
#define THIRD_OP_DEFAULT true
#endif
static volatile bool s_third_op = THIRD_OP_DEFAULT;
static volatile bool s_opl4_drums = true;
static volatile bool s_vcf_on = true;
static uint32_t s_ext_started;
static uint32_t s_ext_skipped;
static volatile int8_t s_melody_channel = -1;

void yamaha_fm_set_instrument_gain(uint8_t percent)
{
    if (percent < 20) percent = 20;
    if (percent > 200) percent = 200;
    s_instrument_percent = percent;
    s_synth_master_q8 = (uint16_t)(((uint32_t)percent * 256U) / 100U);
}

uint8_t yamaha_fm_get_instrument_gain(void)
{
    return s_instrument_percent;
}

void yamaha_fm_set_drum_gain(uint8_t percent)
{
    if (percent < 20) percent = 20;
    if (percent > 200) percent = 200;
    s_drum_percent = percent;
    s_synth_drum_q8 = (uint16_t)(((uint32_t)percent * 166U) / 100U);
}

uint8_t yamaha_fm_get_drum_gain(void)
{
    return s_drum_percent;
}

void yamaha_fm_set_melody_gain(uint8_t percent)
{
    if (percent < 20) percent = 20;
    if (percent > 250) percent = 250;
    s_melody_percent = percent;
}

uint8_t yamaha_fm_get_melody_gain(void)
{
    return s_melody_percent;
}

void yamaha_fm_set_melody_channel(int8_t channel)
{
    s_melody_channel = (channel >= 0 && channel < FM_MIDI_CHANNELS && channel != FM_DRUM_CHANNEL) ? channel : -1;
}
#define GAIN_Q              8               /* extra fractional bits in the per-sample gain ramp */
#define GAIN_ONE            (32767 << GAIN_Q)
#define SILENCE             0.0015f         /* about -56 dB */
#define VIBRATO_HZ          5.5f
#define SEMITONE_LINEAR     0.05776f        /* 2^(1/12) - 1, small-signal pitch change per semitone */
#define EVENT_RING_SIZE     512U            /* power of two: the counters below run freely */
#define EVENT_RING_MASK     (EVENT_RING_SIZE - 1U)
#define EVENT_LEAD_MS       MIDI_EVENT_LEAD_MS   /* events reach the synth this long before they sound */
#define EVENT_ROUND_FRAMES  (SUB_FRAMES / 2U)
#define MAX_BANDWIDTH_HZ    11000.0f
#ifndef THIRD_OP_MAX_VOICES
#define THIRD_OP_MAX_VOICES 22          /* 4-op voices active up to 22 voices */
#ifndef SIX_OP_MAX_VOICES
#define SIX_OP_MAX_VOICES   12          /* 6-op DX7 voices active up to 12 voices */
#endif
#endif
/* Each channel hears both carriers, one louder than the other, with the loud one swapped between
 * the sides. The two weights satisfy p^2 + q^2 = 1 so that a channel keeps the average power of
 * a plain two-operator voice; the beating between the carriers is the chorus effect, and
 * keeping q well under p keeps its nulls from reaching silence. */
#define ENSEMBLE_P_Q10      962         /* 0.94 (4-op primary) */
#define ENSEMBLE_Q_Q10      350         /* 0.34 (4-op secondary) */
#define ENSEMBLE6_P_Q10     940         /* 0.92 (6-op left/right primary) */
#define ENSEMBLE6_Q_Q10     340         /* 0.33 (6-op opposite bleed) */
#define ENSEMBLE6_C_Q10     280         /* 0.27 (6-op octave harmonic pair: orthogonal, never phase-cancels) */

typedef enum {
    ST_IDLE = 0,
    ST_ATTACK,
    ST_DECAY,
    ST_SUSTAIN,
    ST_RELEASE
} env_state_t;

typedef struct {
    float ratio_c, ratio_m;         /* carrier / modulator frequency multiples */
    float idx_start, idx_sus;       /* modulation index in radians, or op2 mix level if additive */
    float idx_tau;                  /* seconds for the index to move toward idx_sus */
    float fb;                       /* modulator self feedback, radians */
    float attack;                   /* seconds to full level */
    float dec_tau;                  /* decay time constant toward the sustain level, seconds */
    float sustain;                  /* sustain level 0..1; 0 means the note dies away while held */
    float release;                  /* release time constant, seconds */
    float level;                    /* loudness trim */
    float vib;                      /* built-in vibrato depth, semitones */
    bool additive;                  /* op2 is mixed into the output instead of modulating op1 */
} fm_patch_t;

typedef struct {
    float f0, f1, f_tau;            /* tone pitch sweep in Hz and its time constant */
    float tone_lvl, tone_tau;
    float noise_lvl, noise_tau;
    bool hp;                        /* high-pass the noise */
    bool open_hat;
} drum_recipe_t;

typedef struct {
    bool active;
    bool is_drum;
    bool key_down;
    bool pedal_held;
    bool open_hat;
    uint8_t channel;
    uint8_t note;
    env_state_t state;
    uint32_t age;
    float amp;                      /* envelope level 0..1 (drums: loudest component) */
    float vel_gain;
    float level;

    /* FM voice */
    bool additive;
    uint32_t pc, pm;
    float base_c, base_m;           /* phase increments at pitch ratio 1 */
    int32_t fb_prev;
    uint32_t fb_u;
    float idx, idx_sus, idx_k;
    float atk_step, dec_k, sus, rel_k;
    float vib;
    int32_t gl, gr;                 /* per-sample gain, Q15 << GAIN_Q */

    /* 3rd..6th operators: 6-op DX7 3-pair or 4-op OPL3 2-pair */
    uint8_t ext;
    uint8_t ops;                    /* 2, 4, or 6 active operators for this voice */
    uint8_t wave_m;                 /* OPL3 modulator waveform (0..7) */
    uint32_t pc2, pm2;              /* Pair 2: Op3 carrier & Op4 modulator phases */
    uint32_t pc3, pm3;              /* Pair 3: Op5 carrier & Op6 modulator phases */
    float base_x, base_m2;          /* Pair 2: Op3 and Op4 phase increments */
    float base_c3, base_m3;         /* Pair 3: Op5 and Op6 phase increments */
    float tine_idx, tine_k;         /* Hammer/tine modulation index and decay */

    /* Dynamic Resonant Low-Pass Filter (VCF) */
    int32_t svf_lp, svf_bp;
    int32_t svf_q;

    /* percussion & OPL4 PCM WaveTable voice */
    const int16_t *pcm_data;
    uint32_t pcm_len;
    uint32_t pcm_pos;               /* 16.16 fixed-point sample cursor */
    uint32_t pcm_step;              /* 16.16 fixed-point pitch step */
    float pcm_amp, pcm_k;
    float tone_f, tone_f1, tone_fk;
    float tone_amp, tone_k, tone_lvl;
    float noise_amp, noise_k, noise_lvl;
    bool hp;
    int32_t noise_prev;
    uint32_t rng;
} fm_voice_t;

typedef struct {
    uint8_t program, volume, expression, pan, mod, reverb, chorus;
    uint8_t rpn_msb, rpn_lsb, bend_semis, bend_cents;
    int16_t bend;
    bool pedal;
    float gain_prev;                /* render-thread smoothing state */
    uint8_t pan_cached;
    float pan_l, pan_r;
} midi_channel_state_t;

typedef struct {
    uint32_t frame;                 /* audio frame at which the event takes effect */
    uint8_t type, ch, a, b;
} synth_event_t;

enum { EV_NOTE_ON = 1, EV_NOTE_OFF, EV_CC, EV_PROGRAM, EV_BEND, EV_ALL_OFF };

static int16_t s_waves[NUM_OPL3_WAVES][SINE_SIZE];
#define s_sine s_waves[0]
static DTCM_BSS float s_note_inc[128];
static float s_sample_rate = 44100.0f;
static float s_sub_sec;
static float s_fast_rel_k;
static float s_lfo_phase;
static uint32_t s_age_counter;
static uint32_t s_steals;
static uint32_t s_steals_audible;
static volatile uint32_t s_dropped_events;

static DTCM_BSS fm_voice_t s_voices[FM_MAX_VOICES];
static DTCM_BSS midi_channel_state_t s_channels[FM_MIDI_CHANNELS];
static DTCM_BSS int32_t s_acc_l[SUB_FRAMES];
static DTCM_BSS int32_t s_acc_r[SUB_FRAMES];
static DTCM_BSS int32_t s_rev[SUB_FRAMES];
static DTCM_BSS int32_t s_cho[SUB_FRAMES];
static DTCM_BSS int32_t s_bus_l[FM_MIDI_CHANNELS][SUB_FRAMES];   /* one dry bus per MIDI channel */
static DTCM_BSS int32_t s_bus_r[FM_MIDI_CHANNELS][SUB_FRAMES];

static DTCM_BSS synth_event_t s_events[EVENT_RING_SIZE];
static volatile uint32_t s_ev_head;     /* next slot to write; counts events ever pushed */
static volatile uint32_t s_ev_tail;     /* next slot to read */
static volatile uint32_t s_flush_to;    /* ring position the last yamaha_fm_synth_reset() saw */
static volatile uint32_t s_frame_clock;         /* frames rendered since init */
static volatile uint32_t s_stamp;               /* frame for the next event the sequencer pushes */
static volatile bool s_stamp_valid;
static volatile uint32_t s_flush_request;
static uint32_t s_flush_seen;
static uint32_t s_rate_u = 44100U;
static uint32_t s_lead_frames;
static uint32_t s_late_events;
static uint32_t s_ring_peak;
static int32_t s_min_margin = INT32_MAX;

/* Song time -> audio frame map: song time s_map_us sounds at frame s_map_frame. */
static bool s_anchor_valid;
static uint32_t s_map_us;
static uint32_t s_map_frame;

#define LAST_CC_SLOTS   6
static uint8_t s_last_cc[FM_MIDI_CHANNELS][LAST_CC_SLOTS];
static uint16_t s_last_bend[FM_MIDI_CHANNELS];

/* -------------------------------------------------------------------------- */
/* Instrument patches                                                         */
/* -------------------------------------------------------------------------- */
enum {
    P_PIANO, P_BRIGHT, P_EPIANO, P_HONKY, P_CLAV, P_BELL, P_VIBES, P_MARIMBA,
    P_ORGAN, P_PERC_ORGAN, P_REED_ORGAN, P_ACCORD,
    P_NYLON, P_STEEL, P_JAZZ_GTR, P_CLEAN_GTR, P_MUTED_GTR, P_OD_GTR, P_DIST_GTR,
    P_ACO_BASS, P_FINGER_BASS, P_PICK_BASS, P_FRETLESS, P_SLAP, P_SYN_BASS1, P_SYN_BASS2,
    P_VIOLIN, P_CELLO, P_STRINGS, P_PIZZ, P_HARP, P_TIMPANI, P_CHOIR,
    P_TRUMPET, P_TROMBONE, P_TUBA, P_HORN, P_BRASS_SEC, P_SYN_BRASS,
    P_SAX, P_OBOE, P_CLARINET, P_FLUTE, P_WHISTLE,
    P_SQ_LEAD, P_SAW_LEAD, P_CALLIOPE, P_PAD,
    P_SITAR, P_PLUCK, P_STEELDRUM, P_SFX, P_BAGPIPE,
    P_COUNT
};

/* rc, rm, idx_start, idx_sus, idx_tau, fb, attack, dec_tau, sustain, release, level, vib, additive */
static const fm_patch_t PATCHES[P_COUNT] = {
    [P_PIANO]       = {1, 1,     3.2f, 0.7f, 0.12f, 0.0f, 0.002f, 1.6f, 0.00f, 0.18f, 0.40f, 0.00f, false},
    [P_BRIGHT]      = {1, 1,     4.2f, 1.0f, 0.10f, 0.0f, 0.002f, 1.4f, 0.00f, 0.16f, 0.38f, 0.00f, false},
    [P_EPIANO]      = {1, 14,    1.3f, 0.12f, 0.22f, 0.0f, 0.002f, 2.2f, 0.00f, 0.30f, 0.34f, 0.00f, false},
    [P_HONKY]       = {1, 1.012f, 2.6f, 0.9f, 0.10f, 0.0f, 0.002f, 1.3f, 0.00f, 0.15f, 0.40f, 0.00f, false},
    [P_CLAV]        = {1, 3,     3.5f, 1.2f, 0.05f, 0.0f, 0.001f, 0.5f, 0.00f, 0.06f, 0.38f, 0.00f, false},
    [P_BELL]        = {1, 3.5f,  2.4f, 0.2f, 0.70f, 0.0f, 0.001f, 2.6f, 0.00f, 0.35f, 0.34f, 0.00f, false},
    [P_VIBES]       = {1, 4,     0.9f, 0.1f, 0.80f, 0.0f, 0.002f, 2.4f, 0.00f, 0.30f, 0.38f, 0.00f, false},
    [P_MARIMBA]     = {1, 4,     1.4f, 0.05f, 0.06f, 0.0f, 0.001f, 0.5f, 0.00f, 0.10f, 0.45f, 0.00f, false},
    [P_ORGAN]       = {1, 2,     0.9f, 0.9f, 1.00f, 0.5f, 0.004f, 0.1f, 1.00f, 0.04f, 0.26f, 0.00f, true},
    [P_PERC_ORGAN]  = {1, 3,     1.1f, 0.35f, 0.12f, 0.4f, 0.002f, 0.1f, 1.00f, 0.05f, 0.26f, 0.00f, true},
    [P_REED_ORGAN]  = {1, 1,     1.6f, 1.4f, 0.10f, 0.8f, 0.020f, 0.1f, 1.00f, 0.06f, 0.30f, 0.03f, false},
    [P_ACCORD]      = {1, 1,     1.8f, 1.3f, 0.15f, 0.6f, 0.040f, 0.1f, 1.00f, 0.08f, 0.30f, 0.08f, false},
    [P_NYLON]       = {1, 2,     2.3f, 0.35f, 0.07f, 0.1f, 0.002f, 0.9f, 0.00f, 0.12f, 0.42f, 0.00f, false},
    [P_STEEL]       = {1, 3,     3.0f, 0.7f, 0.09f, 0.15f, 0.001f, 1.1f, 0.00f, 0.12f, 0.40f, 0.00f, false},
    [P_JAZZ_GTR]    = {1, 1,     1.5f, 0.5f, 0.20f, 0.1f, 0.003f, 2.0f, 0.22f, 0.15f, 0.40f, 0.00f, false},
    [P_CLEAN_GTR]   = {1, 2,     1.9f, 0.6f, 0.15f, 0.1f, 0.002f, 1.8f, 0.15f, 0.12f, 0.40f, 0.00f, false},
    [P_MUTED_GTR]   = {1, 2,     2.6f, 0.4f, 0.04f, 0.2f, 0.001f, 0.22f, 0.00f, 0.05f, 0.42f, 0.00f, false},
    [P_OD_GTR]      = {1, 1,     3.2f, 2.8f, 0.20f, 1.3f, 0.004f, 0.3f, 0.75f, 0.08f, 0.30f, 0.00f, false},
    [P_DIST_GTR]    = {1, 1,     4.4f, 3.8f, 0.20f, 1.7f, 0.004f, 0.3f, 0.75f, 0.08f, 0.28f, 0.00f, false},
    [P_ACO_BASS]    = {1, 1,     1.7f, 0.4f, 0.10f, 0.1f, 0.003f, 1.0f, 0.30f, 0.10f, 0.50f, 0.00f, false},
    [P_FINGER_BASS] = {1, 1,     2.1f, 0.55f, 0.12f, 0.15f, 0.003f, 1.4f, 0.45f, 0.10f, 0.50f, 0.00f, false},
    [P_PICK_BASS]   = {1, 1,     3.0f, 0.8f, 0.06f, 0.2f, 0.002f, 0.9f, 0.35f, 0.08f, 0.50f, 0.00f, false},
    [P_FRETLESS]    = {1, 1,     1.4f, 0.8f, 0.15f, 0.1f, 0.010f, 1.5f, 0.65f, 0.12f, 0.50f, 0.03f, false},
    [P_SLAP]        = {1, 1,     3.6f, 0.6f, 0.03f, 0.2f, 0.001f, 0.45f, 0.25f, 0.06f, 0.48f, 0.00f, false},
    [P_SYN_BASS1]   = {1, 1,     3.0f, 2.2f, 0.25f, 1.0f, 0.002f, 0.3f, 0.80f, 0.06f, 0.40f, 0.00f, false},
    [P_SYN_BASS2]   = {1, 2,     3.6f, 1.5f, 0.15f, 0.6f, 0.002f, 0.3f, 0.70f, 0.06f, 0.40f, 0.00f, false},
    [P_VIOLIN]      = {1, 1,     0.8f, 1.2f, 0.20f, 0.5f, 0.070f, 0.1f, 1.00f, 0.15f, 0.30f, 0.10f, false},
    [P_CELLO]       = {1, 1,     0.7f, 1.0f, 0.25f, 0.45f, 0.090f, 0.1f, 1.00f, 0.18f, 0.34f, 0.07f, false},
    [P_STRINGS]     = {1, 1,     0.9f, 1.3f, 0.35f, 0.55f, 0.180f, 0.1f, 1.00f, 0.30f, 0.28f, 0.07f, false},
    [P_PIZZ]        = {1, 2,     2.0f, 0.3f, 0.05f, 0.1f, 0.001f, 0.3f, 0.00f, 0.06f, 0.45f, 0.00f, false},
    [P_HARP]        = {1, 2,     2.2f, 0.4f, 0.15f, 0.1f, 0.001f, 1.4f, 0.00f, 0.20f, 0.40f, 0.00f, false},
    [P_TIMPANI]     = {1, 1.5f,  3.0f, 0.2f, 0.05f, 0.2f, 0.001f, 0.9f, 0.00f, 0.15f, 0.45f, 0.00f, false},
    [P_CHOIR]       = {1, 1,     0.9f, 0.8f, 0.30f, 0.2f, 0.120f, 0.1f, 1.00f, 0.25f, 0.30f, 0.09f, false},
    [P_TRUMPET]     = {1, 1,     0.8f, 2.5f, 0.08f, 0.2f, 0.035f, 0.1f, 1.00f, 0.07f, 0.30f, 0.03f, false},
    [P_TROMBONE]    = {1, 1,     0.5f, 2.0f, 0.12f, 0.2f, 0.050f, 0.1f, 1.00f, 0.09f, 0.32f, 0.02f, false},
    [P_TUBA]        = {1, 1,     0.6f, 1.6f, 0.12f, 0.2f, 0.060f, 0.1f, 1.00f, 0.10f, 0.36f, 0.00f, false},
    [P_HORN]        = {1, 1,     0.4f, 1.5f, 0.15f, 0.2f, 0.080f, 0.1f, 1.00f, 0.12f, 0.32f, 0.02f, false},
    [P_BRASS_SEC]   = {1, 1,     1.0f, 2.9f, 0.07f, 0.3f, 0.030f, 0.1f, 1.00f, 0.08f, 0.28f, 0.02f, false},
    [P_SYN_BRASS]   = {1, 1,     1.2f, 3.0f, 0.10f, 0.8f, 0.020f, 0.1f, 1.00f, 0.10f, 0.28f, 0.03f, false},
    [P_SAX]         = {1, 1,     1.2f, 2.1f, 0.10f, 0.9f, 0.030f, 0.1f, 1.00f, 0.07f, 0.32f, 0.05f, false},
    [P_OBOE]        = {1, 1,     1.6f, 2.4f, 0.10f, 0.5f, 0.040f, 0.1f, 1.00f, 0.07f, 0.34f, 0.05f, false},
    [P_CLARINET]    = {1, 2,     0.9f, 1.3f, 0.10f, 0.1f, 0.040f, 0.1f, 1.00f, 0.07f, 0.34f, 0.03f, false},
    [P_FLUTE]       = {1, 1,     0.35f, 0.45f, 0.15f, 0.05f, 0.060f, 0.1f, 1.00f, 0.10f, 0.36f, 0.09f, false},
    [P_WHISTLE]     = {1, 1,     0.1f, 0.1f, 0.10f, 0.0f, 0.050f, 0.1f, 1.00f, 0.08f, 0.34f, 0.08f, false},
    [P_SQ_LEAD]     = {1, 2,     1.8f, 1.6f, 0.20f, 0.2f, 0.004f, 0.1f, 1.00f, 0.06f, 0.26f, 0.00f, false},
    [P_SAW_LEAD]    = {1, 1,     3.4f, 3.0f, 0.20f, 1.4f, 0.004f, 0.1f, 1.00f, 0.06f, 0.26f, 0.00f, false},
    [P_CALLIOPE]    = {1, 1,     1.4f, 1.2f, 0.15f, 0.5f, 0.020f, 0.1f, 1.00f, 0.05f, 0.30f, 0.03f, false},
    [P_PAD]         = {1, 1,     1.0f, 1.4f, 0.50f, 0.4f, 0.300f, 0.1f, 1.00f, 0.50f, 0.26f, 0.05f, false},
    [P_SITAR]       = {1, 3.1f,  2.8f, 1.0f, 0.10f, 0.5f, 0.001f, 1.0f, 0.00f, 0.12f, 0.38f, 0.00f, false},
    [P_PLUCK]       = {1, 3,     3.2f, 0.8f, 0.05f, 0.1f, 0.001f, 0.5f, 0.00f, 0.08f, 0.42f, 0.00f, false},
    [P_STEELDRUM]   = {1, 2,     1.8f, 0.2f, 0.30f, 0.0f, 0.001f, 0.7f, 0.00f, 0.20f, 0.40f, 0.00f, false},
    [P_SFX]         = {1, 1.5f,  1.0f, 0.5f, 0.30f, 0.3f, 0.010f, 0.3f, 0.00f, 0.20f, 0.15f, 0.00f, false},
    [P_BAGPIPE]     = {1, 1,     2.0f, 1.8f, 0.10f, 0.9f, 0.020f, 0.1f, 1.00f, 0.06f, 0.28f, 0.00f, false},
};

enum { EXT_NONE = 0, EXT_ENSEMBLE, EXT_TINE };

typedef struct {
    uint8_t kind;
    float a, b, c;      /* ensemble: detune in cents; tine: frequency ratio, index, decay time constant */
} ext_patch_t;

/* Which patches get a third operator. Everything else, and every patch while the player is busy,
 * stays two-operator. */
static const ext_patch_t EXT_PATCHES[P_COUNT] = {
    /* Acoustic & Electric Keys (4-op hammer/tine + sympathetic dual-carrier body) */
    [P_PIANO]       = {EXT_TINE, 4.01f, 1.6f, 0.030f},
    [P_BRIGHT]      = {EXT_TINE, 4.01f, 2.0f, 0.030f},
    [P_HONKY]       = {EXT_TINE, 3.01f, 1.5f, 0.035f},
    [P_EPIANO]      = {EXT_TINE, 7.00f, 1.4f, 0.040f},
    [P_CLAV]        = {EXT_TINE, 3.00f, 1.8f, 0.025f},

    /* Guitars (4-op pick transient & body) */
    [P_NYLON]       = {EXT_TINE, 3.01f, 1.2f, 0.025f},
    [P_STEEL]       = {EXT_TINE, 4.01f, 1.4f, 0.025f},
    [P_CLEAN_GTR]   = {EXT_ENSEMBLE, 2.5f, 1.0f, 1.0f},

    /* Basses (4-op fat dual-oscillator sub & punch) */
    [P_ACO_BASS]    = {EXT_ENSEMBLE, 2.0f, 1.0f, 0.9f},
    [P_FINGER_BASS] = {EXT_ENSEMBLE, 2.5f, 1.0f, 1.0f},
    [P_PICK_BASS]   = {EXT_TINE, 3.00f, 1.3f, 0.025f},
    [P_SLAP]        = {EXT_TINE, 4.00f, 1.8f, 0.020f},
    [P_SYN_BASS1]   = {EXT_ENSEMBLE, 3.5f, 1.0f, 1.1f},
    [P_SYN_BASS2]   = {EXT_ENSEMBLE, 4.0f, 1.0f, 1.0f},

    /* Strings, Choirs & Pads (4-op stereo dual-pair ensemble) */
    [P_STRINGS]     = {EXT_ENSEMBLE, 4.0f, 1.0f, 1.0f},
    [P_VIOLIN]      = {EXT_ENSEMBLE, 3.0f, 1.0f, 1.0f},
    [P_CELLO]       = {EXT_ENSEMBLE, 3.0f, 1.0f, 1.0f},
    [P_CHOIR]       = {EXT_ENSEMBLE, 4.0f, 1.0f, 1.0f},
    [P_PAD]         = {EXT_ENSEMBLE, 5.0f, 1.0f, 1.05f},

    /* Brass & Synth Leads (4-op rich section & saw lead) */
    [P_TRUMPET]     = {EXT_ENSEMBLE, 3.0f, 1.0f, 1.0f},
    [P_TROMBONE]    = {EXT_ENSEMBLE, 2.5f, 1.0f, 1.0f},
    [P_HORN]        = {EXT_ENSEMBLE, 3.0f, 1.0f, 0.9f},
    [P_BRASS_SEC]   = {EXT_ENSEMBLE, 4.5f, 1.0f, 1.1f},
    [P_SYN_BRASS]   = {EXT_ENSEMBLE, 4.0f, 1.0f, 1.1f},
    [P_SAX]         = {EXT_ENSEMBLE, 3.0f, 1.0f, 0.95f},
    [P_SQ_LEAD]     = {EXT_ENSEMBLE, 4.0f, 1.0f, 1.0f},
    [P_SAW_LEAD]    = {EXT_ENSEMBLE, 5.0f, 1.0f, 1.1f},
};

enum {
    WAVE_SINE = 0,          /* 0: Standard Sine */
    WAVE_HALF = 1,          /* 1: Half-Sine (positive half only) */
    WAVE_ABS = 2,           /* 2: Full-Wave / Absolute-Sine (|sin(x)|) */
    WAVE_QUARTER = 3,       /* 3: Quarter-Sine / Pulse-Sine (quarters 1 & 3 only) */
    WAVE_ALT = 4,           /* 4: Alternating-Sine (sin(2x) in first half, 0 in second) */
    WAVE_CAMEL = 5,         /* 5: Camel-Sine (|sin(2x)| in first half, 0 in second) */
    WAVE_SQUARE = 6,        /* 6: Square Wave (+1 / -1) */
    WAVE_LOG_SAW = 7        /* 7: Logarithmic Sawtooth */
};

/* OPL3 / TX81Z 8-waveform assignment across General MIDI instrument families */
static const uint8_t PATCH_WAVE_M[P_COUNT] = {
    [P_CLARINET]    = WAVE_HALF,
    [P_REED_ORGAN]  = WAVE_HALF,
    [P_ACCORD]      = WAVE_HALF,
    [P_SYN_BASS1]   = WAVE_HALF,
    [P_OBOE]        = WAVE_ABS,
    [P_CLAV]        = WAVE_ABS,
    [P_JAZZ_GTR]    = WAVE_ABS,
    [P_PERC_ORGAN]  = WAVE_QUARTER,
    [P_MUTED_GTR]   = WAVE_QUARTER,
    [P_SLAP]        = WAVE_QUARTER,
    [P_SAX]         = WAVE_ALT,
    [P_BAGPIPE]     = WAVE_ALT,
    [P_SITAR]       = WAVE_ALT,
    [P_OD_GTR]      = WAVE_CAMEL,
    [P_FRETLESS]    = WAVE_CAMEL,
    [P_PLUCK]       = WAVE_CAMEL,
    [P_SQ_LEAD]     = WAVE_SQUARE,
    [P_CALLIOPE]    = WAVE_SQUARE,
    [P_SAW_LEAD]    = WAVE_LOG_SAW,
    [P_DIST_GTR]    = WAVE_LOG_SAW,
    [P_SYN_BASS2]   = WAVE_LOG_SAW,
};

/* OPL3 waveform table pointer lookup: wt[phase >> SINE_SHIFT] has ZERO branches in inner loop */

/* General MIDI program number -> patch. */
static const uint8_t PROGRAM_PATCH[128] = {
    /*   0 */ P_PIANO, P_BRIGHT, P_BRIGHT, P_HONKY, P_EPIANO, P_EPIANO, P_CLAV, P_CLAV,
    /*   8 */ P_BELL, P_BELL, P_BELL, P_VIBES, P_MARIMBA, P_MARIMBA, P_BELL, P_PLUCK,
    /*  16 */ P_ORGAN, P_PERC_ORGAN, P_ORGAN, P_ORGAN, P_REED_ORGAN, P_ACCORD, P_ACCORD, P_ACCORD,
    /*  24 */ P_NYLON, P_STEEL, P_JAZZ_GTR, P_CLEAN_GTR, P_MUTED_GTR, P_OD_GTR, P_DIST_GTR, P_NYLON,
    /*  32 */ P_ACO_BASS, P_FINGER_BASS, P_PICK_BASS, P_FRETLESS, P_SLAP, P_SLAP, P_SYN_BASS1, P_SYN_BASS2,
    /*  40 */ P_VIOLIN, P_VIOLIN, P_CELLO, P_CELLO, P_STRINGS, P_PIZZ, P_HARP, P_TIMPANI,
    /*  48 */ P_STRINGS, P_STRINGS, P_STRINGS, P_STRINGS, P_CHOIR, P_CHOIR, P_CHOIR, P_BRASS_SEC,
    /*  56 */ P_TRUMPET, P_TROMBONE, P_TUBA, P_TRUMPET, P_HORN, P_BRASS_SEC, P_SYN_BRASS, P_SYN_BRASS,
    /*  64 */ P_SAX, P_SAX, P_SAX, P_SAX, P_OBOE, P_OBOE, P_OBOE, P_CLARINET,
    /*  72 */ P_FLUTE, P_FLUTE, P_FLUTE, P_FLUTE, P_FLUTE, P_FLUTE, P_WHISTLE, P_WHISTLE,
    /*  80 */ P_SQ_LEAD, P_SAW_LEAD, P_CALLIOPE, P_CALLIOPE, P_SAW_LEAD, P_CHOIR, P_SAW_LEAD, P_SAW_LEAD,
    /*  88 */ P_PAD, P_PAD, P_PAD, P_PAD, P_PAD, P_PAD, P_PAD, P_PAD,
    /*  96 */ P_PAD, P_PAD, P_BELL, P_PAD, P_PAD, P_PAD, P_PAD, P_PAD,
    /* 104 */ P_SITAR, P_PLUCK, P_PLUCK, P_PLUCK, P_BELL, P_BAGPIPE, P_VIOLIN, P_OBOE,
    /* 112 */ P_BELL, P_BELL, P_STEELDRUM, P_MARIMBA, P_TIMPANI, P_TIMPANI, P_TIMPANI, P_SFX,
    /* 120 */ P_SFX, P_SFX, P_SFX, P_SFX, P_SFX, P_SFX, P_SFX, P_SFX,
};

static drum_recipe_t make_recipe(uint8_t note, uint8_t kit_prog)
{
    static const drum_recipe_t KICK_ACO = {135.0f, 43.0f, 0.035f, 0.95f, 0.13f, 0.08f, 0.008f, true,  false};
    static const drum_recipe_t KICK     = {165.0f, 48.0f, 0.028f, 0.95f, 0.11f, 0.10f, 0.008f, true,  false};
    static const drum_recipe_t STICK    = {1700.0f, 1500.0f, 0.02f, 0.45f, 0.025f, 0.25f, 0.02f, true, false};
    static const drum_recipe_t SNARE    = {220.0f, 165.0f, 0.030f, 0.50f, 0.10f, 0.42f, 0.16f, false, false};
    static const drum_recipe_t SNARE_EL = {270.0f, 180.0f, 0.025f, 0.55f, 0.08f, 0.46f, 0.18f, true,  false};
    static const drum_recipe_t CLAP     = {0.0f, 0.0f, 0.05f, 0.0f, 0.10f, 0.55f, 0.13f, false, false};
    static const drum_recipe_t HAT_C    = {4100.0f, 4100.0f, 0.05f, 0.08f, 0.035f, 0.40f, 0.035f, true, false};
    static const drum_recipe_t HAT_P    = {3400.0f, 3400.0f, 0.05f, 0.06f, 0.025f, 0.35f, 0.025f, true, false};
    static const drum_recipe_t HAT_O    = {3900.0f, 3900.0f, 0.05f, 0.10f, 0.30f, 0.40f, 0.30f, true,  true};
    static const drum_recipe_t CRASH1   = {3100.0f, 3100.0f, 0.05f, 0.15f, 0.85f, 0.42f, 0.85f, true,  false};
    static const drum_recipe_t CRASH2   = {3600.0f, 3600.0f, 0.05f, 0.16f, 0.72f, 0.42f, 0.72f, true,  false};
    static const drum_recipe_t SPLASH   = {4300.0f, 4300.0f, 0.05f, 0.18f, 0.35f, 0.40f, 0.35f, true,  false};
    static const drum_recipe_t CHINA    = {2200.0f, 2200.0f, 0.05f, 0.22f, 0.58f, 0.45f, 0.58f, true,  false};
    static const drum_recipe_t RIDE     = {3300.0f, 3300.0f, 0.05f, 0.15f, 0.45f, 0.22f, 0.55f, true,  false};
    static const drum_recipe_t BELL     = {3300.0f, 3300.0f, 0.05f, 0.35f, 0.80f, 0.10f, 0.40f, true,  false};
    static const drum_recipe_t TAMB     = {0.0f, 0.0f, 0.05f, 0.0f, 0.10f, 0.40f, 0.09f, true,  false};
    static const drum_recipe_t COWB     = {800.0f, 800.0f, 0.05f, 0.50f, 0.20f, 0.05f, 0.02f, true,  false};
    static const drum_recipe_t TOM      = {0.0f, 0.0f, 0.04f, 0.90f, 0.16f, 0.06f, 0.012f, true, false};
    static const drum_recipe_t CONGA    = {0.0f, 0.0f, 0.02f, 0.80f, 0.09f, 0.10f, 0.01f, true,  false};
    static const drum_recipe_t CLICK    = {1000.0f, 900.0f, 0.02f, 0.50f, 0.04f, 0.10f, 0.01f, true, false};
    static const drum_recipe_t SHAKE    = {0.0f, 0.0f, 0.05f, 0.0f, 0.10f, 0.30f, 0.05f, true,  false};
    static const drum_recipe_t GUIRO    = {0.0f, 0.0f, 0.05f, 0.0f, 0.10f, 0.30f, 0.10f, false, false};
    static const drum_recipe_t WHIS     = {2400.0f, 2400.0f, 0.05f, 0.35f, 0.18f, 0.0f, 0.02f, false, false};
    static const drum_recipe_t TRI      = {4200.0f, 4200.0f, 0.05f, 0.30f, 0.60f, 0.05f, 0.10f, true,  false};
    drum_recipe_t r;

    switch (note) {
    case 35: r = KICK_ACO; break;
    case 36: r = KICK;     break;
    case 37: r = STICK;    break;
    case 38: r = SNARE;    break;
    case 40: r = SNARE_EL; break;
    case 39: r = CLAP;     break;
    case 42: r = HAT_C;    break;
    case 44: r = HAT_P;    break;
    case 46: r = HAT_O;    break;
    case 49: r = CRASH1;   break;
    case 57: r = CRASH2;   break;
    case 55: r = SPLASH;   break;
    case 52: r = CHINA;    break;
    case 51: case 59: return RIDE;
    case 53: return BELL;
    case 54: return TAMB;
    case 56: return COWB;
    case 41: case 43: case 45: case 47: case 48: case 50: {
        static const float tom_hz[] = {85.0f, 100.0f, 120.0f, 145.0f, 170.0f, 200.0f};
        static const uint8_t tom_note[] = {41, 43, 45, 47, 48, 50};
        r = TOM;
        for (unsigned i = 0; i < sizeof(tom_note); i++) {
            if (tom_note[i] == note) {
                r.f1 = tom_hz[i];
                r.f0 = tom_hz[i] * 1.6f;
            }
        }
        break;
    }
    case 60: case 61: case 62: case 63: case 64: case 65: case 66:
        r = CONGA;
        r.f1 = 220.0f + 35.0f * (float)(note - 60);
        r.f0 = r.f1 * 1.3f;
        return r;
    case 67: case 68: return COWB;
    case 69: case 70: return SHAKE;
    case 71: case 72: return WHIS;
    case 73: case 74: return GUIRO;
    case 75: case 76: case 77: return CLICK;
    case 80: case 81: r = TRI;   break;
    default:          r = SHAKE; break;
    }

    /* Adapt recipe characteristics based on MIDI Drum Kit Program */
    if (kit_prog >= 16 && kit_prog <= 23) {
        /* Power / Rock Kit: Punchier kick & gated snare */
        if (note == 35 || note == 36) { r.f0 *= 1.15f; r.tone_lvl *= 1.1f; }
        if (note == 38 || note == 40) { r.tone_lvl *= 1.15f; r.noise_tau *= 1.25f; }
    } else if (kit_prog >= 24 && kit_prog <= 31) {
        /* Electronic / TR-808 Kit: Booming sub-bass kick & snappy analog snare */
        if (note == 35 || note == 36) { r.f1 = 38.0f; r.tone_tau = 0.28f; }
        if (note == 38 || note == 40) { r.hp = true; r.f0 = 290.0f; }
    } else if (kit_prog >= 8 && kit_prog <= 15) {
        /* Room Kit: Longer ambient room decay */
        r.tone_tau *= 1.25f;
        r.noise_tau *= 1.30f;
    } else if (kit_prog >= 32 && kit_prog <= 47) {
        /* Jazz / Brush Kit: Soft brush snare & warm mellow kick */
        if (note == 38 || note == 40) { r.noise_lvl *= 0.65f; r.noise_tau *= 1.35f; }
        if (note == 35 || note == 36) { r.f0 *= 0.85f; r.tone_lvl *= 0.85f; }
    }
    return r;
}

typedef struct {
    const int16_t *data;
    uint32_t len;
    float pitch;
} opl4_drum_wave_t;

static opl4_drum_wave_t make_opl4_wave(uint8_t note, uint8_t kit_prog)
{
    opl4_drum_wave_t w = { NULL, 0, 1.0f };

    switch (note) {
    case 35: w.data = OPL4_PCM_KICK;    w.len = OPL4_PCM_KICK_LEN;    w.pitch = 0.90f; break;
    case 36: w.data = OPL4_PCM_KICK;    w.len = OPL4_PCM_KICK_LEN;    w.pitch = 1.04f; break;
    case 37: w.data = OPL4_PCM_STICK;   w.len = OPL4_PCM_STICK_LEN;   w.pitch = 1.00f; break;
    case 38: w.data = OPL4_PCM_SNARE;   w.len = OPL4_PCM_SNARE_LEN;   w.pitch = 1.00f; break;
    case 39: w.data = OPL4_PCM_CLAP;    w.len = OPL4_PCM_CLAP_LEN;    w.pitch = 1.00f; break;
    case 40: w.data = OPL4_PCM_SNARE;   w.len = OPL4_PCM_SNARE_LEN;   w.pitch = 1.14f; break;
    case 42: w.data = OPL4_PCM_HAT_C;   w.len = OPL4_PCM_HAT_C_LEN;   w.pitch = 1.00f; break;
    case 44: w.data = OPL4_PCM_HAT_C;   w.len = OPL4_PCM_HAT_C_LEN;   w.pitch = 0.86f; break;
    case 46: w.data = OPL4_PCM_HAT_O;   w.len = OPL4_PCM_HAT_O_LEN;   w.pitch = 1.00f; break;
    case 49: w.data = OPL4_PCM_CRASH;   w.len = OPL4_PCM_CRASH_LEN;   w.pitch = 1.00f; break;
    case 57: w.data = OPL4_PCM_CRASH;   w.len = OPL4_PCM_CRASH_LEN;   w.pitch = 1.15f; break;
    case 55: w.data = OPL4_PCM_CRASH;   w.len = OPL4_PCM_CRASH_LEN;   w.pitch = 1.45f; break;
    case 52: w.data = OPL4_PCM_CRASH;   w.len = OPL4_PCM_CRASH_LEN;   w.pitch = 0.82f; break;
    case 51: w.data = OPL4_PCM_RIDE;    w.len = OPL4_PCM_RIDE_LEN;    w.pitch = 1.00f; break;
    case 59: w.data = OPL4_PCM_RIDE;    w.len = OPL4_PCM_RIDE_LEN;    w.pitch = 1.08f; break;
    case 53: w.data = OPL4_PCM_RIDE;    w.len = OPL4_PCM_RIDE_LEN;    w.pitch = 1.35f; break;
    case 54: w.data = OPL4_PCM_HAT_C;   w.len = OPL4_PCM_HAT_C_LEN;   w.pitch = 1.25f; break;
    case 56: w.data = OPL4_PCM_COWBELL; w.len = OPL4_PCM_COWBELL_LEN; w.pitch = 1.00f; break;
    case 41: case 43: case 45: case 47: case 48: case 50: {
        static const float tom_p[] = {0.65f, 0.77f, 0.92f, 1.12f, 1.31f, 1.54f};
        static const uint8_t tom_n[] = {41, 43, 45, 47, 48, 50};
        w.data = OPL4_PCM_TOM;
        w.len = OPL4_PCM_TOM_LEN;
        for (unsigned i = 0; i < sizeof(tom_n); i++) {
            if (tom_n[i] == note) {
                w.pitch = tom_p[i];
            }
        }
        break;
    }
    case 60: case 61: case 62: case 63: case 64: case 65: case 66:
        w.data = OPL4_PCM_TOM;
        w.len = OPL4_PCM_TOM_LEN;
        w.pitch = 1.65f + 0.18f * (float)(note - 60);
        break;
    case 67: case 68:
        w.data = OPL4_PCM_COWBELL;
        w.len = OPL4_PCM_COWBELL_LEN;
        w.pitch = (note == 67) ? 1.35f : 1.10f;
        break;
    default:
        w.data = OPL4_PCM_HAT_C;
        w.len = OPL4_PCM_HAT_C_LEN;
        w.pitch = 1.15f;
        break;
    }

    if (kit_prog >= 16 && kit_prog <= 23) {
        w.pitch *= 0.95f; /* Power kit: deeper tuning */
    } else if (kit_prog >= 24 && kit_prog <= 31) {
        w.pitch *= 1.06f; /* Electronic kit: snappy attack */
    }
    return w;
}

/* -------------------------------------------------------------------------- */
/* Event ring                                                                 */
/* -------------------------------------------------------------------------- */
static bool push_event(uint8_t type, uint8_t ch, uint8_t a, uint8_t b)
{
    unsigned int key = irq_lock();
    bool ok = ((uint32_t)(s_ev_head - s_ev_tail) < EVENT_RING_SIZE);

    if (!ok) {
        s_dropped_events++;
    } else {
        synth_event_t *e = &s_events[s_ev_head & EVENT_RING_MASK];
        uint32_t frame = 0;

        if (s_stamp_valid) {
            frame = (s_stamp != 0U) ? s_stamp : 1U;
            int32_t margin = (int32_t)(frame - s_frame_clock);
            if (margin < s_min_margin) {
                s_min_margin = margin;
            }
        }
        s_stamp_valid = false;
        e->frame = frame;
        e->type = type;
        e->ch = ch;
        e->a = a;
        e->b = b;
        s_ev_head++;

        uint32_t used = s_ev_head - s_ev_tail;
        if (used > s_ring_peak) {
            s_ring_peak = used;
        }
    }
    irq_unlock(key);
    return ok;
}

/* -------------------------------------------------------------------------- */
/* Voice management (audio thread only)                                       */
/* -------------------------------------------------------------------------- */
static inline uint32_t float_to_phase(float x)
{
    return (x >= 4.2e9f) ? 0x7FFFFFFFU : (uint32_t)x;
}

static void voice_release(fm_voice_t *v)
{
    if (v->active && v->state != ST_RELEASE) {
        v->state = ST_RELEASE;
    }
}

static void voice_fast_release(fm_voice_t *v)
{
    if (!v->active) {
        return;
    }
    v->key_down = false;
    v->pedal_held = false;
    v->state = ST_RELEASE;
    v->rel_k = s_fast_rel_k;
    v->tone_k = (v->tone_k < s_fast_rel_k) ? v->tone_k : s_fast_rel_k;
    v->noise_k = (v->noise_k < s_fast_rel_k) ? v->noise_k : s_fast_rel_k;
    v->pcm_k = (v->pcm_k < s_fast_rel_k) ? v->pcm_k : s_fast_rel_k;
}

static fm_voice_t *alloc_voice(void)
{
    int best = -1;
    float best_amp = 1e9f;

    for (int i = 0; i < FM_MAX_VOICES; i++) {
        if (!s_voices[i].active) {
            return &s_voices[i];
        }
    }
    /* Everything is busy: take the quietest voice that is already dying away, otherwise the oldest. */
    for (int i = 0; i < FM_MAX_VOICES; i++) {
        if (s_voices[i].state == ST_RELEASE && s_voices[i].amp < best_amp) {
            best_amp = s_voices[i].amp;
            best = i;
        }
    }
    if (best < 0) {
        uint32_t oldest = 0xFFFFFFFFU;
        for (int i = 0; i < FM_MAX_VOICES; i++) {
            if (s_voices[i].age < oldest) {
                oldest = s_voices[i].age;
                best = i;
            }
        }
    }
    s_steals++;
    if (s_voices[best].state != ST_RELEASE || s_voices[best].amp > 0.03f) {
        s_steals_audible++;
    }
    return &s_voices[best];
}

static void fm_note_on(uint8_t ch, uint8_t note, uint8_t vel, uint8_t prog)
{
    const uint8_t pid = PROGRAM_PATCH[prog & 127U];
    const fm_patch_t *p = &PATCHES[pid];
    const ext_patch_t *xp = &EXT_PATCHES[pid];
    uint32_t busy = 0;

    for (int i = 0; i < FM_MAX_VOICES; i++) {
        fm_voice_t *o = &s_voices[i];
        if (o->active) {
            busy++;
        }
        if (o->active && !o->is_drum && o->channel == ch && o->note == note && o->key_down) {
            o->key_down = false;
            o->pedal_held = false;
            voice_release(o);
        }
    }

    fm_voice_t *v = alloc_voice();
    float f0 = s_note_inc[note] * s_sample_rate / 4294967296.0f;
    float f_m = f0 * p->ratio_m;
    float scale = 0.55f + 0.45f * ((float)vel / 127.0f);
    float idx_max = (p->idx_start > p->idx_sus) ? p->idx_start : p->idx_sus;

    memset(v, 0, sizeof(*v));
    v->active = true;
    v->channel = ch;
    v->note = note;
    v->key_down = true;
    v->state = ST_ATTACK;
    v->age = ++s_age_counter;
    v->additive = p->additive;
    v->wave_m = PATCH_WAVE_M[pid];
    v->level = p->level;
    v->svf_lp = 0;
    v->svf_bp = 0;
    v->svf_q = ((pid >= P_SYN_BASS1 && pid <= P_SYN_BASS2) || pid == P_SYN_BRASS || pid == P_SQ_LEAD || pid == P_SAW_LEAD || pid == P_CLAV || pid == P_SLAP) ? 15000 : 25500;
    v->vib = p->vib;
    {
        float vg = (float)vel / 127.0f;
        v->vel_gain = vg * sqrtf(vg);
    }

    v->base_c = s_note_inc[note] * p->ratio_c;
    v->base_m = s_note_inc[note] * p->ratio_m;
    v->fb_u = float_to_phase(p->fb * MOD_IDX_UNIT);

    if (note > 64U) {
        /* Keyboard rate scaling: smoothly attenuate modulation depth on upper octaves (-2.5 dB/octave)
         * to eliminate metallic FM screech on high notes. */
        scale *= exp2f(-(float)(note - 64U) * (1.0f / 28.0f));
    }

    if (p->additive) {
        scale = 1.0f;
    } else {
        /* Keep the FM sidebands below about 11 kHz so high notes do not alias. */
        if (f_m * (idx_max + 1.0f) > MAX_BANDWIDTH_HZ && idx_max > 0.0f) {
            float room = MAX_BANDWIDTH_HZ / f_m - 1.0f;
            float limit = (room > 0.0f) ? room / idx_max : 0.0f;
            if (limit < scale) {
                scale = limit;
            }
        }
        if (f_m > 0.45f * s_sample_rate) {
            scale = 0.0f;
            v->base_m = 0.0f;
        }
    }
    v->idx = p->idx_start * scale;
    v->idx_sus = p->idx_sus * scale;
    {
        float tau = (p->idx_tau > s_sub_sec) ? p->idx_tau : s_sub_sec;
        v->idx_k = expf(-s_sub_sec / tau);
    }

    v->ops = 2;
    if (xp->kind != EXT_NONE && !p->additive) {
        if (!s_third_op || busy > THIRD_OP_MAX_VOICES) {
            s_ext_skipped++;
        } else if (xp->kind == EXT_ENSEMBLE) {
            /* 6-Op (3-Pair: L/R detuned + Octave harmonic shimmer pair) when <= SIX_OP_MAX_VOICES */
            float d = exp2f(xp->a / 1200.0f);
            v->ext = EXT_ENSEMBLE;
            v->ops = (busy <= SIX_OP_MAX_VOICES) ? 6 : 4;
            v->base_c3 = v->base_c * 2.001f;
            v->base_m3 = v->base_m * 2.001f * xp->b;
            v->base_x = v->base_c * d;
            v->base_c = v->base_c / d;
            v->base_m2 = v->base_m * d * xp->b;
            v->base_m = v->base_m / d;
            v->pc2 = v->pc;
            v->pm2 = v->pm;
            v->pc3 = v->pc;
            v->pm3 = v->pm;
            v->tine_idx = xp->c;
            s_ext_started++;
        } else {
            /* 6-Op / 4-Op Tine & Hammer Pair */
            float f_t = f0 * xp->a;
            float idx = xp->b;
            float tscale = 1.0f;

            if (f_t * (idx + 1.0f) > MAX_BANDWIDTH_HZ) {
                float room = MAX_BANDWIDTH_HZ / f_t - 1.0f;
                tscale = (room > 0.0f) ? room / idx : 0.0f;
            }
            if (f_t <= 0.45f * s_sample_rate && tscale > 0.0f) {
                float tt = (xp->c > s_sub_sec) ? xp->c : s_sub_sec;
                v->ext = EXT_TINE;
                v->ops = (busy <= SIX_OP_MAX_VOICES) ? 6 : 4;
                v->base_x = s_note_inc[note] * xp->a;
                v->base_m2 = s_note_inc[note] * (p->ratio_c * 2.003f);
                v->base_c3 = s_note_inc[note] * (p->ratio_c * 3.005f);
                v->base_m3 = s_note_inc[note] * (xp->a * 1.498f);
                v->pm2 = v->pc;
                v->pc2 = v->pc;
                v->pm3 = v->pc;
                v->pc3 = v->pc;
                v->tine_idx = idx * tscale * (0.4f + 0.6f * ((float)vel / 127.0f));
                v->tine_k = expf(-s_sub_sec / tt);
                s_ext_started++;
            }
        }
    }
    {
        float atk = (p->attack > s_sub_sec) ? p->attack : s_sub_sec;
        float dec = (p->dec_tau > s_sub_sec) ? p->dec_tau : s_sub_sec;
        float rel = (p->release > s_sub_sec) ? p->release : s_sub_sec;
        v->atk_step = s_sub_sec / atk;
        v->dec_k = expf(-s_sub_sec / dec);
        v->rel_k = expf(-s_sub_sec / rel);
        v->sus = p->sustain;
    }
}

static void drum_note_on(uint8_t note, uint8_t vel)
{
    drum_recipe_t r = make_recipe(note, s_channels[FM_DRUM_CHANNEL].program);

    if (note == 42 || note == 44) {
        for (int i = 0; i < FM_MAX_VOICES; i++) {
            if (s_voices[i].active && s_voices[i].open_hat) {
                voice_fast_release(&s_voices[i]);
            }
        }
    }

    fm_voice_t *v = alloc_voice();
    float vg = (float)vel / 127.0f;
    float tf = (r.f_tau > s_sub_sec) ? r.f_tau : s_sub_sec;
    float tt = (r.tone_tau > s_sub_sec) ? r.tone_tau : s_sub_sec;
    float nt = (r.noise_tau > s_sub_sec) ? r.noise_tau : s_sub_sec;

    memset(v, 0, sizeof(*v));
    v->active = true;
    v->is_drum = true;
    v->channel = FM_DRUM_CHANNEL;
    v->note = note;
    v->open_hat = r.open_hat;
    v->state = ST_RELEASE;              /* one-shots: first in line to be stolen */
    v->age = ++s_age_counter;
    v->vel_gain = vg * sqrtf(vg);
    v->tone_f = r.f0;
    v->tone_f1 = r.f1;
    v->tone_fk = expf(-s_sub_sec / tf);
    v->tone_amp = 1.0f;
    v->tone_k = expf(-s_sub_sec / tt);
    v->tone_lvl = r.tone_lvl;
    v->noise_amp = 1.0f;
    v->noise_k = expf(-s_sub_sec / nt);
    v->noise_lvl = r.noise_lvl;
    v->hp = r.hp;
    v->rng = 0x2545F491U + (uint32_t)note * 2654435761U + v->age;
    v->amp = (r.tone_lvl > r.noise_lvl) ? r.tone_lvl : r.noise_lvl;

    if (s_opl4_drums) {
        opl4_drum_wave_t w = make_opl4_wave(note, s_channels[FM_DRUM_CHANNEL].program);
        v->pcm_data = w.data;
        v->pcm_len = w.len;
        v->pcm_pos = 0;
        v->pcm_step = (uint32_t)(((float)OPL4_PCM_SAMPLE_RATE * 65536.0f * w.pitch) / s_sample_rate);
        v->pcm_amp = 1.0f;
        v->pcm_k = 1.0f;
    }
}

static void apply_note_off(uint8_t ch, uint8_t note)
{
    for (int i = 0; i < FM_MAX_VOICES; i++) {
        fm_voice_t *v = &s_voices[i];
        if (v->active && !v->is_drum && v->channel == ch && v->note == note && v->key_down) {
            v->key_down = false;
            if (s_channels[ch].pedal) {
                v->pedal_held = true;
            } else {
                voice_release(v);
            }
        }
    }
}

static void apply_pedal_up(uint8_t ch)
{
    for (int i = 0; i < FM_MAX_VOICES; i++) {
        fm_voice_t *v = &s_voices[i];
        if (v->active && v->channel == ch && v->pedal_held) {
            v->pedal_held = false;
            if (!v->key_down) {
                voice_release(v);
            }
        }
    }
}

static void reset_channels(void)
{
    for (int ch = 0; ch < FM_MIDI_CHANNELS; ch++) {
        midi_channel_state_t *c = &s_channels[ch];
        memset(c, 0, sizeof(*c));
        c->volume = 100;
        c->expression = 127;
        c->pan = 64;
        c->rpn_msb = 127;
        c->rpn_lsb = 127;
        c->bend_semis = 2;
        c->pan_cached = 255;
        c->gain_prev = -1.0f;
        c->reverb = 40;                 /* General MIDI default send levels */
        c->chorus = 0;
    }
}

static void reset_state(void)
{
    memset(s_voices, 0, sizeof(s_voices));
    reset_channels();
    s_age_counter = 0;
    s_lfo_phase = 0.0f;
}

static void channel_voices_off(uint8_t ch)
{
    for (int i = 0; i < FM_MAX_VOICES; i++) {
        if (s_voices[i].channel == ch) {
            voice_fast_release(&s_voices[i]);
        }
    }
}

static void apply_cc(uint8_t channel, uint8_t control, uint8_t value)
{
    midi_channel_state_t *c = &s_channels[channel];

    switch (control) {
    case 1:
        c->mod = value;
        break;
    case 6:
        if (c->rpn_msb == 0U && c->rpn_lsb == 0U) {
            c->bend_semis = (value > 24U) ? 24U : value;
        }
        break;
    case 38:
        if (c->rpn_msb == 0U && c->rpn_lsb == 0U) {
            c->bend_cents = (value > 99U) ? 99U : value;
        }
        break;
    case 7:
        c->volume = value;
        break;
    case 10:
        c->pan = value;
        break;
    case 11:
        c->expression = value;
        break;
    case 64: {
        bool down = (value >= 64U);
        if (c->pedal && !down) {
            apply_pedal_up(channel);
        }
        c->pedal = down;
        break;
    }
    case 91:
        c->reverb = value;
        break;
    case 93:
        c->chorus = value;
        break;
    case 98:
    case 99:
        c->rpn_msb = 127;
        c->rpn_lsb = 127;
        break;
    case 100:
        c->rpn_lsb = value;
        break;
    case 101:
        c->rpn_msb = value;
        break;
    case 120:
    case 123:
        channel_voices_off(channel);
        break;
    case 121:
        if (c->pedal) {
            apply_pedal_up(channel);
        }
        c->pedal = false;
        c->expression = 127;
        c->mod = 0;
        c->bend = 0;
        c->rpn_msb = 127;
        c->rpn_lsb = 127;
        break;
    default:
        break;
    }
}

static void apply_event(const synth_event_t *e)
{
    uint8_t ch = e->ch;

    switch (e->type) {
    case EV_NOTE_ON:
        if (ch == FM_DRUM_CHANNEL) {
            drum_note_on(e->a, e->b);
        } else {
            fm_note_on(ch, e->a, e->b, s_channels[ch].program);
        }
        break;
    case EV_NOTE_OFF:
        apply_note_off(ch, e->a);
        break;
    case EV_CC:
        apply_cc(ch, e->a, e->b);
        break;
    case EV_PROGRAM:
        s_channels[ch].program = (uint8_t)(e->a & 127U);
        break;
    case EV_BEND:
        s_channels[ch].bend = (int16_t)((int)(e->a | (e->b << 7)) - 8192);
        break;
    case EV_ALL_OFF:
        for (int i = 0; i < FM_MAX_VOICES; i++) {
            voice_fast_release(&s_voices[i]);
        }
        break;
    default:
        break;
    }
}

/* Applies, in order, every queued event whose frame has come. An event without a frame (0)
 * is due at once. The first event that is not due yet holds back all that follow it, so the
 * order the sequencer produced is never changed, only stretched. */
static void drain_events(uint32_t limit)
{
    while (s_ev_tail != s_ev_head) {
        synth_event_t e = s_events[s_ev_tail & EVENT_RING_MASK];

        if (e.frame != 0U && (int32_t)(e.frame - limit) > 0) {
            break;
        }
        s_ev_tail++;
        if (e.frame != 0U && (int32_t)(s_frame_clock - e.frame) > (int32_t)(2U * SUB_FRAMES)) {
            s_late_events++;
        }
        apply_event(&e);
    }
}

/* Frames in @p us microseconds (signed, a few hundred milliseconds at most). */
static int32_t us_to_frames(int32_t us)
{
    float f = (float)us * ((float)s_rate_u * 1e-6f);

    return (int32_t)(f + ((f >= 0.0f) ? 0.5f : -0.5f));
}

/* -------------------------------------------------------------------------- */
/* Public control interface (any context)                                     */
/* -------------------------------------------------------------------------- */
static void reset_send_memory(void)
{
    memset(s_last_cc, 0xFF, sizeof(s_last_cc));
    for (int ch = 0; ch < FM_MIDI_CHANNELS; ch++) {
        s_last_bend[ch] = 0xFFFFU;
    }
}

void yamaha_fm_synth_init(uint32_t sample_rate)
{
    s_rate_u = (sample_rate > 0U) ? sample_rate : 44100U;
    s_sample_rate = (float)s_rate_u;
    s_sub_sec = (float)SUB_FRAMES / s_sample_rate;
    s_fast_rel_k = expf(-s_sub_sec / 0.006f);
    s_lead_frames = s_rate_u * EVENT_LEAD_MS / 1000U;

    memcpy(s_waves, FLASH_OPL3_WAVES, sizeof(s_waves));
    for (int n = 0; n < 128; n++) {
        float freq = 440.0f * exp2f(((float)n - 69.0f) / 12.0f);
        s_note_inc[n] = freq * 4294967296.0f / s_sample_rate;
    }

    s_ev_head = 0;
    s_ev_tail = 0;
    s_flush_to = 0;
    s_frame_clock = 0;
    s_stamp_valid = false;
    s_anchor_valid = false;
    s_flush_seen = s_flush_request;
    s_dropped_events = 0;
    s_ext_started = 0;
    s_ext_skipped = 0;
    s_late_events = 0;
    s_ring_peak = 0;
    s_min_margin = INT32_MAX;
    s_steals = 0;
    s_steals_audible = 0;
    reset_send_memory();
    dsp_init(s_sample_rate);
    reset_state();
}

void yamaha_fm_synth_reset(void)
{
    /* Call while the sequencer is not delivering events (between songs). The audio thread drops
     * whatever is still queued and lets the voices die away. */
    unsigned int key = irq_lock();

    reset_channels();
    reset_send_memory();
    s_anchor_valid = false;
    s_stamp_valid = false;
    s_flush_to = s_ev_head;
    s_flush_request++;
    irq_unlock(key);
}

void yamaha_fm_note_on(uint8_t channel, uint8_t note, uint8_t velocity)
{
    if (channel >= FM_MIDI_CHANNELS || note >= 128U) {
        s_stamp_valid = false;
        return;
    }
    if (velocity == 0U) {
        yamaha_fm_note_off(channel, note, 0);
        return;
    }
    push_event(EV_NOTE_ON, channel, note, velocity);
}

void yamaha_fm_note_off(uint8_t channel, uint8_t note, uint8_t velocity)
{
    ARG_UNUSED(velocity);
    if (channel >= FM_MIDI_CHANNELS || note >= 128U || channel == FM_DRUM_CHANNEL) {
        s_stamp_valid = false;
        return;
    }
    push_event(EV_NOTE_OFF, channel, note, 0);
}

void yamaha_fm_program_change(uint8_t channel, uint8_t program)
{
    if (channel < FM_MIDI_CHANNELS) {
        push_event(EV_PROGRAM, channel, (uint8_t)(program & 127U), 0);
    } else {
        s_stamp_valid = false;
    }
}

/* Slot in s_last_cc for the continuous controllers that songs repeat by the thousand. */
static int cc_slot(uint8_t control)
{
    switch (control) {
    case 1:  return 0;
    case 7:  return 1;
    case 10: return 2;
    case 11: return 3;
    case 91: return 4;
    case 93: return 5;
    default: return -1;
    }
}

void yamaha_fm_control_change(uint8_t channel, uint8_t control, uint8_t value)
{
    if (channel >= FM_MIDI_CHANNELS) {
        s_stamp_valid = false;
        return;
    }
    value &= 127U;

    if (control == 121U) {
        memset(s_last_cc[channel], 0xFF, sizeof(s_last_cc[channel]));
    }
    int slot = cc_slot(control);
    if (slot >= 0 && s_last_cc[channel][slot] == value) {
        s_stamp_valid = false;      /* same value again: nothing to do, keep the ring free */
        return;
    }
    if (push_event(EV_CC, channel, control, value) && slot >= 0) {
        s_last_cc[channel][slot] = value;
    }
}

void yamaha_fm_pitch_bend(uint8_t channel, uint16_t bend)
{
    if (channel >= FM_MIDI_CHANNELS) {
        s_stamp_valid = false;
        return;
    }
    bend &= 0x3FFFU;
    if (s_last_bend[channel] == bend) {
        s_stamp_valid = false;
        return;
    }
    if (push_event(EV_BEND, channel, (uint8_t)(bend & 0x7FU), (uint8_t)(bend >> 7))) {
        s_last_bend[channel] = bend;
    }
}

void yamaha_fm_all_notes_off(void)
{
    push_event(EV_ALL_OFF, 0, 0, 0);
}

/* The sequencer reports where the song clock stands each time it wakes. The map from song time
 * to audio frame is nudged toward "now plus the lead" a sixty-fourth of the error at a time, so
 * a late or early wake-up of the sequencer thread barely moves it, while the difference between
 * the song clock and the audio clock (the I2S rate is not exactly 44100) is followed. */
void yamaha_fm_song_time_anchor(uint32_t song_us)
{
    uint32_t target = s_frame_clock + s_lead_frames;

    if (!s_anchor_valid) {
        s_map_us = song_us;
        s_map_frame = target;
        s_anchor_valid = true;
        return;
    }
    uint32_t predicted = s_map_frame + (uint32_t)us_to_frames((int32_t)(song_us - s_map_us));
    int32_t err = (int32_t)(target - predicted);

    if (err > (int32_t)(s_rate_u / 50U) || err < -(int32_t)(s_rate_u / 50U)) {
        s_map_frame = target;           /* resumed after a pause, or far off: start over */
    } else {
        s_map_frame = predicted + (uint32_t)(err / 64);
    }
    s_map_us = song_us;
}

void yamaha_fm_song_time_event(uint32_t song_us)
{
    if (!s_anchor_valid) {
        s_stamp_valid = false;
        return;
    }
    s_stamp = s_map_frame + (uint32_t)us_to_frames((int32_t)(song_us - s_map_us));
    s_stamp_valid = true;
}

void yamaha_fm_set_third_operator(bool on)
{
    s_third_op = on;
}

bool yamaha_fm_get_third_operator(void)
{
    return s_third_op;
}

void yamaha_fm_set_opl4_drums(bool on)
{
    s_opl4_drums = on;
}

bool yamaha_fm_get_opl4_drums(void)
{
    return s_opl4_drums;
}

void yamaha_fm_set_vcf(bool on)
{
    s_vcf_on = on;
}

bool yamaha_fm_get_vcf(void)
{
    return s_vcf_on;
}

void yamaha_fm_get_third_operator_stats(uint32_t *started, uint32_t *skipped_busy)
{
    if (started != NULL) {
        *started = s_ext_started;
    }
    if (skipped_busy != NULL) {
        *skipped_busy = s_ext_skipped;
    }
}

void yamaha_fm_set_effects_level(uint8_t percent)
{
    dsp_set_effects_level(percent);
}

uint8_t yamaha_fm_get_effects_level(void)
{
    return dsp_get_effects_level();
}

void yamaha_fm_set_compressor(bool on)
{
    dsp_set_compressor(on);
}

bool yamaha_fm_get_compressor(void)
{
    return dsp_get_compressor();
}

void yamaha_fm_get_event_stats(yamaha_fm_event_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    out->late_events = s_late_events;
    out->min_margin_frames = s_min_margin;
    out->ring_peak = s_ring_peak;
    out->dropped_events = s_dropped_events;
    out->limiter_samples = dsp_get_limiter_samples();
    out->max_reduction_db10 = dsp_take_reduction_db10();
}

void yamaha_fm_reset_event_stats(void)
{
    s_late_events = 0;
    s_ring_peak = 0;
    s_min_margin = INT32_MAX;
}

uint8_t yamaha_fm_get_active_voice_count(void)
{
    uint8_t count = 0;

    for (int i = 0; i < FM_MAX_VOICES; i++) {
        if (s_voices[i].active && s_voices[i].amp > 0.01f) {
            count++;
        }
    }
    return count;
}

uint32_t yamaha_fm_get_steal_count(void)
{
    return s_steals;
}

uint32_t yamaha_fm_get_audible_steal_count(void)
{
    return s_steals_audible;
}

uint32_t yamaha_fm_get_dropped_events(void)
{
    return s_dropped_events;
}

/* -------------------------------------------------------------------------- */
/* Rendering                                                                  */
/* -------------------------------------------------------------------------- */
static void env_step(fm_voice_t *v)
{
    switch (v->state) {
    case ST_ATTACK:
        v->amp += v->atk_step;
        if (v->amp >= 1.0f) {
            v->amp = 1.0f;
            v->state = ST_DECAY;
        }
        break;
    case ST_DECAY:
        v->amp = v->sus + (v->amp - v->sus) * v->dec_k;
        if (v->sus <= SILENCE) {
            if (v->amp < SILENCE) {
                v->active = false;
            }
        } else if (v->amp - v->sus < 0.003f) {
            v->amp = v->sus;
            v->state = ST_SUSTAIN;
        }
        break;
    case ST_RELEASE:
        v->amp *= v->rel_k;
        if (v->amp < SILENCE) {
            v->active = false;
        }
        break;
    default:
        break;
    }
}

static void render_fm_sub(fm_voice_t *v, size_t len, float ch_gain, float ratio,
                          float lfo, const midi_channel_state_t *c, int32_t *bl, int32_t *br,
                          uint32_t active_poly)
{
    env_step(v);
    if (!v->active) {
        return;
    }
    v->idx = v->idx_sus + (v->idx - v->idx_sus) * v->idx_k;

    float r = ratio;
    float vib = v->vib + (float)c->mod * (0.8f / 127.0f);
    if (vib > 0.0f) {
        r *= 1.0f + vib * lfo * SEMITONE_LINEAR;
    }

    uint32_t inc_c = float_to_phase(v->base_c * r);
    uint32_t inc_m = float_to_phase(v->base_m * r);
    float amp = v->amp * v->vel_gain * v->level * ch_gain * (float)GAIN_ONE;
    int32_t gl1 = (int32_t)(amp * c->pan_l);
    int32_t gr1 = (int32_t)(amp * c->pan_r);
    if (gl1 > GAIN_ONE) gl1 = GAIN_ONE;
    if (gr1 > GAIN_ONE) gr1 = GAIN_ONE;
    int32_t gl = gl1 >> GAIN_Q;
    int32_t gr = gr1 >> GAIN_Q;
    uint32_t pc = v->pc, pm = v->pm;
    int32_t y = v->fb_prev;
    uint32_t fbu = v->fb_u;
    const int16_t *wt = s_waves[v->wave_m];

    /* Dynamic Resonant State-Variable Filter (VCF): cutoff follows voice amplitude & velocity.
     * When polyphony exceeds 28 simultaneous voices, VCF is smoothly bypassed to ensure zero DMA misses. */
    bool vcf_active = (s_vcf_on && !v->additive && v->ext != EXT_ENSEMBLE && active_poly <= 28U);
    int32_t svf_f = 0;
    (void)v->svf_q;
    int32_t svf_lp = v->svf_lp;
    int32_t svf_bp = v->svf_bp;
    if (vcf_active) {
        float env_f = 0.38f + 0.62f * (v->amp * (0.6f + 0.4f * v->vel_gain));
        svf_f = (int32_t)(env_f * 24000.0f);
        if (svf_f > 26500) svf_f = 26500;
    }

    uint8_t eff_ops = v->ops;
    if (active_poly > 34U && eff_ops == 6) eff_ops = 4;
    if (active_poly > 48U) eff_ops = 2;

    if (v->ext == EXT_ENSEMBLE && eff_ops >= 4) {
        uint32_t iu = float_to_phase(v->idx * MOD_IDX_UNIT);
        uint32_t iu2 = float_to_phase(v->idx * v->tine_idx * MOD_IDX_UNIT);
        uint32_t inc_2 = float_to_phase(v->base_x * r);
        uint32_t inc_m2 = float_to_phase(v->base_m2 * r);
        uint32_t pc2 = v->pc2;
        uint32_t pm2 = v->pm2;

        if (eff_ops == 6) {
            /* 6-Operator DX7 3-Pair Super-Ensemble: Left Pair + Right Pair + Center Anchor Pair */
            uint32_t inc_3 = float_to_phase(v->base_c3 * r);
            uint32_t inc_m3 = float_to_phase(v->base_m3 * r);
            uint32_t pc3 = v->pc3;
            uint32_t pm3 = v->pm3;
            int32_t lg_a = (gl * ENSEMBLE6_P_Q10) >> 10;
            int32_t lg_b = (gl * ENSEMBLE6_Q_Q10) >> 10;
            int32_t lg_c = (gl * ENSEMBLE6_C_Q10) >> 10;
            int32_t rg_a = (gr * ENSEMBLE6_Q_Q10) >> 10;
            int32_t rg_b = (gr * ENSEMBLE6_P_Q10) >> 10;
            int32_t rg_c = (gr * ENSEMBLE6_C_Q10) >> 10;

            for (size_t i = 0; i < len; i++) {
                int32_t m1 = wt[(pm + (uint32_t)y * fbu) >> SINE_SHIFT];
                y = m1;
                pm += inc_m;
                int32_t m2 = wt[pm2 >> SINE_SHIFT];
                pm2 += inc_m2;
                int32_t m3 = wt[pm3 >> SINE_SHIFT];
                pm3 += inc_m3;
                int32_t c1 = s_sine[(pc + (uint32_t)m1 * iu) >> SINE_SHIFT];
                int32_t c2 = s_sine[(pc2 + (uint32_t)m2 * iu2) >> SINE_SHIFT];
                int32_t c3 = s_sine[(pc3 + (uint32_t)m3 * iu) >> SINE_SHIFT];
                pc += inc_c;
                pc2 += inc_2;
                pc3 += inc_3;
                bl[i] += (c1 * lg_a + c2 * lg_b + c3 * lg_c) >> 15;
                br[i] += (c1 * rg_a + c2 * rg_b + c3 * rg_c) >> 15;
            }
            v->pc3 = pc3;
            v->pm3 = pm3;
        } else {
            /* 4-Operator Dual-Pair: Pair 1 (Op1/Op2) flat & Pair 2 (Op3/Op4) sharp across stereo */
            int32_t lg_a = (gl * ENSEMBLE_P_Q10) >> 10;
            int32_t lg_b = (gl * ENSEMBLE_Q_Q10) >> 10;
            int32_t rg_a = (gr * ENSEMBLE_Q_Q10) >> 10;
            int32_t rg_b = (gr * ENSEMBLE_P_Q10) >> 10;

            for (size_t i = 0; i < len; i++) {
                int32_t m1 = wt[(pm + (uint32_t)y * fbu) >> SINE_SHIFT];
                y = m1;
                pm += inc_m;
                int32_t m2 = wt[pm2 >> SINE_SHIFT];
                pm2 += inc_m2;
                int32_t c1 = s_sine[(pc + (uint32_t)m1 * iu) >> SINE_SHIFT];
                int32_t c2 = s_sine[(pc2 + (uint32_t)m2 * iu2) >> SINE_SHIFT];
                pc += inc_c;
                pc2 += inc_2;
                bl[i] += (c1 * lg_a + c2 * lg_b) >> 15;
                br[i] += (c1 * rg_a + c2 * rg_b) >> 15;
            }
        }
        v->pc2 = pc2;
        v->pm2 = pm2;
    } else if (v->ext == EXT_TINE && eff_ops >= 4) {
        /* 6-Op / 4-Op Parallel Tine & Hammer Pairs */
        uint32_t iu = float_to_phase(v->idx * MOD_IDX_UNIT);
        uint32_t it = float_to_phase(v->tine_idx * MOD_IDX_UNIT);
        uint32_t inc_t = float_to_phase(v->base_x * r);
        uint32_t inc_c2 = float_to_phase(v->base_m2 * r);
        uint32_t pt = v->pm2;
        uint32_t pc2 = v->pc2;
        int32_t mix_tine = (int32_t)(v->tine_idx * 16384.0f);

        v->tine_idx *= v->tine_k;
        if (eff_ops == 6) {
            uint32_t inc_c3 = float_to_phase(v->base_c3 * r);
            uint32_t inc_m3 = float_to_phase(v->base_m3 * r);
            uint32_t pc3 = v->pc3;
            uint32_t pm3 = v->pm3;
            int32_t mix_wood = mix_tine >> 1;

            for (size_t i = 0; i < len; i++) {
                int32_t m = wt[(pm + (uint32_t)y * fbu) >> SINE_SHIFT];
                y = m;
                pm += inc_m;
                int32_t t = s_sine[pt >> SINE_SHIFT];
                pt += inc_t;
                int32_t c2 = s_sine[(pc2 + (uint32_t)t * it) >> SINE_SHIFT];
                pc2 += inc_c2;
                int32_t m3 = s_sine[pm3 >> SINE_SHIFT];
                pm3 += inc_m3;
                int32_t c3 = s_sine[(pc3 + (uint32_t)m3 * it) >> SINE_SHIFT];
                pc3 += inc_c3;
                int32_t c1 = s_sine[(pc + (uint32_t)m * iu + (uint32_t)t * it) >> SINE_SHIFT];
                pc += inc_c;
                int32_t cs = c1 + ((c2 * mix_tine + c3 * mix_wood) >> 15);
                if (vcf_active) {
                    svf_lp += ((cs - svf_lp) * svf_f) >> 15;
                    cs = svf_lp;
                }
                bl[i] += (cs * gl) >> 15;
                br[i] += (cs * gr) >> 15;
            }
            v->pc3 = pc3;
            v->pm3 = pm3;
        } else {
            for (size_t i = 0; i < len; i++) {
                int32_t m = wt[(pm + (uint32_t)y * fbu) >> SINE_SHIFT];
                y = m;
                pm += inc_m;
                int32_t t = s_sine[pt >> SINE_SHIFT];
                pt += inc_t;
                int32_t c2 = s_sine[(pc2 + (uint32_t)t * it) >> SINE_SHIFT];
                pc2 += inc_c2;
                int32_t c1 = s_sine[(pc + (uint32_t)m * iu + (uint32_t)t * it) >> SINE_SHIFT];
                pc += inc_c;
                int32_t cs = c1 + ((c2 * mix_tine) >> 15);
                if (vcf_active) {
                    svf_lp += ((cs - svf_lp) * svf_f) >> 15;
                    cs = svf_lp;
                }
                bl[i] += (cs * gl) >> 15;
                br[i] += (cs * gr) >> 15;
            }
        }
        v->pm2 = pt;
        v->pc2 = pc2;
    } else if (v->additive) {
        int32_t mix = (int32_t)(v->idx * 32767.0f);
        if (fbu == 0) {
            for (size_t i = 0; i < len; i++) {
                int32_t m = s_sine[pm >> SINE_SHIFT];
                pm += inc_m;
                int32_t cs = s_sine[pc >> SINE_SHIFT] + ((m * mix) >> 15);
                pc += inc_c;
                bl[i] += (cs * gl) >> 15;
                br[i] += (cs * gr) >> 15;
            }
        } else {
            for (size_t i = 0; i < len; i++) {
                int32_t m = s_sine[(pm + (uint32_t)y * fbu) >> SINE_SHIFT];
                y = m;
                pm += inc_m;
                int32_t cs = s_sine[pc >> SINE_SHIFT] + ((m * mix) >> 15);
                pc += inc_c;
                bl[i] += (cs * gl) >> 15;
                br[i] += (cs * gr) >> 15;
            }
        }
    } else {
        uint32_t iu = float_to_phase(v->idx * MOD_IDX_UNIT);
        if (fbu == 0) {
            for (size_t i = 0; i < len; i++) {
                int32_t m = wt[pm >> SINE_SHIFT];
                pm += inc_m;
                int32_t cs = s_sine[(pc + (uint32_t)m * iu) >> SINE_SHIFT];
                pc += inc_c;
                if (vcf_active) {
                    svf_lp += ((cs - svf_lp) * svf_f) >> 15;
                    cs = svf_lp;
                }
                bl[i] += (cs * gl) >> 15;
                br[i] += (cs * gr) >> 15;
            }
        } else {
            for (size_t i = 0; i < len; i++) {
                int32_t m = wt[(pm + (uint32_t)y * fbu) >> SINE_SHIFT];
                y = m;
                pm += inc_m;
                int32_t cs = s_sine[(pc + (uint32_t)m * iu) >> SINE_SHIFT];
                pc += inc_c;
                if (vcf_active) {
                    svf_lp += ((cs - svf_lp) * svf_f) >> 15;
                    cs = svf_lp;
                }
                bl[i] += (cs * gl) >> 15;
                br[i] += (cs * gr) >> 15;
            }
        }
    }

    v->pc = pc;
    v->pm = pm;
    v->fb_prev = y;
    v->gl = gl1;
    v->gr = gr1;
    v->svf_lp = svf_lp;
    v->svf_bp = svf_bp;
}

static void render_drum_sub(fm_voice_t *v, size_t len, float ch_gain,
                            const midi_channel_state_t *c, int32_t *bl, int32_t *br)
{
    v->tone_f = v->tone_f1 + (v->tone_f - v->tone_f1) * v->tone_fk;
    float ta = v->tone_amp * v->tone_lvl * v->vel_gain;
    float na = v->noise_amp * v->noise_lvl * v->vel_gain;
    v->tone_amp *= v->tone_k;
    v->noise_amp *= v->noise_k;
    v->pcm_amp *= v->pcm_k;

    bool has_pcm = (v->pcm_data != NULL && (v->pcm_pos >> 16) + 1U < v->pcm_len && v->pcm_amp >= SILENCE);
    float synth_amp = (ta > na) ? ta : na;
    v->amp = (has_pcm && v->pcm_amp * 0.5f > synth_amp) ? (v->pcm_amp * 0.5f) : synth_amp;

    if (!has_pcm && v->tone_amp * v->tone_lvl < SILENCE && v->noise_amp * v->noise_lvl < SILENCE) {
        v->active = false;
        return;
    }

    float drum_scale = ch_gain * (float)(32767 * s_synth_drum_q8 / 256);
    int32_t tq = (int32_t)(ta * drum_scale);
    int32_t nq = (int32_t)(na * drum_scale);
    int32_t pq = has_pcm ? (int32_t)(v->pcm_amp * v->vel_gain * drum_scale * 0.65f) : 0;
    if (has_pcm) {
        tq = (tq * 3) >> 3;
        nq = (nq * 3) >> 3;
    }

    int32_t pl = (int32_t)(c->pan_l * 32767.0f);
    int32_t pr = (int32_t)(c->pan_r * 32767.0f);
    uint32_t inc = float_to_phase(v->tone_f * 4294967296.0f / s_sample_rate);
    uint32_t pc = v->pc;
    uint32_t rng = v->rng;
    int32_t prev = v->noise_prev;
    bool hp = v->hp;
    const int16_t *pcm = v->pcm_data;
    uint32_t pcm_len = v->pcm_len;
    uint32_t pcm_pos = v->pcm_pos;
    uint32_t pcm_step = v->pcm_step;

    for (size_t i = 0; i < len; i++) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        int32_t nz = (int32_t)(int16_t)(rng >> 16);
        if (hp) {
            int32_t d = nz - prev;
            prev = nz;
            nz = d >> 1;
        }
        int32_t tone = s_sine[pc >> SINE_SHIFT];
        pc += inc;
        int32_t o = ((tone * tq) >> 15) + ((nz * nq) >> 15);

        if (pq > 0) {
            uint32_t idx = pcm_pos >> 16;
            if (idx + 1U < pcm_len) {
                int32_t frac = (int32_t)((pcm_pos >> 8) & 0xFFU);
                int32_t s0 = pcm[idx];
                int32_t s1 = pcm[idx + 1U];
                int32_t pval = (s0 * (256 - frac) + s1 * frac) >> 8;
                o += (pval * pq) >> 15;
                pcm_pos += pcm_step;
            }
        }
        bl[i] += (o * pl) >> 15;
        br[i] += (o * pr) >> 15;
    }

    v->pc = pc;
    v->rng = rng;
    v->noise_prev = prev;
    v->pcm_pos = pcm_pos;
}

#define GAIN_SLEW   0.4f        /* share of the gap to a new channel gain closed per block */

static void render_chunk(int16_t *out, size_t frames)
{
    float ch_gain[FM_MIDI_CHANNELS];
    float ch_ratio[FM_MIDI_CHANNELS];
    uint32_t dirty = 0;

    if (s_flush_request != s_flush_seen) {
        /* A song change: drop what the old song left in the ring, but not events the new
         * song may already have pushed behind the reset. */
        s_flush_seen = s_flush_request;
        if ((int32_t)(s_flush_to - s_ev_tail) > 0) {
            s_ev_tail = s_flush_to;
        }
        for (int i = 0; i < FM_MAX_VOICES; i++) {
            voice_fast_release(&s_voices[i]);
        }
    }
    drain_events(s_frame_clock + EVENT_ROUND_FRAMES);

    for (int ch = 0; ch < FM_MIDI_CHANNELS; ch++) {
        midi_channel_state_t *c = &s_channels[ch];
        float v = (float)c->volume / 127.0f;
        float e = (float)c->expression / 127.0f;
        float target = v * v * e * e;

        if (ch == s_melody_channel) {
            target *= (float)s_melody_percent * 0.01f;
        }
        if (c->gain_prev < 0.0f) {
            c->gain_prev = target;
        } else {
            c->gain_prev += (target - c->gain_prev) * GAIN_SLEW;
        }
        ch_gain[ch] = c->gain_prev;

        if (c->pan != c->pan_cached) {
            float angle = ((float)c->pan / 127.0f) * 1.5707963f;
            c->pan_l = cosf(angle);
            c->pan_r = sinf(angle);
            c->pan_cached = c->pan;
        }
        if (c->bend != 0) {
            float range = (float)c->bend_semis + (float)c->bend_cents * 0.01f;
            ch_ratio[ch] = exp2f(((float)c->bend / 8192.0f) * range / 12.0f);
        } else {
            ch_ratio[ch] = 1.0f;
        }
    }

    s_lfo_phase += VIBRATO_HZ * s_sub_sec;
    if (s_lfo_phase >= 1.0f) {
        s_lfo_phase -= 1.0f;
    }
    float lfo = (float)s_sine[(uint32_t)(s_lfo_phase * 4294967296.0f) >> SINE_SHIFT] / 32767.0f;

    memset(s_acc_l, 0, frames * sizeof(int32_t));
    memset(s_acc_r, 0, frames * sizeof(int32_t));
    bool fx_on = dsp_effects_active();
    if (fx_on) {
        memset(s_rev, 0, frames * sizeof(int32_t));
        memset(s_cho, 0, frames * sizeof(int32_t));
    }

    uint32_t active_poly = 0;
    for (int i = 0; i < FM_MAX_VOICES; i++) {
        if (s_voices[i].active) active_poly++;
    }

    for (int i = 0; i < FM_MAX_VOICES; i++) {
        fm_voice_t *v = &s_voices[i];
        if (!v->active) {
            continue;
        }
        uint8_t ch = v->channel;
        const midi_channel_state_t *c = &s_channels[ch];

        if ((dirty & (1U << ch)) == 0U) {
            memset(s_bus_l[ch], 0, frames * sizeof(int32_t));
            memset(s_bus_r[ch], 0, frames * sizeof(int32_t));
            dirty |= 1U << ch;
        }
        if (v->is_drum) {
            render_drum_sub(v, frames, ch_gain[ch], c, s_bus_l[ch], s_bus_r[ch]);
        } else {
            render_fm_sub(v, frames, ch_gain[ch], ch_ratio[ch], lfo, c, s_bus_l[ch], s_bus_r[ch], active_poly);
        }
    }

    /* Automatic sidechain ducking: when the lead melody channel is actively singing,
     * accompaniment channels (except drums) smoothly duck by ~2.5 dB so the vocal line stands out. */
    bool melody_singing = false;
    if (s_melody_channel >= 0 && (dirty & (1U << s_melody_channel)) != 0U) {
        for (int i = 0; i < FM_MAX_VOICES; i++) {
            if (s_voices[i].active && s_voices[i].channel == (uint8_t)s_melody_channel && s_voices[i].amp > 0.03f) {
                melody_singing = true;
                break;
            }
        }
    }
    static float s_duck_gain = 1.0f;
    float duck_target = (s_melody_channel >= 0) ? (melody_singing ? 0.75f : 1.0f) : 1.0f;
    s_duck_gain = (s_melody_channel >= 0) ? (s_duck_gain + (duck_target - s_duck_gain) * 0.25f) : 1.0f;
    int32_t duck_q12 = (int32_t)(s_duck_gain * 4096.0f);

    for (int ch = 0; ch < FM_MIDI_CHANNELS; ch++) {
        if ((dirty & (1U << ch)) == 0U) {
            continue;
        }
        const int32_t *bl = s_bus_l[ch];
        const int32_t *br = s_bus_r[ch];
        int32_t rs = fx_on ? ((int32_t)s_channels[ch].reverb * 2048) / 127 : 0;
        int32_t cs = fx_on ? ((int32_t)s_channels[ch].chorus * 2048) / 127 : 0;
        bool duck_ch = (s_melody_channel >= 0 && ch != s_melody_channel && ch != FM_DRUM_CHANNEL && duck_q12 < 4088);

        for (size_t i = 0; i < frames; i++) {
            int32_t l = duck_ch ? ((bl[i] * duck_q12) >> 12) : bl[i];
            int32_t r = duck_ch ? ((br[i] * duck_q12) >> 12) : br[i];
            s_acc_l[i] += l;
            s_acc_r[i] += r;
            if (rs > 0) {
                s_rev[i] += (int32_t)(((int64_t)(l + r) * rs) >> 12);
            }
            if (cs > 0) {
                s_cho[i] += (int32_t)(((int64_t)(l + r) * cs) >> 12);
            }
        }
    }

    if (fx_on) {
        dsp_effects_process(s_rev, s_cho, s_acc_l, s_acc_r, frames);
    }
    dsp_master_process(s_acc_l, s_acc_r, out, frames, s_synth_master_q8);
    s_frame_clock += (uint32_t)frames;
}

void yamaha_fm_synth_render(int16_t *buffer, size_t num_samples)
{
    if (!buffer || num_samples == 0) {
        return;
    }

    size_t frames = num_samples / 2U;
    size_t done = 0;

    while (done < frames) {
        size_t n = frames - done;
        if (n > SUB_FRAMES) {
            n = SUB_FRAMES;
        }
        render_chunk(buffer + done * 2U, n);
        done += n;
    }
}

static const midi_synth_callbacks_t s_synth_callbacks = {
    .note_on = yamaha_fm_note_on,
    .note_off = yamaha_fm_note_off,
    .program_change = yamaha_fm_program_change,
    .control_change = yamaha_fm_control_change,
    .pitch_bend = yamaha_fm_pitch_bend,
    .all_notes_off = yamaha_fm_all_notes_off,
    .song_clock = yamaha_fm_song_time_anchor,
    .event_time = yamaha_fm_song_time_event
};

const midi_synth_callbacks_t *yamaha_fm_get_callbacks(void)
{
    return &s_synth_callbacks;
}
