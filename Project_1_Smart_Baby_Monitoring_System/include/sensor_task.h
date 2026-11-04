/**
 * @file sensor_task.h
 * @brief SENSOR layer entry point for the Smart Baby Monitoring System.
 *
 * Starts the statically defined sensor thread (prio 6, 2000 ms deadline) that
 * probes and polls the AHT10 / AP3216C / ICM20608, refreshes the shared
 * monitor_state_t snapshot, and re-runs the baby decision logic per cycle.
 */
#ifndef SENSOR_TASK_H_
#define SENSOR_TASK_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the sensor acquisition thread.
 *
 * The thread is created at build time via K_THREAD_DEFINE() and starts
 * automatically at boot, so this call is idempotent-safe: repeated or extra
 * invocations are no-ops (no dynamic thread creation, no double start).
 */
void sensor_task_start(void);

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_TASK_H_ */
