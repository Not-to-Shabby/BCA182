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
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/irq.h>

#define SINE_BITS           10U
#define SINE_SIZE           (1U << SINE_BITS)
#define SINE_SHIFT          (32U - SINE_BITS)
#define SUB_FRAMES          32U             /* envelope / pitch / gain update interval */
#define MAX_BLOCK_FRAMES    128U
#define PHASE_PER_RAD       683565275.6f    /* 2^32 / (2 pi) */
#define MOD_IDX_UNIT        20860.76f       /* phase per radian per unit of a Q15 modulator */
#ifndef SYNTH_MASTER_Q8
#define SYNTH_MASTER_Q8     256             /* master gain in 1/256 steps, applied before the limiter */
#endif
#ifndef SYNTH_DRUM_Q8
#define SYNTH_DRUM_Q8       166             /* drum level against the melodic voices, in 1/256 steps */
#endif
#define GAIN_Q              8               /* extra fractional bits in the per-sample gain ramp */
#define GAIN_ONE            (32767 << GAIN_Q)
#define SILENCE             0.0015f         /* about -56 dB */
#define VIBRATO_HZ          5.5f
#define SEMITONE_LINEAR     0.05776f        /* 2^(1/12) - 1, small-signal pitch change per semitone */
#define EVENT_RING_SIZE     128U
#define LIMIT_KNEE          20000
#define MAX_BANDWIDTH_HZ    11000.0f

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

    /* percussion voice */
    float tone_f, tone_f1, tone_fk;
    float tone_amp, tone_k, tone_lvl;
    float noise_amp, noise_k, noise_lvl;
    bool hp;
    int32_t noise_prev;
    uint32_t rng;
} fm_voice_t;

typedef struct {
    uint8_t program, volume, expression, pan, mod;
    uint8_t rpn_msb, rpn_lsb, bend_semis, bend_cents;
    int16_t bend;
    bool pedal;
    float gain_prev;                /* render-thread smoothing state */
    uint8_t pan_cached;
    float pan_l, pan_r;
} midi_channel_state_t;

typedef struct {
    uint8_t type_ch;                /* high nibble event type, low nibble channel */
    uint8_t note, vel, prog;
} synth_event_t;

enum { EV_NOTE_ON = 1, EV_NOTE_OFF, EV_PEDAL_UP, EV_CH_OFF, EV_ALL_OFF };

static int16_t s_sine[SINE_SIZE];
static float s_note_inc[128];
static float s_sample_rate = 44100.0f;
static float s_sub_sec;
static float s_fast_rel_k;
static float s_lfo_phase;
static uint32_t s_age_counter;
static uint32_t s_steals;
static uint32_t s_steals_audible;
static volatile uint32_t s_dropped_events;

static fm_voice_t s_voices[FM_MAX_VOICES];
static midi_channel_state_t s_channels[FM_MIDI_CHANNELS];
static int32_t s_acc_l[MAX_BLOCK_FRAMES];
static int32_t s_acc_r[MAX_BLOCK_FRAMES];

static synth_event_t s_events[EVENT_RING_SIZE];
static volatile uint32_t s_ev_head;
static volatile uint32_t s_ev_tail;

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

