/**
 * @file yamaha_fm_synth.c
 * @brief High-efficiency 18-Voice Yamaha OPL/DX FM Synthesizer with General MIDI
 *        and Rhythm Kit for RT-Spark (STM32F407) and Everest ES8388 3.5mm Output.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include "yamaha_fm_synth.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <zephyr/kernel.h>

#define SINE_TABLE_SIZE     512
#define SINE_TABLE_MASK     (SINE_TABLE_SIZE - 1)

#define ENV_MAX             (1 << 30) /* 1,073,741,824 */

static int16_t s_sine_table[SINE_TABLE_SIZE];
static uint32_t s_sample_rate = 44100;

/* Note to phase increment lookup table (128 notes) */
static uint32_t s_note_phase_inc[128];

/* Envelope stages */
typedef enum {
    ENV_IDLE = 0,
    ENV_ATTACK,
    ENV_DECAY,
    ENV_SUSTAIN,
    ENV_RELEASE
} env_state_t;

/* 2-Operator FM Patch Parameter Definition */
typedef struct {
    uint8_t mult_mod;       /* Modulator multiplier (x1, x2, ...) */
    uint8_t mult_car;       /* Carrier multiplier */
    uint16_t mod_index;     /* Peak modulation index (scaled 0-4096) */
    uint32_t attack_rate;   /* Attack rate increment */
    uint32_t decay_rate;    /* Decay rate decrement */
    uint32_t sustain_level; /* Sustain level (0 to ENV_MAX) */
    uint32_t release_rate;  /* Release rate decrement */
    uint8_t feedback;       /* Feedback shift */
    bool is_drum;
} fm_patch_t;

/* Active Voice State */
typedef struct {
    bool active;
    uint8_t midi_channel;
    uint8_t note;
    uint8_t velocity;
    uint32_t age;           /* Tick age for voice stealing */

    /* Oscillators */
    uint32_t phase_car;
    uint32_t phase_inc_car;
    uint32_t phase_mod;
    uint32_t phase_inc_mod;
    int16_t last_mod_out;

    /* Envelopes */
    env_state_t env_state;
    uint32_t env_level;     /* 0 to ENV_MAX */

    /* Drum specific */
    bool is_drum;
    uint16_t drum_pitch_decay;
    uint32_t noise_lfsr;

    const fm_patch_t *patch;
} fm_voice_t;

/* Channel State */
typedef struct {
    uint8_t program;
    uint8_t volume;
    uint8_t pan;            /* 0 = Left, 64 = Center, 127 = Right */
    int16_t pitch_bend;     /* -8192 to +8191 */
    bool sustain_pedal;
} midi_channel_state_t;

static fm_voice_t s_voices[FM_MAX_VOICES];
static midi_channel_state_t s_channels[FM_MIDI_CHANNELS];
static uint32_t s_voice_age_counter = 0;
static struct k_mutex s_synth_mutex;

/* -------------------------------------------------------------------------- */
/* General MIDI Patch Definitions                                             */
/* -------------------------------------------------------------------------- */
static const fm_patch_t PATCH_GRAND_PIANO = {
    .mult_mod = 2, .mult_car = 1, .mod_index = 2800,
    .attack_rate = 6000000, .decay_rate = 15000, .sustain_level = 750000000, .release_rate = 45000,
    .feedback = 2, .is_drum = false
};

static const fm_patch_t PATCH_ELEC_PIANO = { /* DX7 Glassy E. Piano */
    .mult_mod = 14, .mult_car = 1, .mod_index = 3800,
    .attack_rate = 8000000, .decay_rate = 25000, .sustain_level = 700000000, .release_rate = 40000,
    .feedback = 3, .is_drum = false
};

static const fm_patch_t PATCH_ORGAN = {
    .mult_mod = 1, .mult_car = 1, .mod_index = 1200,
    .attack_rate = 5000000, .decay_rate = 5000, .sustain_level = 950000000, .release_rate = 60000,
    .feedback = 1, .is_drum = false
};

static const fm_patch_t PATCH_GUITAR = {
    .mult_mod = 3, .mult_car = 1, .mod_index = 2400,
    .attack_rate = 6000000, .decay_rate = 20000, .sustain_level = 500000000, .release_rate = 50000,
    .feedback = 2, .is_drum = false
};

