/**
 * @file app_context.h
 * @brief Shared state and inter-thread communication for BenchWatch.
 *
 * Data flow (matches the assignment's required separation of concerns):
 *   sensor_task --g_telemetry_q--> net_task        (readings to publish)
 *   net_task    --g_cmd_q------->  control_task    (validated remote commands)
 *   control_task --g_ack_q------->  net_task        (acks to publish)
 *   all tasks    --g_state_lock-->  g_state         (latest snapshot for UI)
 */

#ifndef APP_CONTEXT_H
#define APP_CONTEXT_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "telemetry.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Acknowledgment envelope produced by control_task, published by net_task. */
typedef struct {
    mqtt_command_t cmd;
    bool accepted;
    char reason[32];
    char actual[48];
    int64_t ts_ms;
} app_ack_t;

typedef struct {
    telemetry_snapshot_t snap;
    uint32_t drops_telemetry; /* sensor readings lost: queue full   */
    uint32_t rejects;         /* malformed/unknown commands received */
    uint32_t accepted_cmds;
} app_state_t;

void app_context_init(void);

/* Latest-snapshot store (mutex-protected, lock-free for the UI's purposes) */
void app_state_update(const telemetry_snapshot_t *snap);
void app_state_get(telemetry_snapshot_t *out);
app_state_t *app_state_raw(void); /* caller must hold g_state_lock */

/* Queues (all non-blocking put, optional-timeout get) */
bool app_telemetry_put(const telemetry_snapshot_t *snap);
bool app_telemetry_get(telemetry_snapshot_t *out, k_timeout_t timeout);
bool app_cmd_put(const mqtt_command_t *cmd);
bool app_cmd_get(mqtt_command_t *out, k_timeout_t timeout);
bool app_ack_put(const mqtt_command_t *cmd, bool accepted, const char *reason,
                 const char *actual);
bool app_ack_get(app_ack_t *out, k_timeout_t timeout);

/* Counters (atomic-ish; single-writer each) */
void app_count_drop(void);
void app_count_reject(void);
void app_count_accept(void);

extern struct k_mutex g_state_lock;

#ifdef __cplusplus
}
#endif

#endif /* APP_CONTEXT_H */
