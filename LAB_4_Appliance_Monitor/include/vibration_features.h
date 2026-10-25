/**
 * @file vibration_features.h
 * @brief Windowed vibration feature extractor for appliance cycle classification.
 *
 * A 200-sample (2 s @ 100 Hz) rolling window of IMU samples is reduced to 14
 * single-precision features that separate the washing machine's physical phases:
 * agitation is low-frequency sloshing (1-3 Hz), extract/spin is a strong
 * 10-25 Hz resonance, idle is a near-silent noise floor.
 */

#ifndef VIBRATION_FEATURES_H
#define VIBRATION_FEATURES_H

#include <stdint.h>
#include <stdbool.h>
#include "icm20608.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VIB_WINDOW_SAMPLES  200U    /* 2 s at 100 Hz */
#define VIB_NUM_FEATURES    14U

/* Feature vector layout */
#define FEAT_ACC_RMS        0   /* RMS of mean-removed accel magnitude (g) */
#define FEAT_ACC_P2P        1   /* Peak-to-peak of mean-removed magnitude (g) */
#define FEAT_ACC_CREST      2   /* Peak / RMS ratio (peakiness of shocks) */
#define FEAT_STD_X          3   /* Per-axis std (g) - motion direction spread */
#define FEAT_STD_Y          4
#define FEAT_STD_Z          5
#define FEAT_ZCR            6   /* Zero-crossing rate of mean-removed magnitude (crossings/sample) */
#define FEAT_BAND_LOW       7   /* Energy fraction 1-3 Hz (agitation slosh) */
#define FEAT_BAND_MID       8   /* Energy fraction 3-10 Hz (motor + drum rotation) */
#define FEAT_BAND_HIGH      9   /* Energy fraction 10-50 Hz (extract resonance) */
#define FEAT_DOM_FREQ       10  /* Dominant frequency 1-50 Hz (Hz) */
#define FEAT_GYRO_RMS       11  /* RMS of mean-removed gyro magnitude (rad/s) */
#define FEAT_GYRO_P2P       12  /* Peak-to-peak gyro magnitude (rad/s) */
#define FEAT_PEAK_CONC      13  /* Fraction of spectral energy within +/-20% of dominant bin */

typedef struct {
    imu_sample_t samples[VIB_WINDOW_SAMPLES];
    uint16_t count;             /* Filled since last extraction (wraps at window size) */
    uint16_t head;              /* Next write index */
    uint32_t dropped;           /* Samples lost because the ring was full */
} vib_window_t;

/** Clear the ring and feature state. */
void vib_features_reset(void);

/**
 * @brief Push one IMU sample into the rolling window.
 * @return true when the window is full and a feature extraction is due.
 */
bool vib_features_push(const imu_sample_t *sample);

/**
 * @brief Extract the 14 features from the current window contents.
 *
 * Extraction does NOT clear the ring: consecutive windows overlap by 50 % at the
 * 1 s inference cadence, giving the state machine faster reaction time.
 *
 * @param out_features Receives VIB_NUM_FEATURES floats in FEAT_* order.
 * @return true when the window held a full VIB_WINDOW_SAMPLES of data.
 */
bool vib_features_extract(float *out_features);

/** Samples currently retained (0..VIB_WINDOW_SAMPLES). */
uint16_t vib_features_count(void);

#ifdef __cplusplus
}
#endif

#endif /* VIBRATION_FEATURES_H */
