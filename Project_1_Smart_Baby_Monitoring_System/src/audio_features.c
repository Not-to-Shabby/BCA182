/**
 * @file audio_features.c
 * @brief On-device 63-feature extractor -- bit-faithful C mirror of
 *        ml_model/extract_features.py (the PC-side trainer front end).
 *
 * Pure C (stdint + math only, no RTOS headers) so the same file compiles on
 * the board (the Zephyr CMake app glob compiles everything in src) and in
 * the host Unity suite (test/test_features includes it directly, LAB_4
 * pattern).
 *
 * Python -> C correspondence, stage by stage (extract_features.py):
 *
 *  1. int16 -> float   load_wav() reads s16 as `np.int16 -> float64 / 32768`.
 *      Every int16/32768 value is exactly representable in float32, so
 *      C computes `(float)sample / 32768.0f` with zero conversion error.
 *  2. framing          `_frame_features()`: n_frames = 1 + (16000-512)/256
 *      = 61 frames of 512 samples, hop 256, samples x[idx] * HANN.
 *      Hann: periodic, w[k] = 0.5 - 0.5*cos(2*pi*k/512) -- table
 *      audio_feat_hann512[] generated from the trainer's float64 HANN.
 *  3. FFT              `np.fft.rfft(frames)` (float64) -> C: 512-point
 *      iterative radix-2 DIT complex FFT with zero imaginary parts
 *      (float32 arithmetic, twiddles precomputed by audio_features_init());
 *      bins 0..256 are the rfft bins.
 *  4. power spectrum   `power = (re^2 + im^2) / N_FFT` (line 237).
 *  5. mel              `mel = log(power @ MEL_FB.T + LOG_FLOOR_MEL)` (line
 *      239): dot product with the 40x257 table, natural log, +1e-10 floor.
 *  6. MFCC             `mfcc = mel @ DCT_MAT.T` (line 240): orthonormal
 *      DCT-II matrix (13x40), so mfcc[k] = sum_m mel[m]*D[k][m].
 *  7. frame energy     TIME-domain sum of squares of the WINDOWED frame
 *      (`frame_energy = (frames**2).sum(axis=1)`, line 242 -- not the
 *      spectral sum), then `log(energy + LOG_FLOOR_ENERGY)` with a 1e-12
 *      floor (line 243).
 *  8. ZCR              per frame on the WINDOWED frame samples via
 *      signbit (`signs = np.signbit(frames)`, line 245-246): fraction of
 *      adjacent sign changes among the N_FFT-1 = 511 pairs.  IEEE
 *      signed-zero rules (x*0.0 -> +0, (-x)*0.0 -> -0) make the C sign bits
 *      identical to numpy's signbit even at the hann[0] == 0 tap.
 *  9. deltas           feature-set v2 temporal dynamics: per-frame first
 *      difference with EDGE REPLICATION, d[t] = (m'[t+1] - m'[t-1]) / 2
 *      where m' pads the frame sequence with a copy of its first/last
 *      frame (interior t: (m[t+1]-m[t-1])/2; d[0] = (m[1]-m[0])/2;
 *      d[60] = (m[60]-m[59])/2), applied to each of the 13 MFCC frame
 *      sequences and to the log-energy sequence
 *      (extract_features._first_delta).
 *  10. spectral shape  per frame on the raw 257-bin power spectrum (bin k
 *      centered at k * 31.25 Hz = k * AUDIO_TABLES_BIN_HZ):
 *      centroid = sum_k(f_k * P_k) / sum_k(P_k); rolloff(0.85) = frequency
 *      of the first bin whose running power sum reaches 0.85 * total
 *      (AF_ROLLOFF_FRAC); flatness = exp(mean_k(log P_k)) / mean_k(P_k).
 *      A zero-power frame (digital silence) defines all three as 0, and a
 *      single zero-power bin makes flatness 0 (-inf log mean) -- exactly
 *      extract_features._spectral_shape, guards included.
 *  11. aggregate       per 1 s window over the 61 frames, population std
 *      (ddof=0) everywhere, emitted in FEATURE_NAMES order:
 *        0-12   mean of each MFCC            (v1 block, unchanged)
 *        13-25  std of each MFCC
 *        26/27  log-energy mean/std
 *        28     mean frame ZCR
 *        29-41  mean of each MFCC delta      (v2 temporal dynamics)
 *        42-54  std of each MFCC delta
 *        55/56  log-energy-delta mean/std
 *        57/58  spectral centroid mean/std
 *        59/60  spectral rolloff(0.85) mean/std
 *        61/62  spectral flatness mean/std
 *
 * Parity is enforced by test/test_features against golden vectors produced
 * by ml_model/gen_feature_golden.py running the real Python pipeline.
 *
 * Memory model: no malloc anywhere.  audio_features_init() fills a
 * 256-entry complex twiddle table; audio_features_compute() uses a private
 * static workspace (about 9.5 KB with the twiddles and the per-frame MFCC /
 * log-energy history needed for the v2 deltas) instead of the stack so
 * small Zephyr thread stacks are safe.  On the board the twiddles, the
 * frame buffers and the histories are placed in the unused 64 KB DTCM
 * region (AF_DTCM_WORKSPACE) so the extractor adds effectively nothing to
 * the nearly-full main SRAM region.  The workspace is fully rewritten on
 * every call and never carries information between calls, and no
 * configuration state exists outside init(), so results depend only on the
 * input window (deterministic).  Not reentrant: the single intended caller
 * is the cry task.
 */

