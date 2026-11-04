/**
 * @file sensor_task.c
 * @brief SENSOR layer: periodic AHT10 / AP3216C / ICM20608 acquisition feeding
 *        the shared monitor_state snapshot and the baby decision logic.
 *
 * Thread architecture (Zephyr RTOS, statically defined, house style):
 *   sensor (prio 6) 2000 ms absolute deadline:
 *     boot: soft_i2c_init() -> aht10_init() -> ap3216c_init() -> icm20608_init()
 *           (drivers latch presence internally and print a graceful ABSENT
 *           line when a sensor is not fitted)
 *     loop: read T/RH (x10), lux + proximity, accel magnitude (mg);
 *           under the state lock: refresh snapshot, then
 *           evaluate_environment() -> evaluate_system_state() ->
 *           baby_logic_buzzer_on(). One [SENSE] line per state/env change.
 *
 * Absent sensors degrade gracefully (fixed defaults, *_ok false); a failed
 * read on a PRESENT sensor keeps the last values for that cycle, clears that
 * sensor's *_ok flag, and warns at most once per 10 consecutive failures.
 */

#include "sensor_task.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <math.h>

#include "soft_i2c.h"
#include "aht10.h"
#include "ap3216c.h"
#include "icm20608.h"
#include "baby_logic.h"
#include "monitor_state.h"

#define SENSOR_PERIOD_MS     2000U
#define SENSOR_FAULT_AFTER_FAILURES 5U /* consecutive env-sensor read failures */
#define READ_WARN_EVERY      10U   /* throttled read-failure warning cadence   */
#define BOOT_SETTLE_MS       500U  /* let main() finish GPIOF bring-up first   */
#define NOMINAL_ACCEL_MG     1000  /* |a| of a resting device: 1 g             */

/* ------------------------------------------------------------------- threads */
static void sensor_thread_fn(void *p1, void *p2, void *p3);

/* House style: static thread, no dynamic allocation. CONFIG_THREAD_NAME=y
 * publishes the symbol name ("sensor_thread") as the runtime thread name. */
K_THREAD_DEFINE(sensor_thread, 2048, sensor_thread_fn, NULL, NULL, NULL, 6, 0, 0);

/* Deadline-resync sleep (Laboratory 4 pattern): keeps a fixed cadence instead
 * of bursting after a missed deadline. */
static void sleep_until(uint32_t *next_wake_ms, uint32_t period_ms)
{
    *next_wake_ms += period_ms;
    uint32_t now = k_uptime_get_32();
    int32_t remaining = (int32_t)(*next_wake_ms - now);
    if (remaining < 1) {
        /* Deadline missed: resync instead of bursting */
        *next_wake_ms = now + period_ms;
        remaining = period_ms;
    }
    k_sleep(K_MSEC(remaining));
}

/* ----------------------------------------------------------------- boot probe */
/* Probe order per design: AHT10 -> AP3216C -> ICM20608. The bus must exist
 * before the first probe; icm20608_init() re-runs the same idempotent GPIO
 * setup internally, so calling soft_i2c_init() here is harmless duplication. */
static void probe_sensors(bool *aht_present, bool *aps_present, bool *imu_present)
{
    soft_i2c_init();

    *aht_present = aht10_init();
    *aps_present = ap3216c_init();
    *imu_present = icm20608_init();

    printk("[SENSE] Probe: AHT10 %s, AP3216C %s, ICM20608 %s\n",
           *aht_present ? "ok" : "absent",
           *aps_present ? "ok" : "absent",
           *imu_present ? "ok" : "absent");
}

/* ------------------------------------------------------------------ sampling */
/* Each sampler returns the sensor's *_ok value for this cycle: present and
 * read OK. On failure (or absence) the last values are retained by the caller
 * and only the *_ok flag drops for that cycle. */

static bool sample_aht10(bool present, int16_t *temp_x10, uint16_t *hum_x10,
                         uint32_t *failures)
{
    aht10_reading_t aht;
    if (aht10_read(&aht)) {
        *temp_x10 = (int16_t)lroundf(aht.temperature_c * 10.0f);
        *hum_x10 = (uint16_t)lroundf(aht.humidity_pct * 10.0f);
        *failures = 0U;
        return present;
    }
    if (present && (++(*failures) == 1U || (*failures % READ_WARN_EVERY) == 0U)) {
        printk("[SENSE] WARNING: AHT10 read failed (%u consecutive)\n",
               (unsigned)*failures);
    }
    return false;
}

static bool sample_ap3216c(bool present, uint16_t *lux, uint16_t *proximity,
                           uint32_t *failures)
{
    ap3216c_reading_t aps;
    if (ap3216c_read(&aps)) {
        *lux = aps.lux;
        *proximity = aps.proximity;
        *failures = 0U;
        return present;
    }
    if (present && (++(*failures) == 1U || (*failures % READ_WARN_EVERY) == 0U)) {
        printk("[SENSE] WARNING: AP3216C read failed (%u consecutive)\n",
               (unsigned)*failures);
    }
    return false;
}

