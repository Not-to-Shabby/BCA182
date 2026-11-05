/**
 * @file baby_logic.h
 * @brief Pure decision logic for the Smart Baby Monitoring System (RTOS-free).
 *
 * Host-testable under Unity ("pio test -e native"): no Zephyr, no HAL, no
 * hardware types -- stdint/stdbool only. Sensor values are fixed-point
 * integers (x10 for temp/humidity, mg for acceleration) so the whole module
 * is deterministic and float-free except where noted.
 */
#ifndef BABY_LOGIC_H_
#define BABY_LOGIC_H_

#include <stdint.h>
#include <stdbool.h>

/* ---- Documented alarm thresholds (P1 defaults, tune in one place) ---- */
#define TEMP_LOW_LIMIT_X10        160    /* 16.0 C  -- alert below            */
#define TEMP_HIGH_LIMIT_X10       320    /* 32.0 C  -- alert above            */
#define TEMP_HYST_X10             10     /* 1.0 C   -- re-arm hysteresis band */
#define HUM_HIGH_LIMIT_X10        800    /* 80.0 %RH -- alert above           */
#define HUM_HYST_X10              50     /* 5.0 %RH  -- re-arm hysteresis band */
#define MOTION_FAULT_THRESHOLD_MG 300    /* |a|-1g deviation (mg) => FAULT    */
#define ALERT_MUTE_MS             60000U /* ack silences buzzer for 60 s      */

typedef enum {
	BABY_STATE_NORMAL = 0, /* environment in range, device stable        */
	BABY_STATE_ALERT = 1,  /* environmental limit violated               */
	BABY_STATE_FAULT = 2   /* device fault (moved/tipped, sensor absent) */
} baby_state_t;

typedef enum {
	ENV_OK = 0,
	ENV_TEMP_LOW = 1,
	ENV_TEMP_HIGH = 2,
	ENV_HUMIDITY_HIGH = 3
} env_alert_t;

/* Reset all internal (static) logic state: mute window, caches. */
void baby_logic_reset(void);

/**
 * Hysteresis evaluation of the environmental alert.
 * Enters an alert when the raw limit is crossed; clears only after the
 * value returns past the hysteresis band. prev is the previous result.
 */
env_alert_t evaluate_environment(env_alert_t prev, int16_t temp_x10,
				 uint16_t hum_x10);

/**
 * System state machine: FAULT overrides ALERT overrides NORMAL.
 * device_moved comes from the IMU (device knocked/tipped), never from
 * anything claiming to sense the infant. env_sensor_ok is false while the
 * temperature/humidity source is absent or failing: a missing sensor is a
 * defined device FAULT, never evidence that the room is safe (spec:
 * troubleshooting & safety). cry_detected is the output of the on-device
 * cry classifier (confidence threshold + consecutive-window confirmation);
 * a confirmed cry raises ALERT like an environmental violation.
 */
baby_state_t evaluate_system_state(baby_state_t current, env_alert_t env,
				   bool device_moved, bool env_sensor_ok,
				   bool cry_detected);

/* Caregiver pressed the acknowledge key: silence the buzzer for
 * ALERT_MUTE_MS starting at now_ms. Does not change the state itself. */
void baby_logic_acknowledge(uint32_t now_ms);

/* Acknowledge with an EXPLICIT mute window (remote mute / portal can
 * request minutes-to-hours; KEY0 keeps ALERT_MUTE_MS). Works for any
 * duration -- buzzer_on() simply compares against the stored deadline. */
void baby_logic_acknowledge_for(uint32_t now_ms, uint32_t mute_ms);

/* True while the buzzer should sound: ALERT state and not inside the
 * mute window that started at the last acknowledge. */
bool baby_logic_buzzer_on(baby_state_t state, uint32_t now_ms);

/* Human-readable names for display/logging (never NULL). */
const char *env_alert_name(env_alert_t e);
const char *baby_state_name(baby_state_t s);

#endif /* BABY_LOGIC_H_ */
