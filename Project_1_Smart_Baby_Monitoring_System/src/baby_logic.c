/**
 * @file baby_logic.c
 * @brief Pure decision logic for the Smart Baby Monitoring System.
 *
 * Implements the contract in baby_logic.h: hysteresis environmental alert
 * evaluation, the FAULT > ALERT > NORMAL state machine, and the buzzer mute
 * window. No Zephyr or HAL includes and no logging of any kind: this file
 * compiles unchanged for both the board and the host Unity suite
 * ("pio test -e native").
 *
 * Thread-safety: every caller invokes these functions while holding the
 * monitor mutex (monitor_state_lock()/monitor_state_unlock()), so the single
 * static below needs no internal locking.
 */

#include "baby_logic.h"

/* The only static state in this module: the buzzer stays silent until this
 * millisecond timestamp. 0 doubles as the "no acknowledge pending" sentinel
 * (an acknowledge always stores a deadline ALERT_MUTE_MS in the future). */
static uint32_t s_mute_until_ms;

void baby_logic_reset(void)
{
    s_mute_until_ms = 0U;
}

env_alert_t evaluate_environment(env_alert_t prev, int16_t temp_x10,
                                 uint16_t hum_x10)
{
    /* Boundary convention (documented choices):
     * - Enter an alert only when the raw limit is strictly crossed:
     *     temp < 160 (16.0 C)   -> ENV_TEMP_LOW       (159 enters, 160 not)
     *     temp > 320 (32.0 C)   -> ENV_TEMP_HIGH      (321 enters, 320 not)
     *     hum  > 800 (80.0 %RH) -> ENV_HUMIDITY_HIGH  (801 enters, 800 not)
     * - Clear only after the value returns past the hysteresis band:
     *     temp >= 170 (16.0 C + 1.0 C)  -> clears ENV_TEMP_LOW      (170 clears)
     *     temp <  310 (32.0 C - 1.0 C)  -> clears ENV_TEMP_HIGH     (309 clears)
     *     hum  <  750 (80.0 %RH - 5.0)  -> clears ENV_HUMIDITY_HIGH (749 clears)
     * A reading inside a band holds the previous result. */

    /* Temperature is evaluated first and outranks humidity: an out-of-range
     * temperature is the more acute risk to an infant than high humidity, so
     * when one reading violates both limits at once the temperature alert is
     * reported and the humidity alert resumes after the temperature recovers.
     * The two temperature alerts can also never co-occur: a single reading
     * cannot be both < 16.0 C and > 32.0 C. If a stale prev says TEMP_LOW
     * while the current reading is already past the high limit (a fast
     * swing), the current extreme reading wins over the stale state. */
    if (temp_x10 < TEMP_LOW_LIMIT_X10) {
        return ENV_TEMP_LOW;
    }
    if (prev == ENV_TEMP_LOW &&
        temp_x10 < TEMP_LOW_LIMIT_X10 + TEMP_HYST_X10) {
        return ENV_TEMP_LOW; /* inside the re-arm band: hold */
    }

    if (temp_x10 > TEMP_HIGH_LIMIT_X10) {
        return ENV_TEMP_HIGH;
    }
    if (prev == ENV_TEMP_HIGH &&
        temp_x10 >= TEMP_HIGH_LIMIT_X10 - TEMP_HYST_X10) {
        return ENV_TEMP_HIGH; /* inside the re-arm band: hold */
    }

    if (hum_x10 > HUM_HIGH_LIMIT_X10) {
        return ENV_HUMIDITY_HIGH;
    }
    if (prev == ENV_HUMIDITY_HIGH &&
        hum_x10 >= HUM_HIGH_LIMIT_X10 - HUM_HYST_X10) {
        return ENV_HUMIDITY_HIGH; /* inside the re-arm band: hold */
    }

    return ENV_OK;
}

baby_state_t evaluate_system_state(baby_state_t current, env_alert_t env,
                                   bool device_moved, bool env_sensor_ok,
                                   bool cry_detected)
{
    /* Pure and stateless by contract: FAULT overrides ALERT overrides
     * NORMAL, with no latching here. current is intentionally unused but
     * kept in the signature so callers can pass their tracked state and a
     * future latch needs no call-site change. A missing/failing environment
     * sensor is a device FAULT (spec: troubleshooting & safety) -- the
     * system must not present stale "0.0 C" readings as a TEMP LOW alert.
     * A confirmed cry (classifier, 2-of-3 windows) raises ALERT. */
    (void)current;

    if (device_moved || !env_sensor_ok) {
        return BABY_STATE_FAULT;
    }
    if (env != ENV_OK || cry_detected) {
        return BABY_STATE_ALERT;
    }
    return BABY_STATE_NORMAL;
}

void baby_logic_acknowledge(uint32_t now_ms)
{
    s_mute_until_ms = now_ms + ALERT_MUTE_MS;
}

bool baby_logic_buzzer_on(baby_state_t state, uint32_t now_ms)
{
    if (state != BABY_STATE_ALERT) {
        return false;
    }
    if (s_mute_until_ms == 0U) {
        return true; /* sentinel: no acknowledge pending */
    }
    /* Signed difference survives the uint32_t tick wraparound: the buzzer
     * stays off while now_ms is before the mute deadline, and sounds again
     * exactly when the deadline is reached. */
    return ((int32_t)(now_ms - s_mute_until_ms) >= 0);
}

const char *env_alert_name(env_alert_t e)
{
    switch (e) {
    case ENV_OK: return "OK";
    case ENV_TEMP_LOW: return "TEMP LOW";
    case ENV_TEMP_HIGH: return "TEMP HIGH";
    case ENV_HUMIDITY_HIGH: return "HUMIDITY HIGH";
    default: return "?";
    }
}

const char *baby_state_name(baby_state_t s)
{
    switch (s) {
    case BABY_STATE_NORMAL: return "NORMAL";
    case BABY_STATE_ALERT: return "ALERT";
    case BABY_STATE_FAULT: return "FAULT";
    default: return "?";
    }
}
