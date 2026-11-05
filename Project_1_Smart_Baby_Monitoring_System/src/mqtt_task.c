/**
 * @file mqtt_task.c
 * @brief STUB — filled in by the P4 connectivity agent. Deliberately
 *        RAM-neutral: no thread, no stack, no networking CONFIG yet.
 *        See src/net_task.c for the DTCM stack pattern and the RAM
 *        liberation plan (ring/record buffers -> DTCM frees ~32 KB).
 */
#include "mqtt_task.h"

void mqtt_task_start(void)
{
}

bool mqtt_task_is_connected(void)
{
	return false;
}
