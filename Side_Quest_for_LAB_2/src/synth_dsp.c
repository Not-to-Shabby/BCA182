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
static float s_rv_ret;                  /* reverb return gain */
static int32_t s_ch_ret;                /* chorus return gain, Q15 */

static inline int32_t sat16(int32_t x)
{
    return (x > 32767) ? 32767 : ((x < -32768) ? -32768 : x);
}

/* -------------------------------------------------------------------------- */
/* Reverb                                                                     */
/* -------------------------------------------------------------------------- */
#define RV_LINES        8
#define RV_AP1_LEN      113U
#define RV_AP2_LEN      337U
#define RV_AP_GAIN      0.6f
#define RV_NOISE        1e-15f          /* keeps the tail out of the denormal range */

/* 8 coprime delay lines for rich flutter-free concert hall / studio plate reverb */
static const uint16_t RV_LEN[RV_LINES]  = {
    431U, 523U, 617U, 709U, 809U, 907U, 997U, 1103U
};
static const uint16_t RV_BASE[RV_LINES] = {
    0U, 431U, 954U, 1571U, 2280U, 3089U, 3996U, 4993U
};
#define RV_MEM_SAMPLES  6096U

/* About 24 KB, read and written only by the CPU: main RAM, because the core-coupled RAM that
 * holds the voices has no room left for it. */
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

static void reverb_block(const int32_t *in, int32_t *out_l, int32_t *out_r, size_t n)
{
    const float g = RV_AP_GAIN;
    const float damp = s_rv_damp;
    const float ret = s_rv_ret;

    for (size_t i = 0; i < n; i++) {
        s_rv_noise = -s_rv_noise;
        float x = (float)in[i] * 0.25f + s_rv_noise;

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

        /* Orthogonal stereo output taps: left and right decorrelated */
        float l = (f[0] - f[1] + f[2] - f[3] + f[4] - f[5] + f[6] - f[7]) * 0.35f;
        float r = (f[0] + f[1] - f[2] - f[3] + f[4] + f[5] - f[6] - f[7]) * 0.35f;
        out_l[i] += (int32_t)(l * ret);
        out_r[i] += (int32_t)(r * ret);
    }
}

/* -------------------------------------------------------------------------- */
/* Chorus: one delay line, two taps swept by triangle LFOs a quarter turn apart */
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

/* Delay of a tap, in 1/256 samples, for an LFO phase. */
static inline int32_t chorus_delay_q8(uint32_t phase)
{
    int32_t p = (int32_t)(phase >> 16);
    int32_t tri = (p < 32768) ? p * 2 : (65535 - p) * 2;

    return s_ch_center_q8 + (((tri - 32767) * s_ch_depth_q8) >> 15);
}

/* The delay is worked out at the ends of the block and stepped linearly in between; it moves
 * by far less than a sample over 32 frames. */
static void chorus_block(const int32_t *in, int32_t *out_l, int32_t *out_r, size_t n)
{
    uint32_t end = s_ch_phase + s_ch_inc * (uint32_t)n;
    /* 4 quadrature delay taps: 0, 90, 180, 270 degrees for thick Dimension D chorus */
    int32_t d0 = chorus_delay_q8(s_ch_phase) * 256;
    int32_t d1 = chorus_delay_q8(s_ch_phase + 0x40000000U) * 256;
    int32_t d2 = chorus_delay_q8(s_ch_phase + 0x80000000U) * 256;
    int32_t d3 = chorus_delay_q8(s_ch_phase + 0xC0000000U) * 256;
    int32_t s0 = (chorus_delay_q8(end) * 256 - d0) / (int32_t)n;
    int32_t s1 = (chorus_delay_q8(end + 0x40000000U) * 256 - d1) / (int32_t)n;
    int32_t s2 = (chorus_delay_q8(end + 0x80000000U) * 256 - d2) / (int32_t)n;
    int32_t s3 = (chorus_delay_q8(end + 0xC0000000U) * 256 - d3) / (int32_t)n;
    uint32_t wr = s_ch_wr;

    for (size_t i = 0; i < n; i++) {
        s_ch_buf[wr & CH_MASK] = (int16_t)sat16(in[i] >> 1);

        uint32_t w0 = (uint32_t)d0 >> 16; int32_t f0 = (d0 >> 8) & 255;
        int32_t tap0 = (s_ch_buf[(wr - w0) & CH_MASK] * (256 - f0) + s_ch_buf[(wr - w0 - 1U) & CH_MASK] * f0) >> 8;
        uint32_t w1 = (uint32_t)d1 >> 16; int32_t f1 = (d1 >> 8) & 255;
        int32_t tap1 = (s_ch_buf[(wr - w1) & CH_MASK] * (256 - f1) + s_ch_buf[(wr - w1 - 1U) & CH_MASK] * f1) >> 8;
        uint32_t w2 = (uint32_t)d2 >> 16; int32_t f2 = (d2 >> 8) & 255;
        int32_t tap2 = (s_ch_buf[(wr - w2) & CH_MASK] * (256 - f2) + s_ch_buf[(wr - w2 - 1U) & CH_MASK] * f2) >> 8;
        uint32_t w3 = (uint32_t)d3 >> 16; int32_t f3 = (d3 >> 8) & 255;
        int32_t tap3 = (s_ch_buf[(wr - w3) & CH_MASK] * (256 - f3) + s_ch_buf[(wr - w3 - 1U) & CH_MASK] * f3) >> 8;

        int32_t l = (tap0 + tap2) >> 1;
        int32_t r = (tap1 + tap3) >> 1;

        out_l[i] += (l * s_ch_ret) >> 15;
        out_r[i] += (r * s_ch_ret) >> 15;
        d0 += s0; d1 += s1; d2 += s2; d3 += s3;
        wr++;
    }
    s_ch_wr = wr;
    s_ch_phase = end;
}