#include <stdint.h>
#include <math.h>

#include "audio_features.h"
#include "audio_features_tables.h"

/* --------- constants mirrored 1:1 from ml_model/extract_features.py ------- */
#define AF_N_FFT            512u    /* N_FFT  (== AUDIO_TABLES_N_FFT)        */
#define AF_HOP              256u    /* HOP    (== AUDIO_TABLES_HOP)          */
#define AF_N_MELS           40u     /* N_MELS (== AUDIO_TABLES_N_MELS)       */
#define AF_N_BINS           257u    /* N_FFT/2+1 (== AUDIO_TABLES_N_BINS)    */
#define AF_N_MFCC           13u     /* N_MFCC (== AUDIO_TABLES_N_MFCC)       */
#define AF_N_TWIDDLE        256u    /* AF_N_FFT / 2                          */
#define AF_N_FRAMES         61u     /* 1 + (16000 - 512) / 256               */
#define AF_ZCR_PAIRS        511u    /* N_FFT - 1 adjacent pairs per frame    */
#define AF_LOG_FLOOR_MEL    1e-10   /* LOG_FLOOR_MEL   (mel energy floor)    */
#define AF_LOG_FLOOR_ENERGY 1e-12   /* LOG_FLOOR_ENERGY (frame energy floor) */
#define AF_BIN_HZ           ((double)AUDIO_TABLES_BIN_HZ) /* FS/N_FFT = 31.25 */
#define AF_ROLLOFF_FRAC     0.85    /* ROLLOFF_FRAC (spectral rolloff)       */

/* Output layout offsets = FEATURE_NAMES order (extract_features.py, v2). */
#define AF_IDX_MFCC_MEAN     0u     /* [0..12]  mean of each MFCC            */
#define AF_IDX_MFCC_STD      13u    /* [13..25] population std of each MFCC  */
#define AF_IDX_LE_MEAN       26u    /* mean frame log-energy                 */
#define AF_IDX_LE_STD        27u    /* std of frame log-energy               */
#define AF_IDX_ZCR_MEAN      28u    /* mean frame zero-crossing rate         */
#define AF_IDX_MFCCD_MEAN    29u    /* [29..41] mean of each MFCC delta      */
#define AF_IDX_MFCCD_STD     42u    /* [42..54] std of each MFCC delta       */
#define AF_IDX_LED_MEAN      55u    /* mean log-energy delta                 */
#define AF_IDX_LED_STD       56u    /* std of log-energy delta               */
#define AF_IDX_CENTROID_MEAN 57u    /* spectral centroid mean [Hz]           */
#define AF_IDX_CENTROID_STD  58u    /* spectral centroid std                 */
#define AF_IDX_ROLLOFF_MEAN  59u    /* spectral rolloff(0.85) mean [Hz]      */
#define AF_IDX_ROLLOFF_STD   60u    /* spectral rolloff(0.85) std            */
#define AF_IDX_FLATNESS_MEAN 61u    /* spectral flatness mean                */
#define AF_IDX_FLATNESS_STD  62u    /* spectral flatness std                 */

_Static_assert(AUDIO_TABLES_N_FFT == 512, "table geometry: N_FFT");
_Static_assert(AUDIO_TABLES_HOP == 256, "table geometry: HOP");
_Static_assert(AUDIO_TABLES_N_MELS == 40, "table geometry: N_MELS");
_Static_assert(AUDIO_TABLES_N_BINS == 257, "table geometry: N_BINS");
_Static_assert(AUDIO_TABLES_N_MFCC == 13, "table geometry: N_MFCC");
_Static_assert(AUDIO_TABLES_BIN_HZ > 31.24 && AUDIO_TABLES_BIN_HZ < 31.26,
               "table geometry: BIN_HZ");