static const fm_patch_t PATCH_BASS = {
    .mult_mod = 1, .mult_car = 1, .mod_index = 2200,
    .attack_rate = 6000000, .decay_rate = 15000, .sustain_level = 800000000, .release_rate = 60000,
    .feedback = 3, .is_drum = false
};

static const fm_patch_t PATCH_STRINGS = {
    .mult_mod = 1, .mult_car = 1, .mod_index = 900,
    .attack_rate = 200000, .decay_rate = 10000, .sustain_level = 900000000, .release_rate = 35000,
    .feedback = 1, .is_drum = false
};

static const fm_patch_t PATCH_BRASS = {
    .mult_mod = 1, .mult_car = 1, .mod_index = 3200,
    .attack_rate = 800000, .decay_rate = 15000, .sustain_level = 850000000, .release_rate = 50000,
    .feedback = 2, .is_drum = false
};

static const fm_patch_t PATCH_REED_SAX = {
    .mult_mod = 2, .mult_car = 1, .mod_index = 2600,
    .attack_rate = 1000000, .decay_rate = 12000, .sustain_level = 850000000, .release_rate = 50000,
    .feedback = 2, .is_drum = false
};

static const fm_patch_t PATCH_FLUTE = {
    .mult_mod = 1, .mult_car = 1, .mod_index = 600,
    .attack_rate = 500000, .decay_rate = 8000, .sustain_level = 800000000, .release_rate = 45000,
    .feedback = 0, .is_drum = false
};

static const fm_patch_t PATCH_SYNTH_LEAD = {
    .mult_mod = 3, .mult_car = 2, .mod_index = 3400,
    .attack_rate = 4000000, .decay_rate = 15000, .sustain_level = 850000000, .release_rate = 50000,
    .feedback = 4, .is_drum = false
};

/* Rhythm Kit Patches */
static const fm_patch_t PATCH_DRUM_KICK = {
    .mult_mod = 1, .mult_car = 1, .mod_index = 2800,
    .attack_rate = 10000000, .decay_rate = 75000, .sustain_level = 0, .release_rate = 100000,
    .feedback = 2, .is_drum = true
};

static const fm_patch_t PATCH_DRUM_SNARE = {
    .mult_mod = 5, .mult_car = 2, .mod_index = 3500,
    .attack_rate = 10000000, .decay_rate = 90000, .sustain_level = 0, .release_rate = 100000,
    .feedback = 4, .is_drum = true
};

static const fm_patch_t PATCH_DRUM_HIHAT = {
    .mult_mod = 11, .mult_car = 7, .mod_index = 3800,
    .attack_rate = 10000000, .decay_rate = 250000, .sustain_level = 0, .release_rate = 250000,
    .feedback = 5, .is_drum = true
};

static const fm_patch_t PATCH_DRUM_CYMBAL = {
    .mult_mod = 9, .mult_car = 5, .mod_index = 3600,
    .attack_rate = 10000000, .decay_rate = 18000, .sustain_level = 0, .release_rate = 25000,
    .feedback = 5, .is_drum = true
};

static const fm_patch_t PATCH_DRUM_TOM = {
    .mult_mod = 1, .mult_car = 1, .mod_index = 1800,
    .attack_rate = 10000000, .decay_rate = 55000, .sustain_level = 0, .release_rate = 80000,
    .feedback = 1, .is_drum = true
};

static const fm_patch_t* get_patch_for_program(uint8_t prog)
{
    if (prog < 8) {
        if (prog == 4 || prog == 5) return &PATCH_ELEC_PIANO;
        return &PATCH_GRAND_PIANO;
    } else if (prog < 16) {
        return &PATCH_GRAND_PIANO; /* Chromatic percussion */
    } else if (prog < 24) {
        return &PATCH_ORGAN;
    } else if (prog < 32) {
        return &PATCH_GUITAR;
    } else if (prog < 40) {
        return &PATCH_BASS;
    } else if (prog < 56) {
        return &PATCH_STRINGS;
    } else if (prog < 64) {
        return &PATCH_BRASS;
    } else if (prog < 72) {
        return &PATCH_REED_SAX;
    } else if (prog < 80) {
        return &PATCH_FLUTE;
    } else {
        return &PATCH_SYNTH_LEAD;
    }
}

