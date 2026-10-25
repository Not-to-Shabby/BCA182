/**
 * @file cycle_model.c
 * @brief Thin wrapper binding cycle_logic to the emlearn-generated forest.
 */

#include "cycle_model.h"
#include "cycle_model_eml.h"
#include <stddef.h>

int cycle_model_predict(const float *features)
{
    if (features == NULL) {
        return 0;
    }
    return (int)cycle_forest_predict(features, CYCLE_MODEL_NUM_FEATURES);
}

const char *cycle_model_training_set(void)
{
    /* Authoritative provenance lives in the comment header of cycle_model_eml.h */
    return "SYNTHETIC bootstrap (regenerate with real captures)";
}
