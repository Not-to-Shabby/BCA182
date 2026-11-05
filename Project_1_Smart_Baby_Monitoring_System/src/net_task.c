/**
 * @file net_task.c
 * @brief STUB — filled in by the P4 connectivity agent. The build stays
 *        green (RAM-neutral: no networking CONFIG enabled yet); the real
 *        implementation brings up RW007 + DHCP with a link supervisor.
 */
#include "net_task.h"

#include <zephyr/kernel.h>

static void net_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	for (;;) {
		k_sleep(K_MSEC(1000));
	}
}

K_THREAD_DEFINE(net_thread, 2048, net_thread_fn, NULL, NULL, NULL, 7, 0, 0);

void net_task_start(void)
{
}

bool net_task_is_online(void)
{
	return false;
}

const char *net_task_get_ip(void)
{
	return "0.0.0.0";
}
