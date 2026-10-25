/**
 * @file main.c
 * @brief Laboratory 4 - Laundry Sentinel: Edge AI appliance cycle monitor.
 *
 * Thread architecture (Zephyr RTOS, the analog of the FreeRTOS task table):
 *   sampler   (prio 6)  10 ms absolute deadline: ICM-20608 -> SPSC feature ring
 *   inference (prio 10) 1 s: features -> forest + baseline -> hysteresis -> events
 *   ui        (prio 12) 4 Hz: LCD dashboard + 19-LED state/alert rendering
 *   service   (prio 14) 50 ms: debounced buttons, DIAG/COLLECT/BENCH modes
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <stm32f4xx.h>

#include "icm20608.h"
#include "aht10.h"
#include "ap3216c.h"
#include "vibration_features.h"
#include "cycle_logic.h"
#include "cycle_model.h"
#include "ui_dashboard.h"
#include "service_mode.h"
#include "data_logger.h"
#include "led_matrix.h"

#include <string.h>
#include <math.h>
#include <stdio.h>

#define SAMPLER_PERIOD_MS    10U
#define INFERENCE_PERIOD_MS  1000U
#define UI_PERIOD_MS         250U
#define SERVICE_PERIOD_MS    50U

#define BENCH_RUNS           50U
#define BENCH_SPACING_MS     100U

/* ---------------------------------------------------------------- shared state */
static K_MUTEX_DEFINE(s_state_lock);
static cycle_decision_t s_decision;
static float s_rms_g;
static float s_dom_hz;
static float s_temp_c = NAN;
static float s_hum_pct = NAN;
static uint16_t s_lux = 0xFFFFU;
static bool s_imu_ok;

/* ------------------------------------------------------------------- threads */
static void sampler_thread_fn(void *, void *, void *);
static void inference_thread_fn(void *, void *, void *);
static void ui_thread_fn(void *, void *, void *);
static void service_thread_fn(void *, void *, void *);

K_THREAD_STACK_DEFINE(s_sampler_stack, 2048);
static struct k_thread s_sampler_thread;
K_THREAD_STACK_DEFINE(s_inference_stack, 6144); /* 3 x 800 B feature scratch + margin */
static struct k_thread s_inference_thread;
K_THREAD_STACK_DEFINE(s_ui_stack, 4096);
static struct k_thread s_ui_thread;
K_THREAD_STACK_DEFINE(s_service_stack, 4096);
static struct k_thread s_service_thread;

static void sleep_until(uint32_t *next_wake_ms, uint32_t period_ms)
{
    *next_wake_ms += period_ms;
    uint32_t now = k_uptime_get_32();
    int32_t remaining = (int32_t)(*next_wake_ms - now);
    if (remaining < 1) {
        /* Deadline missed (e.g. heavy capture streaming): resync instead of bursting */
        *next_wake_ms = now + period_ms;
        remaining = period_ms;
    }
    k_sleep(K_MSEC(remaining));
}

/* ------------------------------------------------------------------- sampler */
static void sampler_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    imu_sample_t sample;
    uint32_t next_wake = k_uptime_get_32();
    uint32_t consecutive_failures = 0;

    for (;;) {
        bool ok = icm20608_read_sample(&sample);
        if (ok) {
            consecutive_failures = 0;
            vib_features_push(&sample);

            if (service_mode_is_collecting()) {
                /* Raw LSB counts: cheap integer printk at 100 Hz. The trainer
                 * converts (4096 LSB/g accel, 32.8 LSB/(deg/s) gyro). */
                printk("%d,%d,%d,%d,%d,%d,%d\n",
                       (int)(sample.ax * ICM20608_ACCEL_SENS_8G),
                       (int)(sample.ay * ICM20608_ACCEL_SENS_8G),
                       (int)(sample.az * ICM20608_ACCEL_SENS_8G),
                       (int)(sample.gx * ICM20608_GYRO_SENS_1000DPS * (180.0f / 3.14159265f)),
                       (int)(sample.gy * ICM20608_GYRO_SENS_1000DPS * (180.0f / 3.14159265f)),
                       (int)(sample.gz * ICM20608_GYRO_SENS_1000DPS * (180.0f / 3.14159265f)),
                       service_mode_collect_label());
                service_mode_collect_note_sample();
            }
        } else {
            if (++consecutive_failures == 100U) {
                printk("[IMU] Read failing (100 consecutive misses)\n");
            }
        }

        k_mutex_lock(&s_state_lock, K_FOREVER);
        s_imu_ok = consecutive_failures < 10U;
        k_mutex_unlock(&s_state_lock);

        sleep_until(&next_wake, SAMPLER_PERIOD_MS);
    }
}

