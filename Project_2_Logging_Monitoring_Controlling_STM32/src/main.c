/**
 * @file main.c
 * @brief BenchWatch - BCA182 Project 2: Logging, Monitoring, and Controlling
 *        on the RT-Spark (STM32F407ZGT6) under Zephyr RTOS.
 *
 * Application: laboratory workbench sentinel. Environmental conditions
 * (AHT10), light/proximity (AP3216C), and motion (ICM20608) are acquired by
 * sensor_task, published over MQTT by net_task, stored in InfluxDB by the
 * PC-side ingestion service, visualized in Grafana, and acted upon locally by
 * control_task (buzzer/LED matrix) with documented local-priority rules.
 *
 * main() only brings up hardware and logging the inventory; the four
 * application threads are created statically (K_THREAD_DEFINE) and start
 * immediately. See docs/plan.md for the phase plan and the Zephyr deviation
 * note.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "aht10.h"
#include "ap3216c.h"
#include "app_config.h"
#include "app_context.h"
#include "icm20608.h"
#include "lcd_st7789.h"
#include "led_matrix.h"
#include "sd_card.h"
#include "soft_i2c.h"

int main(void)
{
    printk("\n============================================\n");
    printk("  BenchWatch %s (%s)\n", "1.0-scaffold", APP_DEVICE_ID);
    printk("  BCA182 Project 2 - MQTT/InfluxDB/Grafana\n");
    printk("============================================\n");

    app_context_init();

    /* --- Hardware inventory bring-up ------------------------------------ */
    soft_i2c_init();

    aht10_init();
    printk("[INIT] AHT10 %s\n", aht10_is_present() ? "present" : "MISSING");

    ap3216c_init();
    printk("[INIT] AP3216C %s\n", ap3216c_is_present() ? "present" : "MISSING");

    printk("[INIT] ICM20608 %s\n", icm20608_init() ? "present" : "MISSING");

    led_matrix_init();

    /* microSD is optional buffering; failure must not stop the system. */
    sd_card_init();
    printk("[INIT] microSD %s (recover count %u)\n",
           sd_card_is_mounted() ? "mounted" : "not mounted",
           (unsigned)sd_card_get_recover_count());

    printk("[INIT] threads: sensor(200ms) net(broker %s:%u) control(50ms) ui\n",
           APP_BROKER_HOST, (unsigned)APP_BROKER_PORT);
    printk("[INIT] topics: %s*\n", APP_MQTT_TOPIC_BASE);

    return 0;
}