_Static_assert(AUDIO_FEAT_COUNT == 63u, "feature layout: AUDIO_FEAT_COUNT");
_Static_assert(AF_IDX_FLATNESS_STD + 1u == AUDIO_FEAT_COUNT,
               "feature layout: last offset");

/* On the Zephyr/STM32 build the FFT workspace lives in the otherwise
 * unused 64 KB DTCM region (core-coupled, zero wait states, unreachable
 * by DMA - fine: this data is CPU-only) instead of the main SRAM region,
 * whose budget is nearly full.  This keeps the board RAM footprint of the
 * extractor at effectively zero.  Everything placed there is fully
 * written before it is read (twiddles by init(), the frame buffers and
 * the per-frame histories by compute()), so no startup zeroing is
 * required (dtcm_noinit semantics).  On the host build (non-ARM
 * toolchain) the attribute compiles out and these are plain statics. */
#if defined(__arm__)
#define AF_DTCM_WORKSPACE __attribute__((section(".dtcm_noinit")))
#else
#define AF_DTCM_WORKSPACE
#endif

/* exp(-j*2*pi*k/512), k = 0..255 -- precomputed by audio_features_init(). */
static AF_DTCM_WORKSPACE float twiddle_re[AF_N_TWIDDLE];
static AF_DTCM_WORKSPACE float twiddle_im[AF_N_TWIDDLE];
static uint8_t twiddles_ready;

/* Per-frame histories for the v2 deltas, coefficient-major so each MFCC
 * sequence is contiguous (61 floats per coefficient). */
static AF_DTCM_WORKSPACE float mfcc_hist[AF_N_MFCC][AF_N_FRAMES];
static AF_DTCM_WORKSPACE float le_hist[AF_N_FRAMES];

void audio_features_init(void)
{
    for (uint32_t k = 0u; k < AF_N_TWIDDLE; k++) {
        const double ang = -2.0 * 3.14159265358979323846 * (double)k / 512.0;
        twiddle_re[k] = (float)cos(ang);
        twiddle_im[k] = (float)sin(ang);
    }
    twiddles_ready = 1u;
}

/* 9-bit reversal for the 512-point FFT input permutation. */
static uint32_t fft_bit_reverse9(uint32_t x)
{
    uint32_t r = 0u;
    for (uint32_t b = 0u; b < 9u; b++) {
        r = (r << 1) | (x & 1u);
        x >>= 1;
    }
    return r;
}

/* In-place iterative radix-2 DIT FFT, N = 512, forward transform
 * (exp(-j*...), same sign convention as np.fft).  Twiddle W_len^k ==
 * W_512^(k*512/len) == twiddle_[k * stride].  Only bins 0..256 are used
 * afterwards (the rfft bins for the real-valued input frame). */
