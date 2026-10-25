/**
 * @file vibration_features.c
 * @brief Windowed vibration statistics + Goertzel band analysis.
 *
 * All math is single-precision float suitable for the Cortex-M4F FPU. The window
 * is a circular buffer; extraction walks the samples oldest-first without copying.
 */

#include "vibration_features.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#if defined(__arm__) || defined(CONFIG_SOC_SERIES_STM32F4X) || defined(ZEPHYR_VERSION_CODE)
#include <zephyr/kernel.h>
/* Sampler thread pushes while the inference thread extracts: a spinlock guards
 * the ring; extraction copies the window out under the lock and computes
 * outside it so the 10 ms sampler deadline is never blocked by the math. */
static struct k_spinlock s_ring_lock;
typedef k_spinlock_key_t vib_lock_key_t;
#define VIB_LOCK(key) key = k_spin_lock(&s_ring_lock)
#define VIB_UNLOCK(key) k_spin_unlock(&s_ring_lock, key)
#else
typedef unsigned int vib_lock_key_t;
#define VIB_LOCK(key) (void)(key)
#define VIB_UNLOCK(key) (void)(key)
#endif

static vib_window_t s_window;
static imu_sample_t s_snapshot[VIB_WINDOW_SAMPLES]; /* only the inference thread extracts */

void vib_features_reset(void)
{
    memset(&s_window, 0, sizeof(s_window));
}

uint16_t vib_features_count(void)
{
    return s_window.count;
}

bool vib_features_push(const imu_sample_t *sample)
{
    if (sample == NULL) {
        return false;
    }

    vib_lock_key_t key;
    VIB_LOCK(key);

    if (s_window.count == VIB_WINDOW_SAMPLES) {
        /* Overwrite the oldest sample; the ring stays full for overlapping windows */
        s_window.dropped++;
    } else {
        s_window.count++;
    }

    s_window.samples[s_window.head] = *sample;
    s_window.head = (uint16_t)((s_window.head + 1U) % VIB_WINDOW_SAMPLES);

    bool full = s_window.count == VIB_WINDOW_SAMPLES;
    VIB_UNLOCK(key);
    return full;
}

