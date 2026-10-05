/**
 * ==============================================================================
 * Temporal Feature Extractor Implementation
 * ==============================================================================
 */

#include "feature_extractor.h"
#include <string.h>
#include <math.h>

#define STEP_THRESHOLD_HIGH  1.40f  /* Minimum g-force magnitude to trigger step peak */
#define STEP_THRESHOLD_LOW   1.05f  /* Hysteresis reset threshold */
#define MIN_STEP_INTERVAL_MS 220    /* Maximum realistic cadence ~270 spm */
#define MAX_STEP_INTERVAL_MS 2000   /* Minimum realistic cadence ~30 spm */

void feature_extractor_init(feature_window_t *win)
{
    if (!win) return;
    memset(win, 0, sizeof(feature_window_t));
    win->last_mag = 1.0f;
    win->step_armed = true;
}

void feature_extractor_reset_buffer(feature_window_t *win)
{
    if (!win) return;
    win->count = 0;
    win->head = 0;
}

bool feature_extractor_push(feature_window_t *win, const imu_sample_t *sample, uint32_t timestamp_ms)
{
    if (!win || !sample) return false;

    /* Store sample in circular buffer */
    win->samples[win->head] = *sample;
    win->head = (uint8_t)((win->head + 1) % ACTIVITY_WINDOW_SAMPLES);
    if (win->count < ACTIVITY_WINDOW_SAMPLES) {
        win->count++;
    }

    /* Pedometer Peak Detection Logic */
    float mag = sqrtf(sample->ax * sample->ax + sample->ay * sample->ay + sample->az * sample->az);
    
    if (win->step_armed && mag > STEP_THRESHOLD_HIGH && (mag > win->last_mag)) {
        if (win->last_step_time_ms == 0) {
            /* First step: record timestamp only */
            win->total_steps++;
            win->step_armed = false;
            win->last_step_time_ms = timestamp_ms;
        } else {
            uint32_t delta_t = timestamp_ms - win->last_step_time_ms;
            if (delta_t >= MIN_STEP_INTERVAL_MS) {
                win->total_steps++;
                win->step_armed = false;
                
                if (delta_t <= MAX_STEP_INTERVAL_MS) {
                    /* Cadence = 60000 ms / delta_t */
                    uint32_t inst_cadence = 60000u / delta_t;
                    if (inst_cadence > 255u) inst_cadence = 255u;
                    /* Exponential moving average for smooth cadence with direct initialization */
                    if (win->current_cadence_spm == 0) {
                        win->current_cadence_spm = (uint8_t)inst_cadence;
                    } else {
                        win->current_cadence_spm = (uint8_t)((win->current_cadence_spm * 3 + inst_cadence) / 4);
                    }
                }
                win->last_step_time_ms = timestamp_ms;
            }
        }
    } else if (!win->step_armed && mag < STEP_THRESHOLD_LOW) {
        win->step_armed = true;
    }
    
    /* Decaying cadence if user stopped moving */
    if (timestamp_ms - win->last_step_time_ms > MAX_STEP_INTERVAL_MS) {
        win->current_cadence_spm = 0;
    }

    win->last_mag = mag;

    return (win->count >= ACTIVITY_WINDOW_SAMPLES);
}

void feature_extractor_compute(const feature_window_t *win, float *out_features)
{
    if (!win || !out_features || win->count == 0) return;

    const uint8_t N = win->count;
    const float inv_N = 1.0f / (float)N;

    float sum[6] = {0};
    float min_val[6];
    float max_val[6];

    /* Initialize min/max with first sample */
    const imu_sample_t *s0 = &win->samples[0];
    min_val[0] = max_val[0] = s0->ax;
    min_val[1] = max_val[1] = s0->ay;
    min_val[2] = max_val[2] = s0->az;
    min_val[3] = max_val[3] = s0->gx;
    min_val[4] = max_val[4] = s0->gy;
    min_val[5] = max_val[5] = s0->gz;

    /* First pass: Sum and Min/Max */
    for (uint8_t i = 0; i < N; i++) {
        const imu_sample_t *s = &win->samples[i];
        const float vals[6] = { s->ax, s->ay, s->az, s->gx, s->gy, s->gz };

        for (uint8_t k = 0; k < 6; k++) {
            sum[k] += vals[k];
            if (vals[k] < min_val[k]) min_val[k] = vals[k];
            if (vals[k] > max_val[k]) max_val[k] = vals[k];
        }
    }

    /* Compute means */
    float mean[6];
    for (uint8_t k = 0; k < 6; k++) {
        mean[k] = sum[k] * inv_N;
    }

    /* Second pass: Standard deviation & Acceleration Magnitude */
    float var_sum[6] = {0};
    float mag_sum = 0.0f;
    float mag_max = 0.0f;
    float mag_vals[ACTIVITY_WINDOW_SAMPLES];

    for (uint8_t i = 0; i < N; i++) {
        const imu_sample_t *s = &win->samples[i];
        const float vals[6] = { s->ax, s->ay, s->az, s->gx, s->gy, s->gz };

        for (uint8_t k = 0; k < 6; k++) {
            float diff = vals[k] - mean[k];
            var_sum[k] += diff * diff;
        }

        float mag = sqrtf(s->ax * s->ax + s->ay * s->ay + s->az * s->az);
        mag_vals[i] = mag;
        mag_sum += mag;
        if (mag > mag_max) mag_max = mag;
    }

    float mag_mean = mag_sum * inv_N;
    float mag_var_sum = 0.0f;
    for (uint8_t i = 0; i < N; i++) {
        float diff = mag_vals[i] - mag_mean;
        mag_var_sum += diff * diff;
    }
    float mag_std = sqrtf(mag_var_sum * inv_N);

    /* Output packaging:
     * [0..5]:   Means
     * [6..11]:  Standard deviations
     * [12..17]: Ranges (max - min)
     * [18]:     mag_mean
     * [19]:     mag_std
     * [20]:     mag_max
     */
    for (uint8_t k = 0; k < 6; k++) {
        out_features[k]      = mean[k];
        out_features[k + 6]  = sqrtf(var_sum[k] * inv_N);
        out_features[k + 12] = max_val[k] - min_val[k];
    }
    out_features[18] = mag_mean;
    out_features[19] = mag_std;
    out_features[20] = mag_max;
}
