/**
 * @file mqtt_task.c
 * @brief STUB — filled in by the P4 connectivity agent. The build stays
 *        green (RAM-neutral: no networking CONFIG enabled yet); the real
 *        implementation publishes readings/events and subscribes to cmds.
 */
#include "mqtt_task.h"

#include <zephyr/kernel.h>

static void mqtt_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	for (;;) {
		k_sleep(K_MSEC(1000));
	}
}

K_THREAD_DEFINE(mqtt_thread, 2048, mqtt_thread_fn, NULL, NULL, NULL, 7, 0, 0);

void mqtt_task_start(void)
{
}

bool mqtt_task_is_connected(void)
{
	return false;
}