/* ----------------------------------------------------------------- inference */
static void inference_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    static float features[VIB_NUM_FEATURES];
    uint32_t next_wake = k_uptime_get_32();
    uint32_t window_index = 0;

    for (;;) {
        if (vib_features_extract(features)) {
            aht10_reading_t env;
            if ((window_index % 10U) == 0U && aht10_read(&env)) {
                k_mutex_lock(&s_state_lock, K_FOREVER);
                s_temp_c = env.temperature_c;
                s_hum_pct = env.humidity_pct;
                k_mutex_unlock(&s_state_lock);
            }

            ap3216c_reading_t light;
            if (ap3216c_read(&light)) {
                k_mutex_lock(&s_state_lock, K_FOREVER);
                s_lux = light.lux;
                k_mutex_unlock(&s_state_lock);
            }

            cycle_decision_t decision;
            cycle_logic_process(features, k_uptime_get_32(), &decision);

            k_mutex_lock(&s_state_lock, K_FOREVER);
            s_decision = decision;
            s_rms_g = features[FEAT_ACC_RMS];
            s_dom_hz = features[FEAT_DOM_FREQ];
            k_mutex_unlock(&s_state_lock);
        }
        window_index++;
        sleep_until(&next_wake, INFERENCE_PERIOD_MS);
    }
}

/* ----------------------------------------------------------------------- ui */
static void ui_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    ui_snapshot_t snap;
    uint32_t next_wake = k_uptime_get_32();

    for (;;) {
        k_mutex_lock(&s_state_lock, K_FOREVER);
        snap.decision = s_decision;
        snap.rms_g = s_rms_g;
        snap.dom_hz = s_dom_hz;
        snap.temperature_c = s_temp_c;
        snap.humidity_pct = s_hum_pct;
        snap.lux = s_lux;
        k_mutex_unlock(&s_state_lock);

        snap.mode = (ui_mode_t)service_mode_get();
        snap.alert = cycle_logic_is_alerting() && ((k_uptime_get_32() / 250U) & 1U);
        snap.diag_page = service_mode_diag_page();
        snap.collect_samples = service_mode_collect_samples();
        snap.collect_label = service_mode_collect_label();
        snap.bench_done = service_mode_bench_done();
        snap.bench_median_us = service_mode_bench_median_us();
        snap.bench_p95_us = service_mode_bench_p95_us();

        ui_dashboard_render(&snap);

        /* Ambient flags come from the cached context (the inference thread owns
         * the AHT10 trigger cadence; the UI thread must not touch the I2C bus). */
        bool hot = !isnan(snap.temperature_c) && snap.temperature_c > 30.0f;
        bool dark = snap.lux < 30U; /* 0xFFFF (absent) fails this test by design */
        led_matrix_update_appliance((int)snap.decision.state, snap.alert,
                                    snap.rms_g, hot, dark);

        sleep_until(&next_wake, UI_PERIOD_MS);
    }
}