static drum_recipe_t make_recipe(uint8_t note)
{
    static const drum_recipe_t KICK  = {150.0f, 48.0f, 0.030f, 0.95f, 0.11f, 0.10f, 0.008f, true,  false};
    static const drum_recipe_t STICK = {1700.0f, 1500.0f, 0.02f, 0.45f, 0.025f, 0.25f, 0.02f, true, false};
    static const drum_recipe_t SNARE = {230.0f, 175.0f, 0.03f, 0.50f, 0.10f, 0.42f, 0.16f, false, false};
    static const drum_recipe_t CLAP  = {0.0f, 0.0f, 0.05f, 0.0f, 0.10f, 0.55f, 0.13f, false, false};
    static const drum_recipe_t HAT_C = {0.0f, 0.0f, 0.05f, 0.0f, 0.10f, 0.40f, 0.035f, true, false};
    static const drum_recipe_t HAT_O = {0.0f, 0.0f, 0.05f, 0.0f, 0.10f, 0.40f, 0.30f, true, true};
    static const drum_recipe_t CRASH = {0.0f, 0.0f, 0.05f, 0.0f, 0.10f, 0.42f, 0.90f, true, false};
    static const drum_recipe_t RIDE  = {3300.0f, 3300.0f, 0.05f, 0.15f, 0.45f, 0.22f, 0.55f, true, false};
    static const drum_recipe_t BELL  = {3300.0f, 3300.0f, 0.05f, 0.35f, 0.80f, 0.10f, 0.40f, true, false};
    static const drum_recipe_t TAMB  = {0.0f, 0.0f, 0.05f, 0.0f, 0.10f, 0.40f, 0.09f, true, false};
    static const drum_recipe_t COWB  = {800.0f, 800.0f, 0.05f, 0.50f, 0.20f, 0.05f, 0.02f, true, false};
    static const drum_recipe_t TOM   = {0.0f, 0.0f, 0.04f, 0.90f, 0.16f, 0.06f, 0.012f, true, false};
    static const drum_recipe_t CONGA = {0.0f, 0.0f, 0.02f, 0.80f, 0.09f, 0.10f, 0.01f, true, false};
    static const drum_recipe_t CLICK = {1000.0f, 900.0f, 0.02f, 0.50f, 0.04f, 0.10f, 0.01f, true, false};
    static const drum_recipe_t SHAKE = {0.0f, 0.0f, 0.05f, 0.0f, 0.10f, 0.30f, 0.05f, true, false};
    static const drum_recipe_t GUIRO = {0.0f, 0.0f, 0.05f, 0.0f, 0.10f, 0.30f, 0.10f, false, false};
    static const drum_recipe_t WHIS  = {2400.0f, 2400.0f, 0.05f, 0.35f, 0.18f, 0.0f, 0.02f, false, false};
    static const drum_recipe_t TRI   = {4200.0f, 4200.0f, 0.05f, 0.30f, 0.60f, 0.05f, 0.10f, true, false};
    drum_recipe_t r;

    switch (note) {
    case 35: case 36: return KICK;
    case 37: return STICK;
    case 38: case 40: return SNARE;
    case 39: return CLAP;
    case 42: case 44: return HAT_C;
    case 46: return HAT_O;
    case 49: case 52: case 55: case 57: return CRASH;
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
        return r;
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
    case 80: case 81: return TRI;
    default: return SHAKE;
    }
}

/* -------------------------------------------------------------------------- */
/* Event ring                                                                 */
/* -------------------------------------------------------------------------- */
static void push_event(uint8_t type, uint8_t ch, uint8_t note, uint8_t vel, uint8_t prog)
{
    unsigned int key = irq_lock();
    uint32_t next = (s_ev_head + 1U) % EVENT_RING_SIZE;

    if (next == s_ev_tail) {
        s_dropped_events++;
    } else {
        s_events[s_ev_head].type_ch = (uint8_t)((type << 4) | (ch & 0x0FU));
        s_events[s_ev_head].note = note;
        s_events[s_ev_head].vel = vel;
        s_events[s_ev_head].prog = prog;
        s_ev_head = next;
    }
    irq_unlock(key);
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
    const fm_patch_t *p = &PATCHES[PROGRAM_PATCH[prog & 127U]];

    for (int i = 0; i < FM_MAX_VOICES; i++) {
        fm_voice_t *o = &s_voices[i];
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
    v->level = p->level;
    v->vib = p->vib;
    {
        float vg = (float)vel / 127.0f;
        v->vel_gain = vg * sqrtf(vg);
    }

    v->base_c = s_note_inc[note] * p->ratio_c;
    v->base_m = s_note_inc[note] * p->ratio_m;
    v->fb_u = float_to_phase(p->fb * MOD_IDX_UNIT);

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
    drum_recipe_t r = make_recipe(note);

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
    }
}

static void reset_state(void)
{
    memset(s_voices, 0, sizeof(s_voices));
    reset_channels();
    s_age_counter = 0;
    s_lfo_phase = 0.0f;
}

