/**
 * @file control_task.c
 * @brief Control and local-feedback thread: applies validated MQTT commands
 *        to the buzzer/LED matrix, serves local buttons, and reports the
 *        actual resulting state through acknowledgments.
 *
 * Documented priority rules (assignment requirement 4):
 *  - KEY0 opens a 60 s LOCAL MUTE window: remote buzzer commands received
 *    during the window are acked as rejected with reason "local_override";
 *  - KEY1 cycles the LED matrix mode locally;
 *  - a bench disturbance (motion event) beeps locally unless muted.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

#include <string.h>

#include "app_config.h"
#include "app_context.h"
#include "led_matrix.h"
#include "telemetry.h"

static const struct device *s_buzzer_dev;
static const struct device *s_key_dev;
static bool s_buzzer_on;
static uint8_t s_led_mode = LED_MODE_STATUS;
static int64_t s_mute_until;

static bool s_key0_prev = true; /* active LOW */
static bool s_key1_prev = true;
static uint8_t s_key0_cnt;
static uint8_t s_key1_cnt;

static void buzzer_set(bool on)
{
    if (s_buzzer_dev != NULL) {
        gpio_pin_set(s_buzzer_dev, APP_BUZZER_PIN, on ? 1 : 0);
    }
    s_buzzer_on = on;
}

static void apply_command(const mqtt_command_t *cmd)
{
    bool allowed = control_command_allowed(k_uptime_get() < s_mute_until, cmd->type);
    if (!allowed) {
        app_ack_put(cmd, false, "local_override", "buzzer=unchanged");
        printk("[CTRL] command %u rejected (local mute window)\n",
               (unsigned)cmd->id);
        return;
    }

    switch (cmd->type) {
    case APP_CMD_BUZZER:
        buzzer_set(cmd->arg != 0);
        app_ack_put(cmd, true, "ok", s_buzzer_on ? "buzzer=on" : "buzzer=off");
        printk("[CTRL] buzzer -> %s (cmd %u)\n", s_buzzer_on ? "on" : "off",
               (unsigned)cmd->id);
        break;

    case APP_CMD_LED:
        s_led_mode = (uint8_t)cmd->arg;
        app_ack_put(cmd, true, "ok", s_led_mode == LED_MODE_OFF ? "led=off"
                                   : "led=on");
        printk("[CTRL] led_mode -> %s (cmd %u)\n", telemetry_led_mode_name(s_led_mode),
               (unsigned)cmd->id);
        break;

    case APP_CMD_LCD:
        /* Handled by ui_task via state; ack reflects accepted request. */
        app_ack_put(cmd, true, "ok", cmd->arg ? "lcd=on" : "lcd=off");
        break;

    case APP_CMD_PING:
        app_ack_put(cmd, true, "ok", "pong");
        break;

    default:
        app_ack_put(cmd, false, "unknown_command", "");
        break;
    }
}

static void led_matrix_render(void)
{
    telemetry_snapshot_t snap;
    app_state_get(&snap);

    switch (s_led_mode) {
    case LED_MODE_OFF:
        led_matrix_clear();
        led_matrix_show();
        break;
    case LED_MODE_RAINBOW: {
        static uint8_t hue;
        for (uint8_t i = 0; i < 19u; i++) {
            uint8_t h = (uint8_t)(hue + i * 13u);
            uint8_t r = (h < 85u) ? (85u - h) : ((h > 170u) ? (h - 170u) : 0u);
            uint8_t g = (h < 85u) ? h : ((h > 170u) ? 0u : (170u - h));
            uint8_t b = (h < 85u) ? 0u : ((h > 170u) ? (255u - h) : (h - 85u));
            led_matrix_set_pixel(i, r, g, b);
        }
        hue += 5u;
        led_matrix_show();
        break;
    }
    case LED_MODE_STATUS:
    default: {
        uint8_t r = 0, g = 0, b = 0;
        if (snap.mqtt_ok && snap.wifi_ok) {
            g = 60; /* healthy link */
        } else if (snap.wifi_ok) {
            r = 80, g = 40; /* Wi-Fi up, broker down */
        } else {
            r = 80; /* offline */
        }
        for (uint8_t i = 0; i < 19u; i++) {
            led_matrix_set_pixel(i, r, g, b);
        }
        led_matrix_show();
        break;
    }
    }
}

void control_task_run(void)
{
    printk("[CTRL] control thread started\n");

    s_buzzer_dev = device_get_binding(APP_BUZZER_PORT_NAME);
    if (s_buzzer_dev != NULL) {
        gpio_pin_configure(s_buzzer_dev, APP_BUZZER_PIN, GPIO_OUTPUT_INACTIVE);
    } else {
        printk("[CTRL] buzzer port %s unavailable; check APP_BUZZER pin map\n",
               APP_BUZZER_PORT_NAME);
    }

    s_key_dev = device_get_binding(APP_KEY0_PORT_NAME);
    if (s_key_dev != NULL) {
        gpio_pin_configure(s_key_dev, APP_KEY0_PIN,
                           GPIO_INPUT | GPIO_PULL_UP | GPIO_ACTIVE_LOW);
        gpio_pin_configure(s_key_dev, APP_KEY1_PIN,
                           GPIO_INPUT | GPIO_PULL_UP | GPIO_ACTIVE_LOW);
    }

    int64_t next_render = 0;

    for (;;) {
        k_msleep(APP_CONTROL_PERIOD_MS);

        /* --- Local input (debounced, 2 consecutive reads) ---------------- */
        if (s_key_dev != NULL) {
            bool k0 = gpio_pin_get(s_key_dev, APP_KEY0_PIN) > 0;
            bool k1 = gpio_pin_get(s_key_dev, APP_KEY1_PIN) > 0;

            if (k0 == s_key0_prev) {
                if (++s_key0_cnt >= 2u && k0 == false && s_key0_prev == false) {
                    /* falling edge: open local mute window */
                    s_mute_until = k_uptime_get() + APP_LOCAL_MUTE_MS;
                    buzzer_set(false);
                    printk("[CTRL] local mute window opened (KEY0)\n");
                }
            } else {
                s_key0_cnt = 0;
            }
            s_key0_prev = k0;

            if (k1 == s_key1_prev) {
                if (++s_key1_cnt >= 2u && k1 == false && s_key1_prev == false) {
                    s_led_mode = (uint8_t)((s_led_mode + 1u) % LED_MODE_COUNT);
                    printk("[CTRL] local led_mode -> %s (KEY1)\n",
                           telemetry_led_mode_name(s_led_mode));
                }
            } else {
                s_key1_cnt = 0;
            }
            s_key1_prev = k1;
        }

        /* --- Remote commands --------------------------------------------- */
        mqtt_command_t cmd;
        while (app_cmd_get(&cmd, K_NO_WAIT)) {
            apply_command(&cmd);
        }

        /* --- Local feedback: disturbance beep ----------------------------- */
        telemetry_snapshot_t snap;
        app_state_get(&snap);
        if (snap.motion_event && !s_buzzer_on &&
            k_uptime_get() >= s_mute_until) {
            buzzer_set(true);
        } else if (s_buzzer_on && !snap.motion_event) {
            buzzer_set(false);
        }

        /* --- LED matrix refresh (5 Hz) ------------------------------------ */
        if (k_uptime_get() >= next_render) {
            led_matrix_render();
            next_render = k_uptime_get() + 200;
        }
    }
}

K_THREAD_DEFINE(control_thread, 1536, control_task_run, NULL, NULL, NULL, 6, 0, 0);
