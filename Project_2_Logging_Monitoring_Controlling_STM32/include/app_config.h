/**
 * @file app_config.h
 * @brief Compile-time settings for the BenchWatch logging/monitoring/control
 *        system (BCA182 Project 2).
 *
 * Credentials are never stored here: Wi-Fi credentials live in internal flash
 * via wifi_storage (set once through the serial console or the onboarding
 * portal) and the broker address below points at the LAN Docker host.
 */

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ---- Identity (must be unique across student groups) --------------------- */
#define APP_DEVICE_ID       "benchwatch-01"
#define APP_MQTT_TOPIC_BASE "bca182/group-1/benchwatch"

/* Derived topic names (assignment-suggested structure) */
#define APP_TOPIC_TELEMETRY APP_MQTT_TOPIC_BASE "/telemetry"
#define APP_TOPIC_STATUS    APP_MQTT_TOPIC_BASE "/status"
#define APP_TOPIC_CMD       APP_MQTT_TOPIC_BASE "/cmd/indicator"
#define APP_TOPIC_ACK       APP_MQTT_TOPIC_BASE "/ack"

/* ---- Broker (LAN Mosquitto in tools/stack/docker-compose.yml) ------------ */
#define APP_BROKER_HOST "192.168.1.2" /* the lab PC's LAN IP */
#define APP_BROKER_PORT 1883u

/* ---- Timing (assignment: sample env 1-5 s, publish 5-10 s) --------------- */
#define APP_SENSOR_PERIOD_MS 200u  /* motion sampling window                */
#define APP_ENV_PERIOD_MS    2000u /* temperature/humidity + light/proximity */
#define APP_MQTT_PERIOD_MS   5000u /* aggregated telemetry publish          */
#define APP_STATUS_PERIOD_MS 30000u /* heartbeat status publish              */
#define APP_UI_PERIOD_MS     500u
#define APP_CONTROL_PERIOD_MS 50u

/* ---- Thresholds ----------------------------------------------------------- */
/* |accel magnitude - 1 g| beyond this counts as a bench disturbance event. */
#define APP_MOTION_THRESHOLD_G 0.35f
/* AP3216C proximity above this wakes the display backlight (someone at bench). */
#define APP_PROXIMITY_WAKE 100u
/* Local mute window opened by KEY0 (local input outranks remote buzzer cmds). */
#define APP_LOCAL_MUTE_MS 60000

/* ---- Actuator pins -------------------------------------------------------- */
/* Onboard buzzer: PASSIVE, driven by TIM3_CH3 PWM on PB0 (pin verified in the
 * Laboratory 2 audio engine). Beep = 2.5 kHz 50% duty; TIM3 kernel clock is
 * 84 MHz (APB1 timer) / 33600 -> 2.5 kHz. */
#define APP_BUZZER_PWM_NODE      pwm3
#define APP_BUZZER_PWM_CH        3u
#define APP_BUZZER_PERIOD_CYCLES 33600u
/* On-board pushbuttons (verified in Laboratory 4, active LOW). */
#define APP_KEY0_PORT_NAME "gpioc"
#define APP_KEY0_PIN       0
#define APP_KEY1_PORT_NAME "gpioc"
#define APP_KEY1_PIN       1

#endif /* APP_CONFIG_H */
