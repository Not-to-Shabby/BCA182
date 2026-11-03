/**
 * @file sensor_task.c
 * @brief Sensor-acquisition thread: ICM20608 every 200 ms; AHT10 + AP3216C
 *        every 2 s (assignment: environmental 1-5 s, motion faster).
 *
 * Publishes each snapshot to the telemetry queue (sensor -> net) and updates
 * the shared state store (sensor -> UI). Deadlines are absolute so the loop
 * keeps the intended average rate even under transient overruns (the
 * vTaskDelayUntil discipline from the assignment, in Zephyr form).
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <math.h>

#include "aht10.h"
#include "ap3216c.h"
#include "app_config.h"
#include "app_context.h"
#include "icm20608.h"
#include "telemetry.h"

static telemetry_snapshot_t s_sample;

void sensor_task_run(void)
{
    uint32_t env_divider = APP_ENV_PERIOD_MS / APP_SENSOR_PERIOD_MS;
    uint32_t tick = 0;

    printk("[SENSOR] acquisition thread started (imu %u ms, env %u ms)\n",
           (unsigned)APP_SENSOR_PERIOD_MS, (unsigned)APP_ENV_PERIOD_MS);

    int64_t deadline = k_uptime_get();

    for (;;) {
        deadline += APP_SENSOR_PERIOD_MS;
        tick++;

        memset(&s_sample, 0, sizeof(s_sample));
        s_sample.seq = tick;
        s_sample.ts_ms = k_uptime_get();

        /* --- Motion (every cycle) -------------------------------------- */
        imu_sample_t imu;
        if (icm20608_read_sample(&imu)) {
            s_sample.ax = imu.ax;
            s_sample.ay = imu.ay;
            s_sample.az = imu.az;
            s_sample.gx = imu.gx;
            s_sample.gy = imu.gy;
            s_sample.gz = imu.gz;
            s_sample.motion_mag =
                sqrtf(imu.ax * imu.ax + imu.ay * imu.ay + imu.az * imu.az);
            s_sample.motion_event =
                fabsf(s_sample.motion_mag - 1.0f) > APP_MOTION_THRESHOLD_G;
        }

        /* --- Environment + light/proximity (every env_divider cycles) --- */
        if ((tick % env_divider) == 0u) {
            aht10_reading_t env;
            if (aht10_read(&env)) {
                s_sample.temperature_c = env.temperature_c;
                s_sample.humidity_pct = env.humidity_pct;
            }
            ap3216c_reading_t als;
            if (ap3216c_read(&als)) {
                s_sample.light = als.lux;
                s_sample.proximity = als.proximity;
            }
        }

        /* NOTE: env fields are only refreshed every 2 s; on the other
         * cycles they carry the last valid values (documented: the
         * dashboard sees a 2 s environmental cadence, 200 ms motion). */

        app_state_update(&s_sample);
        if (!app_telemetry_put(&s_sample)) {
            app_count_drop(); /* net task busy: drop, never block sensing */
        }

        /* --- Absolute-deadline sleep ----------------------------------- */
        int64_t remain = deadline - k_uptime_get();
        if (remain > 0) {
            k_msleep(remain);
        } else {
            deadline = k_uptime_get(); /* overrun: resync, do not spiral */
        }
    }
}

K_THREAD_DEFINE(sensor_thread, 2048, sensor_task_run, NULL, NULL, NULL, 5, 0, 0);
