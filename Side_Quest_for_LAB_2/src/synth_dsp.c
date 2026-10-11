/**
 * @file synth_dsp.c
 * @brief Reverb, chorus, compressor and look-ahead limiter for the FM synthesizer.
 *
 * The reverb is a four-line feedback delay network behind two diffusing all-pass filters, in
 * single-precision floats (the Cortex-M4F multiplies and adds them in hardware). The master
 * stage works in floats too: a feed-forward compressor sets one gain per block, and a limiter
 * holds the peaks under the ceiling by looking DSP_LIMITER_DELAY frames ahead. While nothing
 * needs limiting, which is nearly always, the master stage only delays and converts.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include "synth_dsp.h"
#include "dtcm.h"
#include <math.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Tunables (bench builds can override them with -D)                          */
/* -------------------------------------------------------------------------- */
#ifndef DSP_REVERB_T60_S
#define DSP_REVERB_T60_S        1.7f    /* low-frequency decay time */
#endif
#ifndef DSP_REVERB_DAMP_Q15
#define DSP_REVERB_DAMP_Q15     14000   /* loop low-pass: about 4 kHz per trip */
#endif
#ifndef DSP_REVERB_RETURN_Q15
#define DSP_REVERB_RETURN_Q15   24000
#endif
#ifndef DSP_CHORUS_RETURN_Q15
#define DSP_CHORUS_RETURN_Q15   21000
#endif
#ifndef DSP_COMP_THRESH_DB
#define DSP_COMP_THRESH_DB      -18.0f
#endif
#ifndef DSP_COMP_RATIO
#define DSP_COMP_RATIO          2.5f
#endif
#ifndef DSP_COMP_KNEE_DB
#define DSP_COMP_KNEE_DB        10.0f
#endif
#ifndef DSP_COMP_MAKEUP_DB
#define DSP_COMP_MAKEUP_DB      5.0f
#endif
#ifndef DSP_COMP_ATTACK_MS
#define DSP_COMP_ATTACK_MS      3.0f
#endif
#ifndef DSP_COMP_RELEASE_MS
#define DSP_COMP_RELEASE_MS     180.0f
#endif
#ifndef DSP_LIMITER_RELEASE_MS
#define DSP_LIMITER_RELEASE_MS  90.0f
#endif

#define DB_PER_OCTAVE           6.0206f

static float s_rate = 44100.0f;
static uint8_t s_fx_level = 100;
static uint8_t s_rv_level = 100;        /* reverb return, percent of the effects level */
static uint8_t s_ch_level = 100;        /* chorus return, percent of the effects level */
static float s_rv_ret;                  /* reverb return gain */
static int32_t s_ch_ret;                /* chorus return gain, Q15 */

static inline int32_t sat16(int32_t x)
{
    return (x > 32767) ? 32767 : ((x < -32768) ? -32768 : x);
}

/* -------------------------------------------------------------------------- */
/* Reverb: 8-line coprime FDN running at half-rate (22.05 kHz) with linear     */
/* upsampling interpolation to 44.1 kHz. Saves ~10 KB of Main SRAM and cuts   */
/* FDN computation cycles in half.                                            */
/* -------------------------------------------------------------------------- */
#define RV_LINES        8
#define RV_AP1_LEN      59U
#define RV_AP2_LEN      167U
#define RV_AP_GAIN      0.6f
#define RV_NOISE        1e-15f          /* keeps the tail out of the denormal range */

/* 8 coprime delay lines at 22,050 Hz: 11.4 ms to 30.0 ms delay spread */
static const uint16_t RV_LEN[RV_LINES]  = {
    251U, 307U, 367U, 421U, 479U, 541U, 601U, 661U
};
static const uint16_t RV_BASE[RV_LINES] = {
    0U, 251U, 558U, 925U, 1346U, 1825U, 2366U, 2967U
};
#define RV_MEM_SAMPLES  3628U

static float s_rv_mem[RV_MEM_SAMPLES];
static float s_rv_ap1[RV_AP1_LEN];
static float s_rv_ap2[RV_AP2_LEN];
static float s_rv_lp[RV_LINES];
static float s_rv_gain[RV_LINES];
static float s_rv_damp;
static uint16_t s_rv_pos[RV_LINES];
static uint16_t s_rv_ap1_pos;
static uint16_t s_rv_ap2_pos;
static float s_rv_noise = RV_NOISE;
static float s_rv_prev_l = 0.0f;
static float s_rv_prev_r = 0.0f;
static bool s_rv_active = false;
static uint32_t s_rv_silent_count = 0;