static bool sample_imu(bool present, int32_t *accel_mg, bool *device_moved,
                       uint32_t *failures)
{
    imu_sample_t imu;
    if (icm20608_read_sample(&imu)) {
        /* Magnitude in mg: axes are in g. A resting device reads ~1000 mg;
         * the deviation from 1 g drives the moved/tipped FAULT input. */
        float g = sqrtf(imu.ax * imu.ax + imu.ay * imu.ay + imu.az * imu.az);
        *accel_mg = (int32_t)(g * 1000.0f);
        int32_t deviation = *accel_mg - NOMINAL_ACCEL_MG;
        if (deviation < 0) {
            deviation = -deviation;
        }
        *device_moved = deviation > MOTION_FAULT_THRESHOLD_MG;
        *failures = 0U;
        return present;
    }
    if (present && (++(*failures) == 1U || (*failures % READ_WARN_EVERY) == 0U)) {
        printk("[SENSE] WARNING: ICM20608 read failed (%u consecutive)\n",
               (unsigned)*failures);
    }
    return false; /* last accel_mg / device_moved retained for this cycle */
}

/* ---------------------------------------------------------------- thread body */
static void sensor_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    /* Statically defined threads start before main(); pause until board
     * bring-up in main() (LCD/LED matrix also RMW GPIOF registers, the port
     * holding the soft-I2C pins PF0/PF1) is done. */
    k_sleep(K_MSEC(BOOT_SETTLE_MS));

    monitor_state_init();

    bool aht_present = false;
    bool aps_present = false;
    bool imu_present = false;
    probe_sensors(&aht_present, &aps_present, &imu_present);

    /* Last-good readings; defaults cover absent sensors (device at rest:
     * 1000 mg, not moved). */
    int16_t temp_x10 = 0;
    uint16_t hum_x10 = 0U;
    uint16_t lux = 0U;
    uint16_t proximity = 0U;
    int32_t accel_mg = NOMINAL_ACCEL_MG;
    bool device_moved = false;

    uint32_t aht_failures = 0U;
    uint32_t aps_failures = 0U;
    uint32_t imu_failures = 0U;
    baby_state_t prev_state = BABY_STATE_NORMAL; /* matches monitor_state_init */
    bool s_first_reading_logged = false;
    env_alert_t prev_env = ENV_OK;
    uint32_t next_wake = k_uptime_get_32();

    for (;;) {
        bool aht_ok = sample_aht10(aht_present, &temp_x10, &hum_x10, &aht_failures);
        bool aps_ok = sample_ap3216c(aps_present, &lux, &proximity, &aps_failures);
        bool imu_ok = sample_imu(imu_present, &accel_mg, &device_moved, &imu_failures);

        monitor_state_lock();
        monitor_state_t *st = monitor_state_get();
        st->temp_x10 = temp_x10;
        st->hum_x10 = hum_x10;
        st->lux = lux;
        st->proximity = proximity;
        st->aht_ok = aht_ok;
        st->aps_ok = aps_ok;
        st->imu_ok = imu_ok;
        st->accel_mg = accel_mg;
        st->device_moved = device_moved;
        st->cycles++;
        st->last_update_ms = k_uptime_get_32();

        /* Decision layer (pure logic, runs under the lock for a consistent
         * snapshot). mute_until_ms stays owned by the UI acknowledge path.
         * The environment sensor counts as failed once absent at boot or
         * after SENSOR_FAULT_AFTER_FAILURES consecutive read failures
         * (=> device FAULT, not a false environmental alert). */
        bool env_sensor_ok =
            aht_present && (aht_failures < SENSOR_FAULT_AFTER_FAILURES);
        st->env_sensor_ok = env_sensor_ok;
        /* No environmental evaluation without a live sensor: stale "0.0 C"
         * readings must not surface as a TEMP LOW alert next to the FAULT. */
        env_alert_t env = env_sensor_ok
                              ? evaluate_environment(st->env, st->temp_x10,
                                                     st->hum_x10)
                              : ENV_OK;
        baby_state_t state = evaluate_system_state(st->state, env,
                                                   st->device_moved,
                                                   env_sensor_ok,
                                                   st->cry_detected);
        st->env = env;
        st->state = state;
        st->buzzer_on = baby_logic_buzzer_on(state, st->last_update_ms);
        monitor_state_unlock();

        /* One console line per decision change (compare before overwriting). */
        if (state != prev_state || env != prev_env) {
            printk("[SENSE] state %s -> %s (env %s)\n",
                   baby_state_name(prev_state), baby_state_name(state),
                   env_alert_name(env));
            prev_state = state;
            prev_env = env;
        }

        /* One-shot evidence line: the first live reading after probe. */
        if (!s_first_reading_logged && aht_ok) {
            s_first_reading_logged = true;
            printk("[SENSE] first reading: T=%d.%u C RH=%u.%u %% LUX=%u "
                   "PROX=%u ACCEL=%ld mg\n",
                   temp_x10 / 10, (unsigned)(temp_x10 % 10),
                   hum_x10 / 10, (unsigned)(hum_x10 % 10),
                   (unsigned)lux, (unsigned)proximity,
                   (long)accel_mg);
        }

        sleep_until(&next_wake, SENSOR_PERIOD_MS);
    }
}

/* ----------------------------------------------------------------- start hook */
void sensor_task_start(void)
{
	/* No-op by design: the sensor thread is created at build time via
	 * K_THREAD_DEFINE() above and starts automatically at boot. The
	 * explicit call site in main() documents the bring-up order. */
}