static const fm_patch_t* get_drum_patch(uint8_t note)
{
    if (note == 35 || note == 36) {
        return &PATCH_DRUM_KICK;
    } else if (note == 38 || note == 40) {
        return &PATCH_DRUM_SNARE;
    } else if (note == 42 || note == 44 || note == 46) {
        return &PATCH_DRUM_HIHAT;
    } else if (note == 49 || note == 51 || note == 57 || note == 59) {
        return &PATCH_DRUM_CYMBAL;
    } else {
        return &PATCH_DRUM_TOM;
    }
}

/* -------------------------------------------------------------------------- */
/* Initialization & Frequency Calculations                                   */
/* -------------------------------------------------------------------------- */
void yamaha_fm_synth_init(uint32_t sample_rate)
{
    k_mutex_init(&s_synth_mutex);
    s_sample_rate = (sample_rate > 0) ? sample_rate : 44100;

    /* Generate 512-entry sine table */
    for (int i = 0; i < SINE_TABLE_SIZE; i++) {
        double angle = (2.0 * 3.14159265358979323846 * i) / SINE_TABLE_SIZE;
        s_sine_table[i] = (int16_t)(sin(angle) * 32767.0);
    }

    /* Precompute MIDI note phase increments: f = 440 * 2^((note - 69) / 12) */
    for (int note = 0; note < 128; note++) {
        double freq = 440.0 * pow(2.0, (note - 69.0) / 12.0);
        uint32_t inc = (uint32_t)((freq * 4294967296.0) / (double)s_sample_rate);
        s_note_phase_inc[note] = inc;
    }

    yamaha_fm_synth_reset();
}

void yamaha_fm_synth_reset(void)
{
    k_mutex_lock(&s_synth_mutex, K_FOREVER);
    memset(s_voices, 0, sizeof(s_voices));

    for (int ch = 0; ch < FM_MIDI_CHANNELS; ch++) {
        s_channels[ch].program = 0;
        s_channels[ch].volume = 100;
        s_channels[ch].pan = 64; /* Center */
        s_channels[ch].pitch_bend = 0;
        s_channels[ch].sustain_pedal = false;
    }

    s_voice_age_counter = 0;
    k_mutex_unlock(&s_synth_mutex);
}

static inline int16_t lookup_sine(uint32_t phase)
{
    uint32_t idx = (phase >> 23) & SINE_TABLE_MASK;
    return s_sine_table[idx];
}

void yamaha_fm_note_on(uint8_t channel, uint8_t note, uint8_t velocity)
{
    if (velocity == 0) {
        yamaha_fm_note_off(channel, note, 0);
        return;
    }
    if (note >= 128 || channel >= FM_MIDI_CHANNELS) return;

    k_mutex_lock(&s_synth_mutex, K_FOREVER);

    /* Allocate voice: find free voice or steal oldest */
    int voice_idx = -1;
    uint32_t oldest_age = 0xFFFFFFFFU;
    int oldest_idx = 0;

    for (int i = 0; i < FM_MAX_VOICES; i++) {
        if (!s_voices[i].active) {
            voice_idx = i;
            break;
        }
        if (s_voices[i].age < oldest_age) {
            oldest_age = s_voices[i].age;
            oldest_idx = i;
        }
    }

    if (voice_idx < 0) {
        voice_idx = oldest_idx; /* Voice stealing */
    }

    fm_voice_t *v = &s_voices[voice_idx];
    v->active = true;
    v->midi_channel = channel;
    v->note = note;
    v->velocity = velocity;
    v->age = ++s_voice_age_counter;
    v->phase_car = 0;
    v->phase_mod = 0;
    v->last_mod_out = 0;
    v->env_state = ENV_ATTACK;
    v->env_level = 0;
    v->noise_lfsr = 0xACE1U;

    uint32_t base_inc = s_note_phase_inc[note];

    if (channel == FM_DRUM_CHANNEL) {
        v->is_drum = true;
        v->patch = get_drum_patch(note);
        v->drum_pitch_decay = 250;
        if (note == 35 || note == 36) {
            base_inc = s_note_phase_inc[36]; /* Bass drum pitch */
        }
    } else {
        v->is_drum = false;
        v->patch = get_patch_for_program(s_channels[channel].program);
        v->drum_pitch_decay = 0;
    }

    v->phase_inc_car = base_inc * v->patch->mult_car;
    v->phase_inc_mod = base_inc * v->patch->mult_mod;

    k_mutex_unlock(&s_synth_mutex);
}

