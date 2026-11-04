/**
 * @file cry_task.h
 * @brief On-device baby-cry classification task.
 *
 * Drains the 1 s audio ring (audio_capture.c), extracts the 29-feature
 * vector (audio_features.c), runs the emlearn forest (cry_model.h) and
 * applies the spec's confirmation rule: confidence >= CRY_CONF_THRESHOLD
 * on at least 2 of the last 3 consecutive 1 s windows. The confirmed flag
 * lands in monitor_state (cry_detected/cry_confidence) and drives the
 * system state machine (cry -> ALERT, with buzzer/LED/LCD via the
 * existing alert machinery).
 */
#ifndef CRY_TASK_H_
#define CRY_TASK_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Confidence (CRY-vote %) above which a 1 s window counts as "cry".
 * 70 was too strict for the v1 model (frozen-test recall 0.29): 50 opens
 * the gate for live diagnosis; the retrained model ships with its own
 * val-tuned value. */
#define CRY_CONF_THRESHOLD 75u

/**
 * No-op by design: the thread is created at build time via
 * K_THREAD_DEFINE() and starts automatically at boot; the explicit call
 * site in main() documents the bring-up order.
 */
void cry_task_start(void);

#ifdef __cplusplus
}
#endif

#endif /* CRY_TASK_H_ */
