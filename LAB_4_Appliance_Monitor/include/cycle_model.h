/**
 * @file cycle_model.h
 * @brief Wrapper for the emlearn-generated random forest (ml_model/cycle_model_eml.h).
 *
 * The forest classifies one VIB_NUM_FEATURES window into CYCLE_IDLE / CYCLE_WASH /
 * CYCLE_SPIN. Regenerate with `python ml_model/train_cycle_model.py --real <csv...>`.
 */

#ifndef CYCLE_MODEL_H
#define CYCLE_MODEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CYCLE_MODEL_NUM_FEATURES 14
#define CYCLE_MODEL_NUM_TREES    10

/**
 * @brief Classify one feature window.
 * @return 0 = IDLE, 1 = WASH, 2 = SPIN (matches cycle_measured_t).
 */
int cycle_model_predict(const float *features);

/** Metadata for the diagnostics screen / lab report. */
const char *cycle_model_training_set(void);

#ifdef __cplusplus
}
#endif

#endif /* CYCLE_MODEL_H */