void yamaha_fm_note_off(uint8_t channel, uint8_t note, uint8_t velocity)
{
    ARG_UNUSED(velocity);
    k_mutex_lock(&s_synth_mutex, K_FOREVER);

    for (int i = 0; i < FM_MAX_VOICES; i++) {
        if (s_voices[i].active && s_voices[i].midi_channel == channel && s_voices[i].note == note) {
            if (s_channels[channel].sustain_pedal) {
                /* Hold note while sustain pedal is down */
            } else {
                s_voices[i].env_state = ENV_RELEASE;
            }
        }
    }

    k_mutex_unlock(&s_synth_mutex);
}

void yamaha_fm_program_change(uint8_t channel, uint8_t program)
{
    if (channel < FM_MIDI_CHANNELS) {
        s_channels[channel].program = (program < 128) ? program : 0;
    }
}

void yamaha_fm_control_change(uint8_t channel, uint8_t control, uint8_t value)
{
    if (channel >= FM_MIDI_CHANNELS) return;

    if (control == 7) {
        s_channels[channel].volume = value;
    } else if (control == 10) {
        s_channels[channel].pan = value;
    } else if (control == 64) {
        s_channels[channel].sustain_pedal = (value >= 64);
        if (!s_channels[channel].sustain_pedal) {
            for (int i = 0; i < FM_MAX_VOICES; i++) {
                if (s_voices[i].active && s_voices[i].midi_channel == channel &&
                    s_voices[i].env_state == ENV_SUSTAIN) {
                    s_voices[i].env_state = ENV_RELEASE;
                }
            }
        }
    }
}

void yamaha_fm_pitch_bend(uint8_t channel, uint16_t bend)
{
    if (channel < FM_MIDI_CHANNELS) {
        s_channels[channel].pitch_bend = (int16_t)bend - 8192;
    }
}

void yamaha_fm_all_notes_off(void)
{
    k_mutex_lock(&s_synth_mutex, K_FOREVER);
    for (int i = 0; i < FM_MAX_VOICES; i++) {
        s_voices[i].active = false;
        s_voices[i].env_state = ENV_IDLE;
        s_voices[i].env_level = 0;
    }
    k_mutex_unlock(&s_synth_mutex);
}

uint8_t yamaha_fm_get_active_voice_count(void)
{
    uint8_t count = 0;
    for (int i = 0; i < FM_MAX_VOICES; i++) {
        if (s_voices[i].active && s_voices[i].env_state != ENV_IDLE && s_voices[i].env_level > (1 << 20)) {
            count++;
        }
    }
    return count;
}