bool vib_features_extract(float *out_features)
{
    if (out_features == NULL) {
        return false;
    }

    vib_lock_key_t key;
    VIB_LOCK(key);
    if (s_window.count < VIB_WINDOW_SAMPLES) {
        VIB_UNLOCK(key);
        return false;
    }
    memcpy(s_snapshot, s_window.samples, sizeof(s_snapshot));
    uint16_t head = s_window.head;
    VIB_UNLOCK(key);

    const uint16_t n = VIB_WINDOW_SAMPLES;

    /* Pass 1: means and extrema. The ring holds [oldest..newest]; the oldest
     * sample sits at s_window.head when the ring is full. */
    float sum_x = 0.0f, sum_y = 0.0f, sum_z = 0.0f;
    float sum_mag = 0.0f, min_mag = 1e30f, max_mag = -1e30f;
    float sum_gyr = 0.0f, min_gyr = 1e30f, max_gyr = -1e30f;
    float mag[VIB_WINDOW_SAMPLES];
    float gyr[VIB_WINDOW_SAMPLES];

    for (uint16_t i = 0; i < n; i++) {
        const imu_sample_t *s = &s_snapshot[(head + i) % VIB_WINDOW_SAMPLES];
        float m = sqrtf(s->ax * s->ax + s->ay * s->ay + s->az * s->az);
        float g = sqrtf(s->gx * s->gx + s->gy * s->gy + s->gz * s->gz);
        mag[i] = m;
        gyr[i] = g;
        sum_x += s->ax;
        sum_y += s->ay;
        sum_z += s->az;
        sum_mag += m;
        sum_gyr += g;
        if (m < min_mag) min_mag = m;
        if (m > max_mag) max_mag = m;
        if (g < min_gyr) min_gyr = g;
        if (g > max_gyr) max_gyr = g;
    }

    const float inv_n = 1.0f / (float)n;
    const float mean_x = sum_x * inv_n;
    const float mean_y = sum_y * inv_n;
    const float mean_z = sum_z * inv_n;
    const float mean_mag = sum_mag * inv_n;
    const float mean_gyr = sum_gyr * inv_n;

    /* Pass 2: mean-removed series, stds, RMS, crest, ZCR */
    float mm[VIB_WINDOW_SAMPLES];
    float gm[VIB_WINDOW_SAMPLES];
    float acc_ss = 0.0f, ss_x = 0.0f, ss_y = 0.0f, ss_z = 0.0f, gyr_ss = 0.0f;
    float peak = 0.0f;
    uint32_t crossings = 0;

    for (uint16_t i = 0; i < n; i++) {
        const imu_sample_t *s = &s_snapshot[(head + i) % VIB_WINDOW_SAMPLES];
        mm[i] = mag[i] - mean_mag;
        gm[i] = gyr[i] - mean_gyr;

        float dx = s->ax - mean_x;
        float dy = s->ay - mean_y;
        float dz = s->az - mean_z;
        ss_x += dx * dx;
        ss_y += dy * dy;
        ss_z += dz * dz;
        acc_ss += mm[i] * mm[i];
        gyr_ss += gm[i] * gm[i];

        float am = fabsf(mm[i]);
        if (am > peak) {
            peak = am;
        }
        if (i > 0 && ((mm[i - 1] < 0.0f && mm[i] >= 0.0f) || (mm[i - 1] >= 0.0f && mm[i] < 0.0f))) {
            crossings++;
        }
    }

    const float rms = sqrtf(acc_ss * inv_n);
    float crest = 0.0f;
    if (rms > 1e-6f) {
        crest = peak / rms;
    }

    /* Pass 3: Goertzel power over 1..50 Hz on the mean-removed magnitude.
     * Bin spacing equals the window rate (100 Hz / 200 samples = 0.5 Hz), so
     * bin f is analyzed at true frequency f * FS / N = f Hz for FS=100, N=200. */
    float power[51]; /* index = frequency in Hz, [0] unused */
    float total = 0.0f;
    float s0 = 0.0f; /* cppcheck-suppress unreadVariable ; final Goertzel iteration leaves it dead */
    for (int f = 1; f <= 50; f++) {
        float w = 2.0f * (float)M_PI * (float)f * (2.0f * inv_n); /* f Hz -> 2f cycles/window */
        float coeff = 2.0f * cosf(w);
        float s1 = 0.0f, s2 = 0.0f;
        for (uint16_t i = 0; i < n; i++) {
            s0 = mm[i] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        power[f] = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        total += power[f];
    }

    int dom = 1;
    for (int f = 2; f <= 50; f++) {
        if (power[f] > power[dom]) {
            dom = f;
        }
    }

    float low = 0.0f, mid = 0.0f, high = 0.0f, conc_num = 0.0f;
    for (int f = 1; f <= 50; f++) {
        if (f <= 3) {
            low += power[f];
        } else if (f <= 10) {
            mid += power[f];
        } else {
            high += power[f];
        }
        float lo = 0.8f * (float)dom;
        float hi = 1.2f * (float)dom;
        if ((float)f >= lo && (float)f <= hi) {
            conc_num += power[f];
        }
    }

    float low_frac = 0.0f, mid_frac = 0.0f, high_frac = 0.0f, conc = 0.0f;
    if (total > 1e-12f) {
        low_frac = low / total;
        mid_frac = mid / total;
        high_frac = high / total;
        conc = conc_num / total;
    }

    out_features[FEAT_ACC_RMS] = rms;
    out_features[FEAT_ACC_P2P] = max_mag - min_mag;
    out_features[FEAT_ACC_CREST] = crest;
    out_features[FEAT_STD_X] = sqrtf(ss_x * inv_n);
    out_features[FEAT_STD_Y] = sqrtf(ss_y * inv_n);
    out_features[FEAT_STD_Z] = sqrtf(ss_z * inv_n);
    out_features[FEAT_ZCR] = (float)crossings * inv_n;
    out_features[FEAT_BAND_LOW] = low_frac;
    out_features[FEAT_BAND_MID] = mid_frac;
    out_features[FEAT_BAND_HIGH] = high_frac;
    out_features[FEAT_DOM_FREQ] = (float)dom;
    out_features[FEAT_GYRO_RMS] = sqrtf(gyr_ss * inv_n);
    out_features[FEAT_GYRO_P2P] = max_gyr - min_gyr;
    out_features[FEAT_PEAK_CONC] = conc;
    return true;
}