/* ------------------------------------------------------------------- service */
static void run_inference_bench(void)
{
    static float features[VIB_NUM_FEATURES];
    uint32_t times_us[BENCH_RUNS];
    uint32_t done = 0;

    printk("[BENCH] Timing %u live inferences...\n", (unsigned)BENCH_RUNS);
    while (done < BENCH_RUNS) {
        if (!vib_features_extract(features)) {
            k_sleep(K_MSEC(SERVICE_PERIOD_MS));
            continue;
        }

        DWT->CYCCNT = 0;
        (void)cycle_model_predict(features);
        uint32_t cycles = DWT->CYCCNT;
        times_us[done++] = (uint32_t)((uint64_t)cycles * 1000000ULL / SystemCoreClock);

        k_sleep(K_MSEC(BENCH_SPACING_MS));
    }

    /* Insertion sort for the median and 95th percentile */
    for (uint32_t i = 1; i < BENCH_RUNS; i++) {
        uint32_t v = times_us[i];
        uint32_t j = i;
        while (j > 0 && times_us[j - 1] > v) {
            times_us[j] = times_us[j - 1];
            j--;
        }
        times_us[j] = v;
    }
    uint32_t median = times_us[BENCH_RUNS / 2U];
    uint32_t p95 = times_us[(BENCH_RUNS * 95U) / 100U];
    service_mode_store_bench(done, median, p95);
    printk("[BENCH] %u runs: median %u us, p95 %u us\n",
           (unsigned)done, (unsigned)median, (unsigned)p95);
}

static void service_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    uint32_t next_wake = k_uptime_get_32();
    bool bench_started = false;

    for (;;) {
        service_mode_poll();

        if (service_mode_get() == SVC_BENCH && !bench_started &&
            service_mode_bench_done() == 0U) {
            bench_started = true;
            run_inference_bench();
        }
        if (service_mode_get() != SVC_BENCH) {
            bench_started = false;
        }

        sleep_until(&next_wake, SERVICE_PERIOD_MS);
    }
}

/* ---------------------------------------------------------------------- main */
int main(void)
{
    printk("\n=== Laboratory 4: Laundry Sentinel (Edge AI Appliance Cycle Monitor) ===\n");
    printk("Zephyr RTOS | %s | model: %u trees x %u features\n", CONFIG_BOARD,
           (unsigned)CYCLE_MODEL_NUM_TREES, (unsigned)CYCLE_MODEL_NUM_FEATURES);

    service_mode_init();
    led_matrix_init();
    ui_dashboard_init();

    s_imu_ok = icm20608_init();
    if (!s_imu_ok) {
        printk("[APP] WARNING: primary vibration sensor absent - product disabled\n");
    }
    aht10_init();
    ap3216c_init();

    vib_features_reset();
    cycle_logic_reset();
    data_logger_init();

    k_thread_create(&s_sampler_thread, s_sampler_stack, K_THREAD_STACK_SIZEOF(s_sampler_stack),
                    sampler_thread_fn, NULL, NULL, NULL, 6, 0, K_NO_WAIT);
    k_thread_name_set(&s_sampler_thread, "sampler");
    k_thread_create(&s_inference_thread, s_inference_stack, K_THREAD_STACK_SIZEOF(s_inference_stack),
                    inference_thread_fn, NULL, NULL, NULL, 10, 0, K_NO_WAIT);
    k_thread_name_set(&s_inference_thread, "inference");
    k_thread_create(&s_ui_thread, s_ui_stack, K_THREAD_STACK_SIZEOF(s_ui_stack),
                    ui_thread_fn, NULL, NULL, NULL, 12, 0, K_NO_WAIT);
    k_thread_name_set(&s_ui_thread, "ui");
    k_thread_create(&s_service_thread, s_service_stack, K_THREAD_STACK_SIZEOF(s_service_stack),
                    service_thread_fn, NULL, NULL, NULL, 14, 0, K_NO_WAIT);
    k_thread_name_set(&s_service_thread, "service");

    printk("[APP] Threads running: sampler(6) inference(10) ui(12) service(14)\n");
    return 0;
}
