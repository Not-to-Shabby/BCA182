/**
 * ==============================================================================
 * Temporal Feature Extractor for Edge AI Activity Classification
 * ==============================================================================
 * Target: STM32F407ZGT6 (ARM Cortex-M4F with single-precision FPU)
 * Buffer: 15-sample rolling circular window (~2.8s at 5.4 Hz or 1.5s at 10 Hz)
 * Output: 21 time-domain features matching Kaggle run-or-walk model
 */

#ifndef FEATURE_EXTRACTOR_H
#define FEATURE_EXTRACTOR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ACTIVITY_WINDOW_SAMPLES 15
#define ACTIVITY_NUM_FEATURES   21

typedef struct {
    float ax; /* Acceleration X (g) */
    float ay; /* Acceleration Y (g) */
    float az; /* Acceleration Z (g) */
    float gx; /* Gyroscope X (rad/s) */
    float gy; /* Gyroscope Y (rad/s) */
    float gz; /* Gyroscope Z (rad/s) */
} imu_sample_t;

typedef struct {
    imu_sample_t samples[ACTIVITY_WINDOW_SAMPLES];
    uint8_t count;
    uint8_t head;
    
    /* Pedometer & Cadence state */
    uint32_t total_steps;
    uint32_t last_step_time_ms;
    uint8_t  current_cadence_spm;
    float    last_mag;
    bool     step_armed;
} feature_window_t;

/**
 * @brief Initializes the rolling feature window.
 */
void feature_extractor_init(feature_window_t *win);

/**
 * @brief Pushes a new 6-axis IMU sample into the rolling window.
 * 
 * @param win Pointer to window state
 * @param sample New 6-axis sample in calibrated units (g and rad/s)
 * @param timestamp_ms Current system timestamp in milliseconds
 * @return true if window is full and ready for feature extraction
 */
bool feature_extractor_push(feature_window_t *win, const imu_sample_t *sample, uint32_t timestamp_ms);

/**
 * @brief Computes 21 statistical features across the rolling window.
 * 
 * @param win Pointer to window state
 * @param out_features Array of size ACTIVITY_NUM_FEATURES (21 floats)
 */
void feature_extractor_compute(const feature_window_t *win, float *out_features);

/**
 * @brief Resets the sample buffer while keeping step count intact.
 */
void feature_extractor_reset_buffer(feature_window_t *win);

#ifdef __cplusplus
}
#endif

#endif /* FEATURE_EXTRACTOR_H */
