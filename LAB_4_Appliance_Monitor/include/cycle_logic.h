/**
 * @file cycle_logic.h
 * @brief Appliance cycle decision logic: baseline rules, trained-model wrapper,
 *        hysteresis state machine and DONE-event derivation.
 *
 * This module is pure C (no RTOS, no hardware) so the complete decision chain is
 * unit-testable on the host with the exact code that runs on the board.
 *
 * States: IDLE -> WASH -> SPIN are *measured* (the trained model classifies them);
 * DONE is a *derived* event: a verified SPIN phase of >= 8 s that ends in a
 * sustained quiet period produces exactly one DONE alert until acknowledged.
 */

#ifndef CYCLE_LOGIC_H
#define CYCLE_LOGIC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Measured appliance states (model classes) */
typedef enum {
    CYCLE_IDLE = 0,
    CYCLE_WASH = 1,
    CYCLE_SPIN = 2,
} cycle_measured_t;

/** Full product state (measured + derived DONE) */
typedef enum {
    CYCLE_STATE_IDLE = 0,
    CYCLE_STATE_WASH = 1,
    CYCLE_STATE_SPIN = 2,
    CYCLE_STATE_DONE = 3,
} cycle_state_t;

/** Decision source reported alongside the state */
typedef struct {
    cycle_state_t state;        /* Hysteresis-filtered product state */
    int model_class;            /* Raw trained-model class this window */
    int baseline_class;         /* Raw rule-based class this window */
    bool models_agree;          /* model_class == baseline_class */
    bool alert_active;          /* DONE alert waiting for acknowledgment */
    uint32_t state_age_ms;      /* Time since the current state was entered */
    uint32_t state_changes;     /* Total measured transitions since reset */
} cycle_decision_t;

/** Reset the state machine (call at boot and when entering BENCH mode). */
void cycle_logic_reset(void);

/**
 * @brief Feed one extracted feature window (VIB_NUM_FEATURES floats) to the chain.
 *
 * Internally: baseline rules -> trained forest -> 2-window hysteresis ->
 * DONE derivation. Call once per second (the window cadence).
 */
void cycle_logic_process(const float *features, uint32_t now_ms, cycle_decision_t *out);

/** Clear the DONE alert (KEY0). @return true if an alert was actually cleared. */
bool cycle_logic_acknowledge(void);

/** Current product state. */
cycle_state_t cycle_logic_state(void);

/** true while a DONE alert is unacknowledged. */
bool cycle_logic_is_alerting(void);

/** Time in the current state (ms). */
uint32_t cycle_logic_state_age_ms(uint32_t now_ms);

/** Rule-based baseline classifier (the ML-vs-baseline comparison partner). */
int cycle_logic_baseline_classify(const float *features);

/** Fixed text name of a product state ("IDLE", "WASH", "SPIN", "DONE"). */
const char *cycle_state_name(cycle_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* CYCLE_LOGIC_H */