/* -------------------------------------------------------------------------- */
/* Stereo PCM Audio Renderer for ES8388 I2S DMA Buffer                        */
/* -------------------------------------------------------------------------- */
void yamaha_fm_synth_render(int16_t *buffer, size_t num_samples)
{
    if (!buffer || num_samples == 0) return;

    memset(buffer, 0, num_samples * sizeof(int16_t));

    k_mutex_lock(&s_synth_mutex, K_FOREVER);

    size_t stereo_frames = num_samples / 2;

    for (int v_idx = 0; v_idx < FM_MAX_VOICES; v_idx++) {
        fm_voice_t *v = &s_voices[v_idx];
        if (!v->active || v->env_state == ENV_IDLE) continue;

        const fm_patch_t *p = v->patch;
        uint8_t ch = v->midi_channel;
        int32_t chan_vol = (int32_t)s_channels[ch].volume * (int32_t)v->velocity / 127;
        int32_t pan_l = 127 - (int32_t)s_channels[ch].pan;
        int32_t pan_r = (int32_t)s_channels[ch].pan;

        for (size_t f = 0; f < stereo_frames; f++) {
            /* 1. Advance 32-bit Fixed-Point ADSR Envelope Generator */
            switch (v->env_state) {
            case ENV_ATTACK:
                if (v->env_level + p->attack_rate >= ENV_MAX) {
                    v->env_level = ENV_MAX;
                    v->env_state = ENV_DECAY;
                } else {
                    v->env_level += p->attack_rate;
                }
                break;
            case ENV_DECAY:
                if (v->env_level <= p->decay_rate + p->sustain_level) {
                    v->env_level = p->sustain_level;
                    v->env_state = (p->sustain_level > 0) ? ENV_SUSTAIN : ENV_IDLE;
                    if (v->env_state == ENV_IDLE) v->active = false;
                } else {
                    v->env_level -= p->decay_rate;
                }
                break;
            case ENV_SUSTAIN:
                /* Hold steady at sustain level while key is pressed */
                break;
            case ENV_RELEASE:
                if (v->env_level <= p->release_rate) {
                    v->env_level = 0;
                    v->env_state = ENV_IDLE;
                    v->active = false;
                } else {
                    v->env_level -= p->release_rate;
                }
                break;
            default:
                break;
            }

            if (!v->active) break;

            /* 2. Modulator Phase & Synthesis */
            int32_t mod_feedback = 0;
            if (p->feedback > 0) {
                mod_feedback = ((int32_t)v->last_mod_out * (int32_t)p->feedback) << 14;
            }

            int16_t mod_sample = lookup_sine(v->phase_mod + (uint32_t)mod_feedback);
            v->last_mod_out = mod_sample;

            /* Modulation index scaling */
            int32_t mod_deviation = ((int32_t)mod_sample * (int32_t)p->mod_index) >> 1;

            /* 3. Carrier Phase & Output */
            uint32_t car_phase_mod = v->phase_car + (uint32_t)mod_deviation;
            int16_t car_sample = lookup_sine(car_phase_mod);

            /* Drum noise blend for snare / hi-hat / cymbal */
            if (v->is_drum && (v->note == 38 || v->note == 40 || v->note == 42 || v->note == 46 || v->note == 49)) {
                v->noise_lfsr = (v->noise_lfsr >> 1) ^ (-(v->noise_lfsr & 1U) & 0xB400U);
                int16_t noise = (int16_t)(v->noise_lfsr - 32768);
                car_sample = (int16_t)(((int32_t)car_sample + (int32_t)noise) / 2);
            }

            /* Scale 32-bit envelope to 15-bit (0-32767) */
            int32_t env_scaled = (int32_t)(v->env_level >> 15);
            int32_t voice_out = ((int32_t)car_sample * env_scaled) >> 15;
            voice_out = (voice_out * chan_vol) >> 7;

            /* Pitch decay for punchy kick drums */
            if (v->drum_pitch_decay > 0) {
                if (v->phase_inc_car > 10000000U) {
                    v->phase_inc_car -= (v->drum_pitch_decay << 14);
                }
            }

            /* Advance Phase Accumulators */
            v->phase_car += v->phase_inc_car;
            v->phase_mod += v->phase_inc_mod;

            /* Accumulate into Stereo Output with Soft Limiting */
            size_t left_idx  = f * 2;
            size_t right_idx = left_idx + 1;

            int32_t out_l = (int32_t)buffer[left_idx]  + ((voice_out * pan_l) >> 7);
            int32_t out_r = (int32_t)buffer[right_idx] + ((voice_out * pan_r) >> 7);

            if (out_l > 32767)  out_l = 32767;
            if (out_l < -32768) out_l = -32768;
            if (out_r > 32767)  out_r = 32767;
            if (out_r < -32768) out_r = -32768;

            buffer[left_idx]  = (int16_t)out_l;
            buffer[right_idx] = (int16_t)out_r;
        }
    }

    k_mutex_unlock(&s_synth_mutex);
}

static const midi_synth_callbacks_t s_synth_callbacks = {
    .note_on = yamaha_fm_note_on,
    .note_off = yamaha_fm_note_off,
    .program_change = yamaha_fm_program_change,
    .control_change = yamaha_fm_control_change,
    .pitch_bend = yamaha_fm_pitch_bend,
    .all_notes_off = yamaha_fm_all_notes_off
};

const midi_synth_callbacks_t* yamaha_fm_get_callbacks(void)
{
    return &s_synth_callbacks;
}
