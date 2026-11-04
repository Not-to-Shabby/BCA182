/**
 * @file monitor_state.c
 * @brief Mutex-guarded shared system snapshot (single lock for the app).
 */
#include "monitor_state.h"

#include <string.h>
#include <zephyr/kernel.h>

static K_MUTEX_DEFINE(s_state_lock);
static monitor_state_t s_state;

void monitor_state_init(void)
{
	k_mutex_lock(&s_state_lock, K_FOREVER);
	memset(&s_state, 0, sizeof(s_state));
	s_state.state = BABY_STATE_NORMAL;
	s_state.env = ENV_OK;
	k_mutex_unlock(&s_state_lock);
}

monitor_state_t *monitor_state_get(void)
{
	return &s_state;
}

void monitor_state_lock(void)
{
	k_mutex_lock(&s_state_lock, K_FOREVER);
}

void monitor_state_unlock(void)
{
	k_mutex_unlock(&s_state_lock);
}
