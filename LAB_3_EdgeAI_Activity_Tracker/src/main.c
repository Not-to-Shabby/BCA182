/**
 * ==============================================================================
 * Laboratory Activity 3: Edge AI Activity Tracker & BLE Telemetry
 * ==============================================================================
 * Microcontroller: STMicroelectronics STM32F407ZGT6 (RT-Thread Spark Board)
 * Sensors: InvenSense ICM-20608-G (I2C2 on PB10/PB11 @ 10 Hz)
 * Display: 1.3" 240x240 ST7789 v3 IPS LCD via FSMC Bank 3 8080 Parallel Bus
 * Wireless: Realtek RW007 BLE Peripheral (Method A GATT Telemetry Service)
 * Model: 10-Tree Random Forest executing on ARM Cortex-M4F Hardware FPU
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#if defined(ZEPHYR_VERSION_CODE) || defined(__arm__)
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#define SLEEP_MS(ms) k_msleep(ms)
#define GET_TIME_MS() k_uptime_get_32()
#else
#define SLEEP_MS(ms) (void)ms
static uint32_t s_sim_time = 0;
#define GET_TIME_MS() (s_sim_time += 100)
#endif

#include "activity_model.h"
#include "feature_extractor.h"
#include "icm20608.h"
#include "rw007_ble.h"
#include "lcd_st7789.h"
#include "ui_dashboard.h"
#include "uart_telemetry.h"

int main(void)
{
    /* 0. Initialize Direct Hardware USART1 on PA9/PA10 for ST-LINK VCP @ 115200 baud */
    uart1_telemetry_init();

    uart1_printf("\n===============================================================\n");
    uart1_printf("  BCA180/BCA152 - Laboratory Activity 3: Activity Recognition  \n");
    uart1_printf("  Target: RT-Thread Spark Board (STM32F407ZGT6)                \n");
    uart1_printf("  Display: 240x240 ST7789 IPS LCD (FSMC Parallel Bus)          \n");
    uart1_printf("  Telemetry: Direct Hardware USART1 (PA9/PA10 @ 115200 baud)   \n");
    uart1_printf("  Architecture: On-Device TinyML (Cortex-M4F FPU) + RW007 BLE   \n");
    uart1_printf("===============================================================\n\n");

    /* 1. Initialize LCD & Render Dashboard Frame */
    uart1_printf("[INIT] Initializing 240x240 ST7789 IPS LCD on FSMC Bank 3...\n");
    ui_dashboard_init();
    uart1_printf("[INIT] LCD frame and backlight (PF9) active.\n");

    /* 2. Initialize 6-Axis Motion Sensor */
    uart1_printf("[INIT] Initializing InvenSense ICM-20608 6-Axis IMU (I2C2)...\n");
    bool imu_ok = icm20608_init();
    if (!imu_ok) {
        uart1_printf("[WARN] ICM-20608 not responding (WHO_AM_I mismatch). Using simulation fallback.\n");
    } else {
        uart1_printf("[INIT] ICM-20608 verified (WHO_AM_I = 0xAF). Configured @ 10 Hz, +-4g, +-1000 dps.\n");
    }

    /* 3. Initialize Feature Extractor Window & Pedometer */
    feature_window_t window;
    feature_extractor_init(&window);
    uart1_printf("[INIT] Feature window initialized (15 samples, 21 metrics, FPU-accelerated).\n");

    /* 4. Initialize RW007 BLE GATT Notification Service */
    rw007_ble_telemetry_init();

    uart1_printf("\n[SYSTEM] Entering continuous real-time acquisition and classification loop...\n\n");

    activity_type_t current_activity = ACTIVITY_WALKING;
    activity_type_t last_activity = ACTIVITY_UNKNOWN;
    uint8_t confidence = 85;
    uint32_t loop_count = 0;

    while (1) {
        uint32_t now_ms = GET_TIME_MS();
        imu_sample_t sample;

        /* Acquire reading from physical sensor or fallback */
        bool got_sample = false;
        if (imu_ok) {
            got_sample = icm20608_read_sample(&sample);
        }

        if (!got_sample) {
            /* If physical sensor read fails, default to realistic STATIONARY resting baseline
             * (1.0g gravity along Z-axis, microscopic MEMS thermal noise < 0.01g) */
            float noise = ((float)(loop_count % 7) - 3.0f) * 0.001f;
            sample.ax = 0.02f + noise;
            sample.ay = -0.01f - noise;
            sample.az = 0.99f + noise;
            sample.gx = 0.001f;
            sample.gy = -0.001f;
            sample.gz = 0.001f;
        }

        /* Push into temporal sliding window */
        bool window_ready = feature_extractor_push(&window, &sample, now_ms);

        /* Compute features and run TinyML inference every 7 samples (~0.7s step) */
        if (window_ready && (loop_count % 7 == 0)) {
            float features[ACTIVITY_NUM_FEATURES];
            feature_extractor_compute(&window, features);

            /* Run 10-Tree Random Forest Model on Cortex-M4F Hardware FPU (< 5 microseconds) */
            current_activity = tinyml_predict_activity(features, &confidence);

            float mag_mean = features[18];

            /* 1. Transmit BLE Telemetry Notification (Method A: RW007 Char 0xFEA2) */
            rw007_ble_send_activity(
                current_activity,
                confidence,
                window.current_cadence_spm,
                window.total_steps,
                mag_mean
            );

            /* 2. Differential Live Update on ST7789 IPS Display */
            ui_dashboard_update(
                current_activity,
                confidence,
                window.current_cadence_spm,
                window.total_steps,
                mag_mean,
                sample.ax,
                sample.ay,
                sample.az
            );

            /* 3. Terminal Notification on State Transition */
            if (current_activity != last_activity) {
                const char *act_name = "STATIONARY";
                if (current_activity == ACTIVITY_RUNNING) act_name = "RUNNING";
                else if (current_activity == ACTIVITY_WALKING) act_name = "WALKING";

                uart1_printf("[TRANSITION] Movement changed -> %s (Confidence: %u%%)\n",
                             act_name, confidence);
                last_activity = current_activity;
            }
        } else if (loop_count % 3 == 0) {
            /* High-rate live accelerometer readout update on LCD (every 300 ms) */
            float inst_mag = sqrtf(sample.ax * sample.ax + sample.ay * sample.ay + sample.az * sample.az);
            ui_dashboard_update(
                current_activity,
                confidence,
                window.current_cadence_spm,
                window.total_steps,
                inst_mag,
                sample.ax,
                sample.ay,
                sample.az
            );
        }

        loop_count++;
        SLEEP_MS(100); /* 100 ms loop delay = 10 Hz sampling rate */
    }

    return 0;
}
