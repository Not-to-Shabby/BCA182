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
#define RV_LINES        4
#define RV_AP1_LEN      113U
#define RV_AP2_LEN      337U
#define RV_AP_GAIN      0.6f
#define RV_NOISE        1e-15f          /* keeps the tail out of the denormal range */

static const uint16_t RV_LEN[RV_LINES]  = { 1117U, 1361U, 1597U, 1801U };
static const uint16_t RV_BASE[RV_LINES] = { 0U, 1117U, 2478U, 4075U };
#define RV_MEM_SAMPLES  5876U

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

        float f0, f1, f2, f3;
        float *m0 = &s_rv_mem[RV_BASE[0] + s_rv_pos[0]];
        float *m1 = &s_rv_mem[RV_BASE[1] + s_rv_pos[1]];
        float *m2 = &s_rv_mem[RV_BASE[2] + s_rv_pos[2]];
        float *m3 = &s_rv_mem[RV_BASE[3] + s_rv_pos[3]];

        f0 = s_rv_lp[0] + damp * (*m0 - s_rv_lp[0]);
        f1 = s_rv_lp[1] + damp * (*m1 - s_rv_lp[1]);
        f2 = s_rv_lp[2] + damp * (*m2 - s_rv_lp[2]);
        f3 = s_rv_lp[3] + damp * (*m3 - s_rv_lp[3]);
        s_rv_lp[0] = f0;
        s_rv_lp[1] = f1;
        s_rv_lp[2] = f2;
        s_rv_lp[3] = f3;

        float v0 = f0 * s_rv_gain[0];
        float v1 = f1 * s_rv_gain[1];
        float v2 = f2 * s_rv_gain[2];
        float v3 = f3 * s_rv_gain[3];
        float inj = z * 0.5f;
        float a = (v0 + v1) * 0.5f;
        float b = (v2 + v3) * 0.5f;
        float c = (v0 - v1) * 0.5f;
        float e = (v2 - v3) * 0.5f;

        *m0 = a + b + inj;
        *m1 = c + e + inj;
        *m2 = a - b + inj;
        *m3 = c - e + inj;
        if (++s_rv_pos[0] >= RV_LEN[0]) s_rv_pos[0] = 0;
        if (++s_rv_pos[1] >= RV_LEN[1]) s_rv_pos[1] = 0;
        if (++s_rv_pos[2] >= RV_LEN[2]) s_rv_pos[2] = 0;
        if (++s_rv_pos[3] >= RV_LEN[3]) s_rv_pos[3] = 0;

        float l = (f0 - f1 + f2 - f3) * 0.5f;
        float r = (f0 + f1 - f2 - f3) * 0.5f;
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
    int32_t dl = chorus_delay_q8(s_ch_phase) * 256;
    int32_t dr = chorus_delay_q8(s_ch_phase + 0x40000000U) * 256;
    int32_t sl = (chorus_delay_q8(end) * 256 - dl) / (int32_t)n;
    int32_t sr = (chorus_delay_q8(end + 0x40000000U) * 256 - dr) / (int32_t)n;
    uint32_t wr = s_ch_wr;

    for (size_t i = 0; i < n; i++) {
        s_ch_buf[wr & CH_MASK] = (int16_t)sat16(in[i] >> 1);

        uint32_t wl = (uint32_t)dl >> 16;
        int32_t fl = (dl >> 8) & 255;
        int32_t l = (s_ch_buf[(wr - wl) & CH_MASK] * (256 - fl) + s_ch_buf[(wr - wl - 1U) & CH_MASK] * fl) >> 8;
        uint32_t wr2 = (uint32_t)dr >> 16;
        int32_t fr = (dr >> 8) & 255;
        int32_t r = (s_ch_buf[(wr - wr2) & CH_MASK] * (256 - fr) + s_ch_buf[(wr - wr2 - 1U) & CH_MASK] * fr) >> 8;

        out_l[i] += (l * s_ch_ret) >> 15;
        out_r[i] += (r * s_ch_ret) >> 15;
        dl += sl;
        dr += sr;
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
static float s_env_db;              /* smoothed input level */
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
    if (s_clean && (float)pk_i * mg * top <= ceil_abs) {
        uint32_t cur = s_lim_n;

        for (size_t i = 0; i < n; i++) {
            g += step;
            float xl = (float)acc_l[i] * mg * g;
            float xr = (float)acc_r[i] * mg * g;
            uint32_t at = cur & LIM_MASK;
            uint32_t rd = (cur + 1U) & LIM_MASK;

            out[i * 2U] = to_i16(s_dl_l[rd]);
            out[i * 2U + 1U] = to_i16(s_dl_r[rd]);
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

        out[i * 2U] = to_i16(dl * s_glim);
        out[i * 2U + 1U] = to_i16(dr * s_glim);
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
void dsp_set_compressor(bool on)
{
    s_comp_on = on;
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
    s_gr_max = 0.0f;
    s_lim_samples = 0;
    dsp_set_effects_level(100);
    dsp_clear();
}
