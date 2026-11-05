/**
 * @file mqtt_task.h
 * @brief MQTT client task: publishes sensor readings and cry/env events,
 *        subscribes to the command topic (ack/mute). P4 Smart Baby Monitor.
 *
 * Topics:
 *   publish  babymonitor/reading  JSON: {"t":..,"h":..,"lux":..,"px":..,
 *                                        "mg":..,"ts":<ms>}
 *   publish  babymonitor/event    JSON: {"type":"cry"|"env"|"fault",
 *                                        "on":true|false,"conf":..,"ts":..}
 *   subscribe babymonitor/cmd     JSON: {"cmd":"ack"} | {"cmd":"mute",
 *                                               "sec":60}
 *
 * The task waits for net_task_is_online(), connects (no TLS in baseline:
 * LAN broker, documented trade-off), publishes periodically and on state
 * transitions, queues events in a small RAM ring while offline and
 * replays them on reconnect, and reconnects with exponential backoff.
 */
#ifndef MQTT_TASK_H_
#define MQTT_TASK_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** No-op: thread created at build time (K_THREAD_DEFINE); parks until
 *  net_task_is_online() before touching the broker. */
void mqtt_task_start(void);

/** True while the MQTT connection to the broker is live. */
bool mqtt_task_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_TASK_H_ */