static void apply_event(const synth_event_t *e)
{
    uint8_t type = (uint8_t)(e->type_ch >> 4);
    uint8_t ch = (uint8_t)(e->type_ch & 0x0FU);

    switch (type) {
    case EV_NOTE_ON:
        if (ch == FM_DRUM_CHANNEL) {
            drum_note_on(e->note, e->vel);
        } else {
            fm_note_on(ch, e->note, e->vel, e->prog);
        }
        break;
    case EV_NOTE_OFF:
        apply_note_off(ch, e->note);
        break;
    case EV_PEDAL_UP:
        apply_pedal_up(ch);
        break;
    case EV_CH_OFF:
        for (int i = 0; i < FM_MAX_VOICES; i++) {
            if (s_voices[i].channel == ch) {
                voice_fast_release(&s_voices[i]);
            }
        }
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

static void drain_events(void)
{
    while (s_ev_tail != s_ev_head) {
        synth_event_t e = s_events[s_ev_tail];
        s_ev_tail = (s_ev_tail + 1U) % EVENT_RING_SIZE;
        apply_event(&e);
    }
}

/* -------------------------------------------------------------------------- */
/* Public control interface (any context)                                     */
/* -------------------------------------------------------------------------- */
void yamaha_fm_synth_init(uint32_t sample_rate)
{
    s_sample_rate = (sample_rate > 0U) ? (float)sample_rate : 44100.0f;
    s_sub_sec = (float)SUB_FRAMES / s_sample_rate;
    s_fast_rel_k = expf(-s_sub_sec / 0.006f);

    for (unsigned i = 0; i < SINE_SIZE; i++) {
        s_sine[i] = (int16_t)lrintf(sinf(2.0f * 3.14159265f * (float)i / (float)SINE_SIZE) * 32767.0f);
    }
    for (int n = 0; n < 128; n++) {
        float freq = 440.0f * exp2f(((float)n - 69.0f) / 12.0f);
        s_note_inc[n] = freq * 4294967296.0f / s_sample_rate;
    }

    s_ev_head = 0;
    s_ev_tail = 0;
    s_dropped_events = 0;
    s_steals = 0;
    s_steals_audible = 0;
    reset_state();
}

void yamaha_fm_synth_reset(void)
{
    /* Call while the sequencer is not delivering events (between songs). */
    reset_channels();
    push_event(EV_ALL_OFF, 0, 0, 0, 0);
}

void yamaha_fm_note_on(uint8_t channel, uint8_t note, uint8_t velocity)
{
    if (channel >= FM_MIDI_CHANNELS || note >= 128U) {
        return;
    }
    if (velocity == 0U) {
        yamaha_fm_note_off(channel, note, 0);
        return;
    }
    push_event(EV_NOTE_ON, channel, note, velocity, s_channels[channel].program);
}

void yamaha_fm_note_off(uint8_t channel, uint8_t note, uint8_t velocity)
{
    ARG_UNUSED(velocity);
    if (channel >= FM_MIDI_CHANNELS || note >= 128U || channel == FM_DRUM_CHANNEL) {
        return;
    }
    push_event(EV_NOTE_OFF, channel, note, 0, 0);
}

void yamaha_fm_program_change(uint8_t channel, uint8_t program)
{
    if (channel < FM_MIDI_CHANNELS) {
        s_channels[channel].program = (uint8_t)(program & 127U);
    }
}

void yamaha_fm_control_change(uint8_t channel, uint8_t control, uint8_t value)
{
    if (channel >= FM_MIDI_CHANNELS) {
        return;
    }
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
            push_event(EV_PEDAL_UP, channel, 0, 0, 0);
        }
        c->pedal = down;
        break;
    }
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
        push_event(EV_CH_OFF, channel, 0, 0, 0);
        break;
    case 121:
        if (c->pedal) {
            push_event(EV_PEDAL_UP, channel, 0, 0, 0);
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

void yamaha_fm_pitch_bend(uint8_t channel, uint16_t bend)
{
    if (channel < FM_MIDI_CHANNELS) {
        s_channels[channel].bend = (int16_t)((int)(bend & 0x3FFFU) - 8192);
    }
}

void yamaha_fm_all_notes_off(void)
{
    push_event(EV_ALL_OFF, 0, 0, 0, 0);
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

static void render_fm_sub(fm_voice_t *v, size_t off, size_t len, float ch_gain, float ratio,
                          float lfo, const midi_channel_state_t *c)
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
    int32_t dgl = (gl1 - v->gl) / (int32_t)len;
    int32_t dgr = (gr1 - v->gr) / (int32_t)len;
    int32_t gl = v->gl, gr = v->gr;
    uint32_t pc = v->pc, pm = v->pm;
    int32_t y = v->fb_prev;
    uint32_t fbu = v->fb_u;

    if (v->additive) {
        int32_t mix = (int32_t)(v->idx * 32767.0f);
        for (size_t i = 0; i < len; i++) {
            int32_t m = s_sine[(pm + (uint32_t)y * fbu) >> SINE_SHIFT];
            y = m;
            pm += inc_m;
            int32_t cs = s_sine[pc >> SINE_SHIFT] + ((m * mix) >> 15);
            pc += inc_c;
            gl += dgl;
            gr += dgr;
            s_acc_l[off + i] += (cs * (gl >> GAIN_Q)) >> 15;
            s_acc_r[off + i] += (cs * (gr >> GAIN_Q)) >> 15;
        }
    } else {
        uint32_t iu = float_to_phase(v->idx * MOD_IDX_UNIT);
        for (size_t i = 0; i < len; i++) {
            int32_t m = s_sine[(pm + (uint32_t)y * fbu) >> SINE_SHIFT];
            y = m;
            pm += inc_m;
            int32_t cs = s_sine[(pc + (uint32_t)m * iu) >> SINE_SHIFT];
            pc += inc_c;
            gl += dgl;
            gr += dgr;
            s_acc_l[off + i] += (cs * (gl >> GAIN_Q)) >> 15;
            s_acc_r[off + i] += (cs * (gr >> GAIN_Q)) >> 15;
        }
    }

    v->pc = pc;
    v->pm = pm;
    v->fb_prev = y;
    v->gl = gl1;
    v->gr = gr1;
}

static void render_drum_sub(fm_voice_t *v, size_t off, size_t len, float ch_gain,
                            const midi_channel_state_t *c)
{
    v->tone_f = v->tone_f1 + (v->tone_f - v->tone_f1) * v->tone_fk;
    float ta = v->tone_amp * v->tone_lvl * v->vel_gain;
    float na = v->noise_amp * v->noise_lvl * v->vel_gain;
    v->tone_amp *= v->tone_k;
    v->noise_amp *= v->noise_k;
    v->amp = (ta > na) ? ta : na;

    if (v->tone_amp * v->tone_lvl < SILENCE && v->noise_amp * v->noise_lvl < SILENCE) {
        v->active = false;
        return;
    }

    int32_t tq = (int32_t)(ta * ch_gain * (float)(32767 * SYNTH_DRUM_Q8 / 256));
    int32_t nq = (int32_t)(na * ch_gain * (float)(32767 * SYNTH_DRUM_Q8 / 256));
    int32_t pl = (int32_t)(c->pan_l * 32767.0f);
    int32_t pr = (int32_t)(c->pan_r * 32767.0f);
    uint32_t inc = float_to_phase(v->tone_f * 4294967296.0f / s_sample_rate);
    uint32_t pc = v->pc;
    uint32_t rng = v->rng;
    int32_t prev = v->noise_prev;
    bool hp = v->hp;

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
        s_acc_l[off + i] += (o * pl) >> 15;
        s_acc_r[off + i] += (o * pr) >> 15;
    }

    v->pc = pc;
    v->rng = rng;
    v->noise_prev = prev;
}

static inline int16_t soft_limit(int32_t x)
{
    const int32_t range = 32767 - LIMIT_KNEE;
    int32_t a = (x < 0) ? -x : x;

    if (a > LIMIT_KNEE) {
        int32_t d = a - LIMIT_KNEE;
        if (d > 131072) {
            d = 131072;
        }
        a = LIMIT_KNEE + (range * d) / (d + range);
    }
    return (int16_t)((x < 0) ? -a : a);
}

static void render_block(int16_t *out, size_t frames)
{
    float ch_target[FM_MIDI_CHANNELS];
    float ch_ratio[FM_MIDI_CHANNELS];
    unsigned subs = (unsigned)((frames + SUB_FRAMES - 1U) / SUB_FRAMES);

    memset(s_acc_l, 0, frames * sizeof(int32_t));
    memset(s_acc_r, 0, frames * sizeof(int32_t));

    for (int ch = 0; ch < FM_MIDI_CHANNELS; ch++) {
        midi_channel_state_t *c = &s_channels[ch];
        float v = (float)c->volume / 127.0f;
        float e = (float)c->expression / 127.0f;
        ch_target[ch] = v * v * e * e;
        if (c->gain_prev < 0.0f) {
            c->gain_prev = ch_target[ch];
        }
        if (c->pan != c->pan_cached) {
            float angle = ((float)c->pan / 127.0f) * 1.5707963f;
            c->pan_l = cosf(angle);
            c->pan_r = sinf(angle);
            c->pan_cached = c->pan;
        }
    }

    for (unsigned s = 0; s < subs; s++) {
        size_t off = (size_t)s * SUB_FRAMES;
        size_t len = frames - off;
        float frac = (float)(s + 1U) / (float)subs;

        if (len > SUB_FRAMES) {
            len = SUB_FRAMES;
        }
        s_lfo_phase += VIBRATO_HZ * s_sub_sec;
        if (s_lfo_phase >= 1.0f) {
            s_lfo_phase -= 1.0f;
        }
        float lfo = (float)s_sine[(uint32_t)(s_lfo_phase * 4294967296.0f) >> SINE_SHIFT] / 32767.0f;

        for (int ch = 0; ch < FM_MIDI_CHANNELS; ch++) {
            const midi_channel_state_t *c = &s_channels[ch];
            if (c->bend != 0) {
                float range = (float)c->bend_semis + (float)c->bend_cents * 0.01f;
                ch_ratio[ch] = exp2f(((float)c->bend / 8192.0f) * range / 12.0f);
            } else {
                ch_ratio[ch] = 1.0f;
            }
        }

        for (int i = 0; i < FM_MAX_VOICES; i++) {
            fm_voice_t *v = &s_voices[i];
            if (!v->active) {
                continue;
            }
            const midi_channel_state_t *c = &s_channels[v->channel];
            float g = c->gain_prev + (ch_target[v->channel] - c->gain_prev) * frac;

            if (v->is_drum) {
                render_drum_sub(v, off, len, g, c);
            } else {
                render_fm_sub(v, off, len, g, ch_ratio[v->channel], lfo, c);
            }
        }
    }

    for (int ch = 0; ch < FM_MIDI_CHANNELS; ch++) {
        s_channels[ch].gain_prev = ch_target[ch];
    }
    for (size_t i = 0; i < frames; i++) {
        out[i * 2U] = soft_limit((s_acc_l[i] * SYNTH_MASTER_Q8) >> 8);
        out[i * 2U + 1U] = soft_limit((s_acc_r[i] * SYNTH_MASTER_Q8) >> 8);
    }
}

void yamaha_fm_synth_render(int16_t *buffer, size_t num_samples)
{
    if (!buffer || num_samples == 0) {
        return;
    }
    drain_events();

    size_t frames = num_samples / 2U;
    size_t done = 0;

    while (done < frames) {
        size_t n = frames - done;
        if (n > MAX_BLOCK_FRAMES) {
            n = MAX_BLOCK_FRAMES;
        }
        render_block(buffer + done * 2U, n);
        done += n;
    }
}

static const midi_synth_callbacks_t s_synth_callbacks = {
    .note_on = yamaha_fm_note_on,
    .note_off = yamaha_fm_note_off,
    .program_change = yamaha_fm_program_change,
    .control_change = yamaha_fm_control_change,
    .pitch_bend = yamaha_fm_pitch_bend,
    .all_notes_off = yamaha_fm_all_notes_off
};

const midi_synth_callbacks_t *yamaha_fm_get_callbacks(void)
{
    return &s_synth_callbacks;
}
