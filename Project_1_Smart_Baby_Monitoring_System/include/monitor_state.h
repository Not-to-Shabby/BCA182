/**
 * @file monitor_state.h
 * @brief Shared system snapshot guarded by one mutex (LAB_4 pattern).
 *
 * Device-side only (Zephyr). The sensor task writes under the lock; the
 * UI/alert paths read a consistent copy. Never include this from native
 * tests -- it pulls in Zephyr kernel types via the implementation.
 */
#ifndef MONITOR_STATE_H_
#define MONITOR_STATE_H_

#include <stdint.h>
#include <stdbool.h>

#include "baby_logic.h"

typedef struct {
	/* Sensor snapshot (refreshed by the sensor task every 2 s) */
	int16_t temp_x10;   /* AHT10 temperature, C x10          */
	uint16_t hum_x10;   /* AHT10 relative humidity, %RH x10  */
	uint16_t lux;       /* AP3216C ambient light             */
	uint16_t proximity; /* AP3216C raw proximity             */
	bool aht_ok;        /* AHT10 responded at boot/probe     */
	bool aps_ok;        /* AP3216C responded                 */
	bool imu_ok;        /* ICM20608 responded                */
	int32_t accel_mg;   /* accel magnitude, mg               */
	bool device_moved;  /* |a|-1g > MOTION_FAULT_THRESHOLD_MG */

	/* Decision outputs (recomputed by the sensor task) */
	baby_state_t state;
	env_alert_t env;

	/* Timing / user feedback */
	uint32_t last_update_ms; /* k_uptime_get_32() of last refresh */
	uint32_t mute_until_ms;  /* mirror of logic mute window       */
	bool buzzer_on;          /* baby_logic_buzzer_on() result     */
	uint32_t cycles;         /* sensor refresh counter            */
	bool test_mode;          /* KEY1 demo-alert toggle (UI level) */
} monitor_state_t;

void monitor_state_init(void);
monitor_state_t *monitor_state_get(void); /* use between lock/unlock */
void monitor_state_lock(void);
void monitor_state_unlock(void);

#endif /* MONITOR_STATE_H_ */