static void reverb_block(const int32_t *in, int32_t *out_l, int32_t *out_r, size_t n)
{
    const float g = RV_AP_GAIN;
    const float damp = s_rv_damp;
    const float ret = s_rv_ret;

    /* Check if input has energy */
    bool has_input = false;
    for (size_t i = 0; i < n; i++) {
        if (in[i] != 0) {
            has_input = true;
            break;
        }
    }
    if (has_input) {
        s_rv_active = true;
        s_rv_silent_count = 0;
    } else if (!s_rv_active) {
        return; /* Reverb tail is in complete digital silence: sleep inner FDN loop */
    }

    float max_tail = 0.0f;

    /* Process 2 stereo frames per FDN step (22.05 kHz downsampled execution) */
    for (size_t i = 0; i < n; i += 2U) {
        s_rv_noise = -s_rv_noise;
        /* Average continuous input sample pairs (0.125 = 0.25 * 0.5); preserve single-sample impulses */
        float scale = (in[i] != 0 && in[i + 1U] != 0) ? 0.125f : 0.25f;
        float in_pair = ((float)in[i] + (float)in[i + 1U]) * scale;
        float x = in_pair + s_rv_noise;

        float d = s_rv_ap1[s_rv_ap1_pos];
        float y = d - g * x;
        s_rv_ap1[s_rv_ap1_pos] = x + g * y;
        if (++s_rv_ap1_pos >= RV_AP1_LEN) {
            s_rv_ap1_pos = 0;
        }

        d = s_rv_ap2[s_rv_ap2_pos];
        float z = d - g * y;
        s_rv_ap2[s_rv_ap2_pos] = y + g * z;
        if (++s_rv_ap2_pos >= RV_AP2_LEN) {
            s_rv_ap2_pos = 0;
        }

        float f[8];
        float v[8];
        float sum_v = 0.0f;
        for (int j = 0; j < 8; j++) {
            const float *mp = &s_rv_mem[RV_BASE[j] + s_rv_pos[j]];
            f[j] = s_rv_lp[j] + damp * (*mp - s_rv_lp[j]);
            s_rv_lp[j] = f[j];
            v[j] = f[j] * s_rv_gain[j];
            sum_v += v[j];
            float af = (f[j] < 0.0f) ? -f[j] : f[j];
            if (af > max_tail) max_tail = af;
        }
        sum_v *= 0.25f;
        float inj = z * 0.45f;

        /* 8x8 Lossless Householder reflection matrix: w_j = v_j - 0.25*sum(v) + inj_j */
        s_rv_mem[RV_BASE[0] + s_rv_pos[0]] = (v[0] - sum_v) + inj;
        s_rv_mem[RV_BASE[1] + s_rv_pos[1]] = (v[1] - sum_v) - inj;
        s_rv_mem[RV_BASE[2] + s_rv_pos[2]] = (v[2] - sum_v) + inj;
        s_rv_mem[RV_BASE[3] + s_rv_pos[3]] = (v[3] - sum_v) - inj;
        s_rv_mem[RV_BASE[4] + s_rv_pos[4]] = (v[4] - sum_v) + inj;
        s_rv_mem[RV_BASE[5] + s_rv_pos[5]] = (v[5] - sum_v) - inj;
        s_rv_mem[RV_BASE[6] + s_rv_pos[6]] = (v[6] - sum_v) + inj;
        s_rv_mem[RV_BASE[7] + s_rv_pos[7]] = (v[7] - sum_v) - inj;

        for (int j = 0; j < 8; j++) {
            if (++s_rv_pos[j] >= RV_LEN[j]) {
                s_rv_pos[j] = 0;
            }
        }

        /* Mutually orthogonal Walsh-Hadamard stereo output taps (orthogonal to inj and to each other) */
        float cur_l = (f[0] - f[1] - f[2] + f[3] + f[4] - f[5] - f[6] + f[7]) * 0.35f;
        float cur_r = (f[0] + f[1] - f[2] - f[3] + f[4] + f[5] - f[6] - f[7]) * 0.35f;

        /* Anti-imaging linear interpolation back to 44.1 kHz */
        float mid_l = (s_rv_prev_l + cur_l) * 0.5f;
        float mid_r = (s_rv_prev_r + cur_r) * 0.5f;

        out_l[i]      += (int32_t)(mid_l * ret);
        out_r[i]      += (int32_t)(mid_r * ret);
        out_l[i + 1U] += (int32_t)(cur_l * ret);
        out_r[i + 1U] += (int32_t)(cur_r * ret);

        s_rv_prev_l = cur_l;
        s_rv_prev_r = cur_r;
    }

    if (!has_input && max_tail < 0.05f) {
        if (++s_rv_silent_count > 64U) {
            /* Tail has completely decayed into silence: clear state and enter sleep */
            memset(s_rv_mem, 0, sizeof(s_rv_mem));
            memset(s_rv_ap1, 0, sizeof(s_rv_ap1));
            memset(s_rv_ap2, 0, sizeof(s_rv_ap2));
            memset(s_rv_lp, 0, sizeof(s_rv_lp));
            s_rv_prev_l = 0.0f;
            s_rv_prev_r = 0.0f;
            s_rv_active = false;
            s_rv_silent_count = 0;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Chorus: Roland Juno / CE-2 True Stereo Quadrature (2 opposite-phase taps)  */
/* -------------------------------------------------------------------------- */
#define CH_LEN              1024U
#define CH_MASK             (CH_LEN - 1U)
#define CH_CENTER_MS        14.0f
#define CH_DEPTH_MS         2.5f
#define CH_RATE_HZ          0.6f

static DTCM_BSS int16_t s_ch_buf[CH_LEN];
static uint32_t s_ch_wr;
static uint32_t s_ch_phase;
static uint32_t s_ch_inc;
static int32_t s_ch_center_q8;
static int32_t s_ch_depth_q8;
static bool s_ch_active = false;
static uint32_t s_ch_silent_count = 0;

/* Delay of a tap, in 1/256 samples, for an LFO phase. */
static inline int32_t chorus_delay_q8(uint32_t phase)
{
    int32_t p = (int32_t)(phase >> 16);
    int32_t tri = (p < 32768) ? p * 2 : (65535 - p) * 2;

    return s_ch_center_q8 + (((tri - 32767) * s_ch_depth_q8) >> 15);
}

static void chorus_block(const int32_t *in, int32_t *out_l, int32_t *out_r, size_t n)
{
    bool has_input = false;
    for (size_t i = 0; i < n; i++) {
        if (in[i] != 0) {
            has_input = true;
            break;
        }
    }
    if (has_input) {
        s_ch_active = true;
        s_ch_silent_count = 0;
    } else if (!s_ch_active) {
        return; /* Delay buffer is completely silent */
    }

    uint32_t end = s_ch_phase + s_ch_inc * (uint32_t)n;
    /* 2 opposite-phase taps (0 deg Left, 180 deg Right) for classic lush Roland stereo chorus */
    int32_t d0 = chorus_delay_q8(s_ch_phase) * 256;
    int32_t d1 = chorus_delay_q8(s_ch_phase + 0x80000000U) * 256;
    int32_t s0 = (chorus_delay_q8(end) * 256 - d0) / (int32_t)n;
    int32_t s1 = (chorus_delay_q8(end + 0x80000000U) * 256 - d1) / (int32_t)n;
    uint32_t wr = s_ch_wr;

    for (size_t i = 0; i < n; i++) {
        s_ch_buf[wr & CH_MASK] = (int16_t)sat16(in[i] >> 1);

        uint32_t w0 = (uint32_t)d0 >> 16; int32_t f0 = (d0 >> 8) & 255;
        int32_t tap_l = (s_ch_buf[(wr - w0) & CH_MASK] * (256 - f0) + s_ch_buf[(wr - w0 - 1U) & CH_MASK] * f0) >> 8;

        uint32_t w1 = (uint32_t)d1 >> 16; int32_t f1 = (d1 >> 8) & 255;
        int32_t tap_r = (s_ch_buf[(wr - w1) & CH_MASK] * (256 - f1) + s_ch_buf[(wr - w1 - 1U) & CH_MASK] * f1) >> 8;

        out_l[i] += (tap_l * s_ch_ret) >> 15;
        out_r[i] += (tap_r * s_ch_ret) >> 15;
        d0 += s0;
        d1 += s1;
        wr++;
    }
    s_ch_wr = wr;
    s_ch_phase = end;

    if (!has_input) {
        if (++s_ch_silent_count > (CH_LEN / DSP_BLOCK_FRAMES + 2U)) {
            memset(s_ch_buf, 0, sizeof(s_ch_buf));
            s_ch_active = false;
            s_ch_silent_count = 0;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Mastering: compressor, then look-ahead limiter                             */
/* -------------------------------------------------------------------------- */
#define LIM_W               64U
#define LIM_MASK            (LIM_W - 1U)

static bool s_comp_on = true;
static bool s_eq_on = true;
static float s_env_db;              /* smoothed input level */

/* Master Low Shelf (95 Hz, +2.5 dB) and High Shelf (6000 Hz, +2.0 dB). dsp_set_tone() moves the
 * two gains; at 0 dB offset the coefficients below are used exactly. */
static const float TONE_FLAT[10] = {
    1.0013795f, -1.9821613f, 0.9809916f, -1.9821876f, 0.9823448f,
    1.1982029f, -1.0955769f, 0.3875449f, -0.7955999f, 0.2857707f
};
#define EQ_BASE_LOW_DB     2.5f
#define EQ_BASE_HIGH_DB    2.0f
#define EQ_HEADROOM_FLAT   1.40f

static float s_eq_coef[10] = {
    1.0013795f, -1.9821613f, 0.9809916f, -1.9821876f, 0.9823448f,
    1.1982029f, -1.0955769f, 0.3875449f, -0.7955999f, 0.2857707f
};
static float s_eq_headroom = EQ_HEADROOM_FLAT;
static float s_eq_pending[10];
static float s_eq_pending_headroom;
static volatile bool s_eq_dirty;
static int8_t s_tone_bass;
static int8_t s_tone_treble;

static DTCM_BSS float s_eq_ls_s1[2], s_eq_ls_s2[2];
static DTCM_BSS float s_eq_hs_s1[2], s_eq_hs_s2[2];
static float s_cg = 1.0f;           /* compressor gain at the end of the previous block */
static float s_gr_max;
static float s_att_k;
static float s_rel_k;
static float s_lim_rel;

static DTCM_BSS float s_dl_l[LIM_W];
static DTCM_BSS float s_dl_r[LIM_W];
static DTCM_BSS float s_need[LIM_W];
static DTCM_BSS uint32_t s_dq[LIM_W];
static DTCM_BSS uint32_t s_mhist[LIM_W];
static uint32_t s_lim_n;
static uint32_t s_dq_head;
static uint32_t s_dq_tail;
static uint32_t s_msum;
static float s_glim = 1.0f;
static uint32_t s_lim_samples;
static uint32_t s_clear_run;        /* consecutive samples that needed no limiting */
static bool s_clean;                /* limiter history is all "no limiting": take the fast path */
static uint32_t s_dith_prng = 0x12345678U;
static float s_dith_err[2];

static void master_clear(void)
{
    memset(s_dl_l, 0, sizeof(s_dl_l));
    memset(s_dl_r, 0, sizeof(s_dl_r));
    for (uint32_t i = 0; i < LIM_W; i++) {
        s_need[i] = 1.0f;
        s_mhist[i] = 65536U;
    }
    memset(s_dq, 0, sizeof(s_dq));
    s_msum = 65536U * LIM_W;
    s_lim_n = 0;
    s_dq_head = 0;
    s_dq_tail = 0;
    s_glim = 1.0f;
    s_env_db = -100.0f;
    s_cg = 1.0f;
    s_clear_run = LIM_W;
    s_clean = true;
    memset(s_eq_ls_s1, 0, sizeof(s_eq_ls_s1));
    memset(s_eq_ls_s2, 0, sizeof(s_eq_ls_s2));
    memset(s_eq_hs_s1, 0, sizeof(s_eq_hs_s1));
    memset(s_eq_hs_s2, 0, sizeof(s_eq_hs_s2));
    s_dith_prng = 0x12345678U;
    s_dith_err[0] = 0.0f;
    s_dith_err[1] = 0.0f;
}

static float compressor_target(float peak_norm)
{
    if (!s_comp_on) {
        return 1.0f;
    }

    float lvl = (peak_norm > 1e-5f) ? DB_PER_OCTAVE * log2f(peak_norm) : -100.0f;
    float k = (lvl > s_env_db) ? s_att_k : s_rel_k;
    s_env_db += (lvl - s_env_db) * k;

    float over = s_env_db - DSP_COMP_THRESH_DB;
    float slope = 1.0f - 1.0f / DSP_COMP_RATIO;
    float gr = 0.0f;

    if (over >= DSP_COMP_KNEE_DB * 0.5f) {
        gr = over * slope;
    } else if (over > -DSP_COMP_KNEE_DB * 0.5f) {
        float t = over + DSP_COMP_KNEE_DB * 0.5f;
        gr = slope * t * t / (2.0f * DSP_COMP_KNEE_DB);
    }
    if (gr > s_gr_max) {
        s_gr_max = gr;
    }

    float makeup = DSP_COMP_MAKEUP_DB;
    float gate_db = (lvl > s_env_db) ? lvl : s_env_db;
    if (gate_db < -52.0f) {
        /* Downward expander / gate: opens instantly on note attack (lvl), closes smoothly on dying tail (s_env_db) */
        float gate_t = (gate_db + 68.0f) * (1.0f / 16.0f);
        if (gate_t <= 0.0f) {
            return 0.0f;
        }
        makeup = DSP_COMP_MAKEUP_DB * gate_t - (1.0f - gate_t) * 12.0f;
    }
    return exp2f((makeup - gr) * (1.0f / DB_PER_OCTAVE));
}

static inline int16_t to_i16(float y)
{
    int32_t i = (int32_t)(y + ((y >= 0.0f) ? 0.5f : -0.5f));
    return (int16_t)sat16(i);
}

static inline int16_t to_i16_dither(float y, int ch)
{
    if (fabsf(y) < 0.25f) {
        s_dith_err[ch] = 0.0f;
        return 0;
    }
    s_dith_prng = s_dith_prng * 1664525U + 1013904223U;
    int32_t r1 = (int32_t)(s_dith_prng >> 23) - 256;
    s_dith_prng = s_dith_prng * 1664525U + 1013904223U;
    int32_t r2 = (int32_t)(s_dith_prng >> 23) - 256;
    float tpdf = (float)(r1 - r2) * (1.0f / 512.0f); /* Triangular TPDF dither [-1.0, +1.0] LSB */

    float target = y + tpdf + s_dith_err[ch] * 0.5f; /* 1st-order high-pass error feedback */
    int32_t i = (int32_t)(target + ((target >= 0.0f) ? 0.5f : -0.5f));
    int16_t out = (int16_t)sat16(i);
    s_dith_err[ch] = y - (float)out;
    return out;
}

void dsp_master_process(const int32_t *acc_l, const int32_t *acc_r, int16_t *out,
                        size_t n, uint32_t master_q8)
{
    const float mg = (float)master_q8 * (1.0f / 256.0f);
    const float ceil_abs = DSP_CEILING * 32767.0f;
    int32_t pk_i = 0;

    if (s_eq_dirty) {
        memcpy(s_eq_coef, s_eq_pending, sizeof(s_eq_coef));
        s_eq_headroom = s_eq_pending_headroom;
        s_eq_dirty = false;
    }
    const float ls_b0 = s_eq_coef[0], ls_b1 = s_eq_coef[1], ls_b2 = s_eq_coef[2], ls_a1 = s_eq_coef[3], ls_a2 = s_eq_coef[4];
    const float hs_b0 = s_eq_coef[5], hs_b1 = s_eq_coef[6], hs_b2 = s_eq_coef[7], hs_a1 = s_eq_coef[8], hs_a2 = s_eq_coef[9];

    for (size_t i = 0; i < n; i++) {
        int32_t a = (acc_l[i] < 0) ? -acc_l[i] : acc_l[i];
        int32_t b = (acc_r[i] < 0) ? -acc_r[i] : acc_r[i];
        pk_i = (a > pk_i) ? a : pk_i;
        pk_i = (b > pk_i) ? b : pk_i;
    }

    float target = compressor_target((float)pk_i * mg * (1.0f / 32768.0f));
    float g = s_cg;
    float step = (target - g) / (float)n;
    float top = (g > target) ? g : target;

    /* Fast path: the loudest sample this block can produce is under the ceiling and the limiter
     * has nothing in memory, so every needed gain is 1 and the output is the delayed input. */
    if (s_clean && (float)pk_i * mg * top * (s_eq_on ? s_eq_headroom : 1.0f) <= ceil_abs) {
        uint32_t cur = s_lim_n;

        for (size_t i = 0; i < n; i++) {
            g += step;
            float xl = (float)acc_l[i] * mg * g;
            float xr = (float)acc_r[i] * mg * g;
            if (s_eq_on) {
                float yl = ls_b0 * xl + s_eq_ls_s1[0];
                s_eq_ls_s1[0] = ls_b1 * xl - ls_a1 * yl + s_eq_ls_s2[0];
                s_eq_ls_s2[0] = ls_b2 * xl - ls_a2 * yl;
                float yl2 = hs_b0 * yl + s_eq_hs_s1[0];
                s_eq_hs_s1[0] = hs_b1 * yl - hs_a1 * yl2 + s_eq_hs_s2[0];
                s_eq_hs_s2[0] = hs_b2 * yl - hs_a2 * yl2;
                xl = yl2;

                float yr = ls_b0 * xr + s_eq_ls_s1[1];
                s_eq_ls_s1[1] = ls_b1 * xr - ls_a1 * yr + s_eq_ls_s2[1];
                s_eq_ls_s2[1] = ls_b2 * xr - ls_a2 * yr;
                float yr2 = hs_b0 * yr + s_eq_hs_s1[1];
                s_eq_hs_s1[1] = hs_b1 * yr - hs_a1 * yr2 + s_eq_hs_s2[1];
                s_eq_hs_s2[1] = hs_b2 * yr - hs_a2 * yr2;
                xr = yr2;
            }
            uint32_t at = cur & LIM_MASK;
            uint32_t rd = (cur + 1U) & LIM_MASK;

            out[i * 2U] = to_i16_dither(s_dl_l[rd], 0);
            out[i * 2U + 1U] = to_i16_dither(s_dl_r[rd], 1);
            s_dl_l[at] = xl;
            s_dl_r[at] = xr;
            cur++;
        }
        s_lim_n = cur;
        s_cg = target;
        return;
    }
    s_clean = false;

    for (size_t i = 0; i < n; i++) {
        g += step;
        float xl = (float)acc_l[i] * mg * g;
        float xr = (float)acc_r[i] * mg * g;
        if (s_eq_on) {
            float yl = ls_b0 * xl + s_eq_ls_s1[0];
            s_eq_ls_s1[0] = ls_b1 * xl - ls_a1 * yl + s_eq_ls_s2[0];
            s_eq_ls_s2[0] = ls_b2 * xl - ls_a2 * yl;
            float yl2 = hs_b0 * yl + s_eq_hs_s1[0];
            s_eq_hs_s1[0] = hs_b1 * yl - hs_a1 * yl2 + s_eq_hs_s2[0];
            s_eq_hs_s2[0] = hs_b2 * yl - hs_a2 * yl2;
            xl = yl2;

            float yr = ls_b0 * xr + s_eq_ls_s1[1];
            s_eq_ls_s1[1] = ls_b1 * xr - ls_a1 * yr + s_eq_ls_s2[1];
            s_eq_ls_s2[1] = ls_b2 * xr - ls_a2 * yr;
            float yr2 = hs_b0 * yr + s_eq_hs_s1[1];
            s_eq_hs_s1[1] = hs_b1 * yr - hs_a1 * yr2 + s_eq_hs_s2[1];
            s_eq_hs_s2[1] = hs_b2 * yr - hs_a2 * yr2;
            xr = yr2;
        }
        float ax = (xl < 0.0f) ? -xl : xl;
        float ay = (xr < 0.0f) ? -xr : xr;
        float pk = (ax > ay) ? ax : ay;
        float need = (pk > ceil_abs) ? ceil_abs / pk : 1.0f;
        uint32_t cur = s_lim_n;

        /* Smallest gain needed by any of the last LIM_W samples: a monotonic queue of sample
         * indexes. The expired front goes first so that the queue never needs more than
         * LIM_W slots. */
        while (s_dq_tail != s_dq_head && s_dq[s_dq_head & LIM_MASK] + LIM_W <= cur) {
            s_dq_head++;
        }
        s_need[cur & LIM_MASK] = need;
        while (s_dq_tail != s_dq_head && s_need[s_dq[(s_dq_tail - 1U) & LIM_MASK] & LIM_MASK] >= need) {
            s_dq_tail--;
        }
        s_dq[s_dq_tail & LIM_MASK] = cur;
        s_dq_tail++;
        float m = s_need[s_dq[s_dq_head & LIM_MASK] & LIM_MASK];

        /* A box average of that minimum ramps the gain down over LIM_W samples, so it has
         * reached the needed value by the time the peak leaves the delay line. */
        uint32_t mq = (uint32_t)(m * 65536.0f);
        s_msum += mq - s_mhist[cur & LIM_MASK];
        s_mhist[cur & LIM_MASK] = mq;
        float smooth = (float)(s_msum >> 6) * (1.0f / 65536.0f);

        float rising = s_glim + (1.0f - s_glim) * s_lim_rel;
        s_glim = (smooth < rising) ? smooth : rising;
        if (s_glim < 0.999f) {
            s_lim_samples++;
        }
        if (need < 1.0f) {
            s_clear_run = 0;
        } else if (s_clear_run < LIM_W) {
            s_clear_run++;
        }

        float dl = s_dl_l[(cur + 1U) & LIM_MASK];
        float dr = s_dl_r[(cur + 1U) & LIM_MASK];
        s_dl_l[cur & LIM_MASK] = xl;
        s_dl_r[cur & LIM_MASK] = xr;
        s_lim_n = cur + 1U;

        out[i * 2U] = to_i16_dither(dl * s_glim, 0);
        out[i * 2U + 1U] = to_i16_dither(dr * s_glim, 1);
    }
    s_cg = target;

    /* All of the last LIM_W needs were 1 and the gain has recovered: from here on the fast
     * path is exact (the ring holds only 1.0 and the box sum is full). */
    if (s_clear_run >= LIM_W && s_glim > 0.9999f && s_msum == 65536U * LIM_W) {
        s_glim = 1.0f;
        s_clean = true;
    }
}

int dsp_take_reduction_db10(void)
{
    int v = (int)(s_gr_max * 10.0f);

    s_gr_max = 0.0f;
    return v;
}

uint32_t dsp_get_limiter_samples(void)
{
    return s_lim_samples;
}

/* -------------------------------------------------------------------------- */
/* Public control                                                             */
/* -------------------------------------------------------------------------- */
void dsp_set_eq(bool on)
{
    s_eq_on = on;
}

bool dsp_get_eq(void)
{
    return s_eq_on;
}

void dsp_set_compressor(bool on)
{
    s_comp_on = on;
    s_eq_on = on;
}

bool dsp_get_compressor(void)
{
    return s_comp_on;
}

static void update_returns(void)
{
    float all = (float)s_fx_level / 100.0f;

    s_rv_ret = ((float)DSP_REVERB_RETURN_Q15 * all * ((float)s_rv_level / 100.0f)) * (1.0f / 32768.0f);
    s_ch_ret = (int32_t)((float)DSP_CHORUS_RETURN_Q15 * all * ((float)s_ch_level / 100.0f));
}

void dsp_set_effects_level(uint8_t percent)
{
    if (percent > 200U) {
        percent = 200U;
    }
    s_fx_level = percent;
    update_returns();
}

void dsp_set_reverb_level(uint8_t percent)
{
    s_rv_level = (percent > 200U) ? 200U : percent;
    update_returns();
}

void dsp_set_chorus_level(uint8_t percent)
{
    s_ch_level = (percent > 200U) ? 200U : percent;
    update_returns();
}

uint8_t dsp_get_reverb_level(void)
{
    return s_rv_level;
}

uint8_t dsp_get_chorus_level(void)
{
    return s_ch_level;
}

/* RBJ shelf: gain in dB, shelf slope S. Writes b0 b1 b2 a1 a2 (a0 divided out). */
static void shelf_coef(bool high, float f0, float gain_db, float slope, float rate, float *out)
{
    float a = powf(10.0f, gain_db / 40.0f);
    float w = 6.2831853f * f0 / rate;
    float cw = cosf(w);
    float al = sinf(w) / 2.0f * sqrtf((a + 1.0f / a) * (1.0f / slope - 1.0f) + 2.0f);
    float tsa = 2.0f * sqrtf(a) * al;
    float b0, b1, b2, a0, a1, a2;

    if (!high) {
        b0 = a * ((a + 1.0f) - (a - 1.0f) * cw + tsa);
        b1 = 2.0f * a * ((a - 1.0f) - (a + 1.0f) * cw);
        b2 = a * ((a + 1.0f) - (a - 1.0f) * cw - tsa);
        a0 = (a + 1.0f) + (a - 1.0f) * cw + tsa;
        a1 = -2.0f * ((a - 1.0f) + (a + 1.0f) * cw);
        a2 = (a + 1.0f) + (a - 1.0f) * cw - tsa;
    } else {
        b0 = a * ((a + 1.0f) + (a - 1.0f) * cw + tsa);
        b1 = -2.0f * a * ((a - 1.0f) + (a + 1.0f) * cw);
        b2 = a * ((a + 1.0f) + (a - 1.0f) * cw - tsa);
        a0 = (a + 1.0f) - (a - 1.0f) * cw + tsa;
        a1 = 2.0f * ((a - 1.0f) - (a + 1.0f) * cw);
        a2 = (a + 1.0f) - (a - 1.0f) * cw - tsa;
    }
    out[0] = b0 / a0;
    out[1] = b1 / a0;
    out[2] = b2 / a0;
    out[3] = a1 / a0;
    out[4] = a2 / a0;
}

void dsp_set_tone(int8_t bass_db, int8_t treble_db)
{
    if (bass_db < -DSP_TONE_MAX_DB) bass_db = -DSP_TONE_MAX_DB;
    if (bass_db > DSP_TONE_MAX_DB) bass_db = DSP_TONE_MAX_DB;
    if (treble_db < -DSP_TONE_MAX_DB) treble_db = -DSP_TONE_MAX_DB;
    if (treble_db > DSP_TONE_MAX_DB) treble_db = DSP_TONE_MAX_DB;
    s_tone_bass = bass_db;
    s_tone_treble = treble_db;

    float lo = EQ_BASE_LOW_DB + (float)bass_db;
    float hi = EQ_BASE_HIGH_DB + (float)treble_db;
    float top = (lo > hi) ? lo : hi;

    if (bass_db == 0 && treble_db == 0) {
        memcpy(s_eq_pending, TONE_FLAT, sizeof(s_eq_pending));
        s_eq_pending_headroom = EQ_HEADROOM_FLAT;
    } else {
        shelf_coef(false, 95.0f, lo, 1.0f, s_rate, &s_eq_pending[0]);
        shelf_coef(true, 6000.0f, hi, 1.035f, s_rate, &s_eq_pending[5]);
        s_eq_pending_headroom = 1.05f * powf(10.0f, ((top > 0.0f) ? top : 0.0f) / 20.0f);
    }
    s_eq_dirty = true;
}

int8_t dsp_get_tone_bass(void)
{
    return s_tone_bass;
}

int8_t dsp_get_tone_treble(void)
{
    return s_tone_treble;
}

uint8_t dsp_get_effects_level(void)
{
    return s_fx_level;
}

bool dsp_effects_active(void)
{
    return s_fx_level > 0U && (s_rv_level > 0U || s_ch_level > 0U);
}

void dsp_effects_process(const int32_t *rev_in, const int32_t *cho_in,
                         int32_t *acc_l, int32_t *acc_r, size_t n)
{
    if (s_fx_level == 0U) {
        return;
    }
    if (s_rv_ret > 0.0f) {
        reverb_block(rev_in, acc_l, acc_r, n);
    }
    if (s_ch_ret > 0) {
        chorus_block(cho_in, acc_l, acc_r, n);
    }
}

void dsp_clear(void)
{
    memset(s_rv_mem, 0, sizeof(s_rv_mem));
    memset(s_rv_ap1, 0, sizeof(s_rv_ap1));
    memset(s_rv_ap2, 0, sizeof(s_rv_ap2));
    memset(s_rv_lp, 0, sizeof(s_rv_lp));
    memset(s_rv_pos, 0, sizeof(s_rv_pos));
    s_rv_ap1_pos = 0;
    s_rv_ap2_pos = 0;
    s_rv_prev_l = 0.0f;
    s_rv_prev_r = 0.0f;
    s_rv_active = false;
    s_rv_silent_count = 0;
    memset(s_ch_buf, 0, sizeof(s_ch_buf));
    s_ch_wr = 0;
    s_ch_active = false;
    s_ch_silent_count = 0;
    master_clear();
}

void dsp_init(float sample_rate)
{
    s_rate = (sample_rate > 0.0f) ? sample_rate : 44100.0f;
    float rv_rate = s_rate * 0.5f;

    for (int j = 0; j < RV_LINES; j++) {
        s_rv_gain[j] = expf(-6.9078f * (float)RV_LEN[j] / (DSP_REVERB_T60_S * rv_rate));
    }
    s_rv_damp = (float)DSP_REVERB_DAMP_Q15 * (1.0f / 32768.0f);
    s_ch_inc = (uint32_t)(CH_RATE_HZ * 4294967296.0f / s_rate);
    s_ch_phase = 0;
    s_ch_center_q8 = (int32_t)(CH_CENTER_MS * 0.001f * s_rate * 256.0f);
    s_ch_depth_q8 = (int32_t)(CH_DEPTH_MS * 0.001f * s_rate * 256.0f);

    float block_ms = 1000.0f * (float)DSP_BLOCK_FRAMES / s_rate;
    s_att_k = 1.0f - expf(-block_ms / DSP_COMP_ATTACK_MS);
    s_rel_k = 1.0f - expf(-block_ms / DSP_COMP_RELEASE_MS);
    s_lim_rel = 1.0f - expf(-1000.0f / (DSP_LIMITER_RELEASE_MS * s_rate));

    s_comp_on = true;
    s_eq_on = true;
    s_gr_max = 0.0f;
    s_lim_samples = 0;
    dsp_set_effects_level(100);
    s_rv_level = 100;
    s_ch_level = 100;
    update_returns();
    dsp_set_tone(0, 0);
    memcpy(s_eq_coef, s_eq_pending, sizeof(s_eq_coef));
    s_eq_headroom = s_eq_pending_headroom;
    s_eq_dirty = false;
    dsp_clear();
}