static void fft512_inplace(float *re, float *im)
{
    for (uint32_t i = 0u; i < AF_N_FFT; i++) {
        const uint32_t j = fft_bit_reverse9(i);
        if (j > i) {
            float t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }

    for (uint32_t len = 2u; len <= AF_N_FFT; len <<= 1) {
        const uint32_t half = len >> 1;
        const uint32_t stride = AF_N_FFT / len;
        for (uint32_t base = 0u; base < AF_N_FFT; base += len) {
            for (uint32_t k = 0u; k < half; k++) {
                const uint32_t w = k * stride;
                const float wr = twiddle_re[w];
                const float wi = twiddle_im[w];
                float *ar = &re[base + k];
                float *ai = &im[base + k];
                float *br = &re[base + k + half];
                float *bi = &im[base + k + half];
                const float tr = (wr * (*br)) - (wi * (*bi));
                const float ti = (wr * (*bi)) + (wi * (*br));
                *br = (*ar) - tr;
                *bi = (*ai) - ti;
                *ar += tr;
                *ai += ti;
            }
        }
    }
}

/* Population std (ddof=0) from accumulated value and square sums, with the
 * rounding-residue clamp the trainer relies on for constant frames. */
static float af_std_from_sums(double sq_sum, double mean)
{
    double var = sq_sum / (double)AF_N_FRAMES - mean * mean;
    if (var < 0.0) {
        var = 0.0; /* clamp rounding residue for constant frames */
    }
    return (float)sqrt(var);
}

/* Mean + population std of the first difference (edge replication) of one
 * per-frame sequence: d[t] = (m'[t+1] - m'[t-1]) / 2 where m' pads the
 * sequence with copies of its first/last element -- exactly
 * extract_features._first_delta() followed by mean/std aggregation. */
static void af_delta_stats(const float *hist, double *out_mean,
                           double *out_std)
{
    double sum = 0.0;
    double sq = 0.0;

    for (uint32_t t = 0u; t < AF_N_FRAMES; t++) {
        const double left = (t == 0u) ? (double)hist[0]
                                      : (double)hist[t - 1u];
        const double right = (t == AF_N_FRAMES - 1u)
                                 ? (double)hist[AF_N_FRAMES - 1u]
                                 : (double)hist[t + 1u];
        const double d = 0.5 * (right - left);
        sum += d;
        sq += d * d;
    }
    *out_mean = sum / (double)AF_N_FRAMES;
    *out_std = af_std_from_sums(sq, *out_mean);
}

void audio_features_compute(const int16_t *window, uint32_t n, float *out)
{
    /* Private workspace: static instead of ~4.1 KB of stack so small
     * Zephyr thread stacks are safe (and, on the board, placed in DTCM -
     * see AF_DTCM_WORKSPACE above).  Fully rewritten on every call, no
     * state carried between calls; single-caller design (cry task). */
    static AF_DTCM_WORKSPACE float fft_re[AF_N_FFT];
    static AF_DTCM_WORKSPACE float fft_im[AF_N_FFT];
    double mel_log[AF_N_MELS];
    double mfcc_sum[AF_N_MFCC];
    double mfcc_sq[AF_N_MFCC];
    double le_sum = 0.0;
    double le_sq = 0.0;
    double zcr_sum = 0.0;
    /* v2 spectral-shape accumulators (mean + population std per feature). */
    double cent_sum = 0.0;
    double cent_sq = 0.0;
    double roll_sum = 0.0;
    double roll_sq = 0.0;
    double flat_sum = 0.0;
    double flat_sq = 0.0;
    double dm;
    double ds;
    const double n_frames = (double)AF_N_FRAMES;
    uint32_t i;

    if ((window == NULL) || (out == NULL) || (n != AUDIO_FEAT_WINDOW_SAMPLES)) {
        if (out != NULL) {
            for (i = 0u; i < AUDIO_FEAT_COUNT; i++) {
                out[i] = 0.0f;
            }
        }
        return;
    }
    if (twiddles_ready == 0u) {
        audio_features_init(); /* defensive; the contract says call it first */
    }

    for (i = 0u; i < AF_N_MFCC; i++) {
        mfcc_sum[i] = 0.0;
        mfcc_sq[i] = 0.0;
    }

    for (uint32_t f = 0u; f < AF_N_FRAMES; f++) {
        const int16_t *src = &window[f * AF_HOP];
        double energy = 0.0;
        uint32_t zcr_changes = 0u;
        int prev_sign;

        /* (1) int16 -> float (/32768, exact for every int16, as load_wav)
         * and periodic Hann windowing.  The windowed frame feeds the FFT,
         * the frame energy and the ZCR -- exactly the three uses of
         * `frames` in extract_features.py:_frame_features(). */
        prev_sign = 0;
        for (i = 0u; i < AF_N_FFT; i++) {
            const float s = ((float)src[i] / 32768.0f) * audio_feat_hann512[i];
            const int sign = signbit(s) ? 1 : 0;
            fft_re[i] = s;
            fft_im[i] = 0.0f;
            energy += (double)s * (double)s;        /* (frames**2).sum() */
            if ((i > 0u) && (sign != prev_sign)) {
                zcr_changes++;                       /* adjacent sign flip */
            }
            prev_sign = sign;
        }

        /* (2) 512-pt FFT -> power spectrum (re^2 + im^2) / N_FFT,
         * stored in-place into bins 0..256. */
        fft512_inplace(fft_re, fft_im);
        for (i = 0u; i < AF_N_BINS; i++) {
            fft_re[i] = (fft_re[i] * fft_re[i] + fft_im[i] * fft_im[i]) *
                        (1.0f / 512.0f);
        }

        /* (2b) v2 spectral shape on the raw power spectrum (stage 10 of
         * the file header).  Accumulated in double for parity with
         * numpy's float64 reductions.  A zero-power frame contributes 0
         * to every accumulator (guards -> all three features 0). */
        {
            double p_sum = 0.0;
            double wf_sum = 0.0;
            double log_sum = 0.0;

            for (i = 0u; i < AF_N_BINS; i++) {
                const double p = (double)fft_re[i];
                p_sum += p;
                wf_sum += p * ((double)i * AF_BIN_HZ);
                log_sum += log(p);  /* log(0) = -inf, as numpy: geo -> 0 */
            }
            if (p_sum > 0.0) {
                const double centroid = wf_sum / p_sum;
                const double geo = exp(log_sum / (double)AF_N_BINS);
                const double flat = geo / (p_sum / (double)AF_N_BINS);
                double roll = 0.0;
                double acc = 0.0;

                for (i = 0u; i < AF_N_BINS; i++) {
                    acc += (double)fft_re[i];
                    if (acc >= AF_ROLLOFF_FRAC * p_sum) {
                        break;
                    }
                }
                roll = (double)i * AF_BIN_HZ; /* p_sum >= 0.85*p_sum */

                cent_sum += centroid;
                cent_sq += centroid * centroid;
                roll_sum += roll;
                roll_sq += roll * roll;
                flat_sum += flat;
                flat_sq += flat * flat;
            }
        }

        /* (3) 40-mel filterbank + natural log with the 1e-10 floor.
         * Accumulated in double for parity with numpy's float64 BLAS. */
        for (uint32_t m = 0u; m < AF_N_MELS; m++) {
            const float *fb = audio_feat_mel_fb[m];
            double acc = 0.0;
            for (i = 0u; i < AF_N_BINS; i++) {
                acc += (double)fft_re[i] * (double)fb[i];
            }
            mel_log[m] = log(acc + AF_LOG_FLOOR_MEL);
        }

        /* (4) orthonormal DCT-II -> 13 MFCCs; accumulate window sums and
         * keep the frame sequence for the v2 deltas (stage 9). */
        for (uint32_t j = 0u; j < AF_N_MFCC; j++) {
            const float *dct_row = audio_feat_dct_mat[j];
            double v = 0.0;
            for (uint32_t m = 0u; m < AF_N_MELS; m++) {
                v += mel_log[m] * (double)dct_row[m];
            }
            mfcc_sum[j] += v;
            mfcc_sq[j] += v * v;
            mfcc_hist[j][f] = (float)v;
        }

        /* (5) frame log-energy (time-domain windowed energy, 1e-12 floor)
         * and frame ZCR (sign changes / 511 pairs). */
        {
            const double le = log(energy + AF_LOG_FLOOR_ENERGY);
            le_sum += le;
            le_sq += le * le;
            le_hist[f] = (float)le;
            zcr_sum += (double)zcr_changes / (double)AF_ZCR_PAIRS;
        }
    }

    /* (6) aggregate the v1 block: population std (ddof=0), FEATURE_NAMES
     * order, indices 0-28 unchanged from feature-set v1. */
    for (uint32_t j = 0u; j < AF_N_MFCC; j++) {
        const double mean = mfcc_sum[j] / n_frames;
        out[AF_IDX_MFCC_MEAN + j] = (float)mean;
        out[AF_IDX_MFCC_STD + j] = af_std_from_sums(mfcc_sq[j], mean);
    }
    {
        const double le_mean = le_sum / n_frames;
        out[AF_IDX_LE_MEAN] = (float)le_mean;
        out[AF_IDX_LE_STD] = af_std_from_sums(le_sq, le_mean);
    }
    out[AF_IDX_ZCR_MEAN] = (float)(zcr_sum / n_frames);

    /* (7) v2 deltas: MFCC delta mean/std (29-54) and log-energy delta
     * mean/std (55-56) from the per-frame histories. */
    for (uint32_t j = 0u; j < AF_N_MFCC; j++) {
        af_delta_stats(mfcc_hist[j], &dm, &ds);
        out[AF_IDX_MFCCD_MEAN + j] = (float)dm;
        out[AF_IDX_MFCCD_STD + j] = (float)ds;
    }
    af_delta_stats(le_hist, &dm, &ds);
    out[AF_IDX_LED_MEAN] = (float)dm;
    out[AF_IDX_LED_STD] = (float)ds;

    /* (8) v2 spectral-shape aggregates (57-62). */
    {
        const double cent_mean = cent_sum / n_frames;
        const double roll_mean = roll_sum / n_frames;
        const double flat_mean = flat_sum / n_frames;
        out[AF_IDX_CENTROID_MEAN] = (float)cent_mean;
        out[AF_IDX_CENTROID_STD] = af_std_from_sums(cent_sq, cent_mean);
        out[AF_IDX_ROLLOFF_MEAN] = (float)roll_mean;
        out[AF_IDX_ROLLOFF_STD] = af_std_from_sums(roll_sq, roll_mean);
        out[AF_IDX_FLATNESS_MEAN] = (float)flat_mean;
        out[AF_IDX_FLATNESS_STD] = af_std_from_sums(flat_sq, flat_mean);
    }
}