/* -------------------------------------------------------------------------- */
/* Mastering: compressor, then look-ahead limiter                             */
/* -------------------------------------------------------------------------- */
#define LIM_W               64U
#define LIM_MASK            (LIM_W - 1U)

static bool s_comp_on = true;
static bool s_eq_on = true;
static float s_env_db;              /* smoothed input level */

/* Master Low Shelf (95 Hz, +2.5 dB) and High Shelf (6000 Hz, +2.0 dB) */
#define LS_B0  1.0013795f
#define LS_B1 -1.9821613f
#define LS_B2  0.9809916f
#define LS_A1 -1.9821876f
#define LS_A2  0.9823448f

#define HS_B0  1.1982029f
#define HS_B1 -1.0955769f
#define HS_B2  0.3875449f
#define HS_A1 -0.7955999f
#define HS_A2  0.2857707f

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
    if (s_clean && (float)pk_i * mg * top * (s_eq_on ? 1.40f : 1.0f) <= ceil_abs) {
        uint32_t cur = s_lim_n;

        for (size_t i = 0; i < n; i++) {
            g += step;
            float xl = (float)acc_l[i] * mg * g;
            float xr = (float)acc_r[i] * mg * g;
            if (s_eq_on) {
                float yl = LS_B0 * xl + s_eq_ls_s1[0];
                s_eq_ls_s1[0] = LS_B1 * xl - LS_A1 * yl + s_eq_ls_s2[0];
                s_eq_ls_s2[0] = LS_B2 * xl - LS_A2 * yl;
                float yl2 = HS_B0 * yl + s_eq_hs_s1[0];
                s_eq_hs_s1[0] = HS_B1 * yl - HS_A1 * yl2 + s_eq_hs_s2[0];
                s_eq_hs_s2[0] = HS_B2 * yl - HS_A2 * yl2;
                xl = yl2;

                float yr = LS_B0 * xr + s_eq_ls_s1[1];
                s_eq_ls_s1[1] = LS_B1 * xr - LS_A1 * yr + s_eq_ls_s2[1];
                s_eq_ls_s2[1] = LS_B2 * xr - LS_A2 * yr;
                float yr2 = HS_B0 * yr + s_eq_hs_s1[1];
                s_eq_hs_s1[1] = HS_B1 * yr - HS_A1 * yr2 + s_eq_hs_s2[1];
                s_eq_hs_s2[1] = HS_B2 * yr - HS_A2 * yr2;
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
            float yl = LS_B0 * xl + s_eq_ls_s1[0];
            s_eq_ls_s1[0] = LS_B1 * xl - LS_A1 * yl + s_eq_ls_s2[0];
            s_eq_ls_s2[0] = LS_B2 * xl - LS_A2 * yl;
            float yl2 = HS_B0 * yl + s_eq_hs_s1[0];
            s_eq_hs_s1[0] = HS_B1 * yl - HS_A1 * yl2 + s_eq_hs_s2[0];
            s_eq_hs_s2[0] = HS_B2 * yl - HS_A2 * yl2;
            xl = yl2;

            float yr = LS_B0 * xr + s_eq_ls_s1[1];
            s_eq_ls_s1[1] = LS_B1 * xr - LS_A1 * yr + s_eq_ls_s2[1];
            s_eq_ls_s2[1] = LS_B2 * xr - LS_A2 * yr;
            float yr2 = HS_B0 * yr + s_eq_hs_s1[1];
            s_eq_hs_s1[1] = HS_B1 * yr - HS_A1 * yr2 + s_eq_hs_s2[1];
            s_eq_hs_s2[1] = HS_B2 * yr - HS_A2 * yr2;
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

void dsp_set_effects_level(uint8_t percent)
{
    if (percent > 200U) {
        percent = 200U;
    }
    s_fx_level = percent;
    s_rv_ret = ((float)DSP_REVERB_RETURN_Q15 * (float)percent / 100.0f) * (1.0f / 32768.0f);
    s_ch_ret = (DSP_CHORUS_RETURN_Q15 * (int32_t)percent) / 100;
}

uint8_t dsp_get_effects_level(void)
{
    return s_fx_level;
}

bool dsp_effects_active(void)
{
    return s_fx_level > 0U;
}

void dsp_effects_process(const int32_t *rev_in, const int32_t *cho_in,
                         int32_t *acc_l, int32_t *acc_r, size_t n)
{
    if (s_fx_level == 0U) {
        return;
    }
    reverb_block(rev_in, acc_l, acc_r, n);
    chorus_block(cho_in, acc_l, acc_r, n);
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
    memset(s_ch_buf, 0, sizeof(s_ch_buf));
    s_ch_wr = 0;
    master_clear();
}

void dsp_init(float sample_rate)
{
    s_rate = (sample_rate > 0.0f) ? sample_rate : 44100.0f;

    for (int j = 0; j < RV_LINES; j++) {
        s_rv_gain[j] = expf(-6.9078f * (float)RV_LEN[j] / (DSP_REVERB_T60_S * s_rate));
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
    dsp_clear();
}
