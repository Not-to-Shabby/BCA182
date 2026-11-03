/**
 * @file app_context.c
 * @brief Static IPC objects (queues, mutex, shared state) for BenchWatch.
 */

#include "app_context.h"

#include <string.h>

K_MSGQ_DEFINE(g_telemetry_q, sizeof(telemetry_snapshot_t), 8, 4);
K_MSGQ_DEFINE(g_cmd_q, sizeof(mqtt_command_t), 4, 4);
K_MSGQ_DEFINE(g_ack_q, sizeof(app_ack_t), 4, 4);

K_MUTEX_DEFINE(g_state_lock);

static app_state_t g_state;

void app_context_init(void)
{
    k_msgq_purge(&g_telemetry_q);
    k_msgq_purge(&g_cmd_q);
    k_msgq_purge(&g_ack_q);
    memset(&g_state, 0, sizeof(g_state));
}

void app_state_update(const telemetry_snapshot_t *snap)
{
    if (snap == NULL) {
        return;
    }
    k_mutex_lock(&g_state_lock, K_FOREVER);
    g_state.snap = *snap;
    k_mutex_unlock(&g_state_lock);
}

void app_state_get(telemetry_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    k_mutex_lock(&g_state_lock, K_FOREVER);
    *out = g_state.snap;
    k_mutex_unlock(&g_state_lock);
}

app_state_t *app_state_raw(void)
{
    return &g_state;
}

bool app_telemetry_put(const telemetry_snapshot_t *snap)
{
    return k_msgq_put(&g_telemetry_q, snap, K_NO_WAIT) == 0;
}

bool app_telemetry_get(telemetry_snapshot_t *out, k_timeout_t timeout)
{
    return k_msgq_get(&g_telemetry_q, out, timeout) == 0;
}

bool app_cmd_put(const mqtt_command_t *cmd)
{
    return k_msgq_put(&g_cmd_q, cmd, K_NO_WAIT) == 0;
}

bool app_cmd_get(mqtt_command_t *out, k_timeout_t timeout)
{
    return k_msgq_get(&g_cmd_q, out, timeout) == 0;
}

bool app_ack_put(const mqtt_command_t *cmd, bool accepted, const char *reason,
                 const char *actual)
{
    app_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    ack.cmd = *cmd;
    ack.accepted = accepted;
    ack.ts_ms = k_uptime_get();
    if (reason != NULL) {
        strncpy(ack.reason, reason, sizeof(ack.reason) - 1);
    }
    if (actual != NULL) {
        strncpy(ack.actual, actual, sizeof(ack.actual) - 1);
    }
    return k_msgq_put(&g_ack_q, &ack, K_NO_WAIT) == 0;
}

bool app_ack_get(app_ack_t *out, k_timeout_t timeout)
{
    return k_msgq_get(&g_ack_q, out, timeout) == 0;
}

void app_count_drop(void)
{
    k_mutex_lock(&g_state_lock, K_FOREVER);
    g_state.drops_telemetry++;
    k_mutex_unlock(&g_state_lock);
}

void app_count_reject(void)
{
    k_mutex_lock(&g_state_lock, K_FOREVER);
    g_state.rejects++;
    k_mutex_unlock(&g_state_lock);
}

void app_count_accept(void)
{
    k_mutex_lock(&g_state_lock, K_FOREVER);
    g_state.accepted_cmds++;
    k_mutex_unlock(&g_state_lock);
}
