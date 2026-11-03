/**
 * @file telemetry.h
 * @brief Pure, hardware-independent telemetry/command logic for BenchWatch.
 *
 * Everything in this header is implemented without RTOS or hardware calls so
 * the same sources compile in host-native Unity tests (test/test_telemetry)
 * and on the board. JSON schemas follow the assignment's suggested topic
 * structure for bca182/<group-id>/... (telemetry, status, cmd, ack).
 */

#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TELEMETRY_JSON_MAX    384
#define TELEMETRY_CMD_RAW_MAX 96

/* LED matrix modes (status = link/MQTT health colors, rainbow = idle sweep) */
#define LED_MODE_OFF     0u
#define LED_MODE_STATUS  1u
#define LED_MODE_RAINBOW 2u
#define LED_MODE_COUNT   3u

/** One aggregated measurement set plus the actual actuator/health state. */
typedef struct {
    uint32_t seq;        /* monotonic sample counter                      */
    int64_t ts_ms;       /* device uptime (ms) at capture                 */
    float temperature_c;
    float humidity_pct;
    uint16_t light;      /* AP3216C ambient light                         */
    uint16_t proximity;  /* AP3216C proximity                             */
    float ax, ay, az;    /* ICM20608 accelerometer (g)                    */
    float gx, gy, gz;    /* ICM20608 gyroscope (rad/s)                    */
    float motion_mag;    /* |a| in g                                      */
    bool motion_event;   /* bench disturbance flag                        */
    /* Actual output state (kept separate from any requested state) */
    bool buzzer_on;
    uint8_t led_mode;
    bool wifi_ok;
    bool mqtt_ok;
} telemetry_snapshot_t;

/* ---- Commands ------------------------------------------------------------ */

typedef enum {
    APP_CMD_NONE = 0,
    APP_CMD_BUZZER,
    APP_CMD_LED,
    APP_CMD_LCD,
    APP_CMD_PING,
} app_cmd_type_t;

typedef struct {
    uint32_t id; /* command_id echoed in the ack */
    app_cmd_type_t type;
    int arg; /* buzzer/lcd: 0=off 1=on; led: LED_MODE_x */
    char raw[TELEMETRY_CMD_RAW_MAX]; /* original payload for diagnostics */
} mqtt_command_t;

/* Parse results */
#define TELEMETRY_OK                0
#define TELEMETRY_REJECT_MALFORMED  -1
#define TELEMETRY_REJECT_UNKNOWN_CMD -2
#define TELEMETRY_REJECT_BAD_ARG    -3
#define TELEMETRY_REJECT_TOO_LONG   -4
#define TELEMETRY_ERR_BUFFER        -10

/**
 * Strict parser for `{"id":<n>,"cmd":"buzzer|led|lcd|ping","arg":"..."}`.
 * Unknown keys, missing id/cmd, or an arg invalid for the command are
 * rejected (never guessed). Always NUL-terminates out->raw on success.
 */
int telemetry_parse_command(const char *payload, size_t len, mqtt_command_t *out);

/** Human-readable command name for ack "actual" fields. */
const char *telemetry_cmd_name(app_cmd_type_t type);

const char *telemetry_led_mode_name(uint8_t mode);
bool telemetry_led_mode_from_name(const char *name, size_t len, uint8_t *out_mode);

/* ---- JSON builders (all return TELEMETRY_OK or TELEMETRY_ERR_BUFFER) ----- */

int telemetry_build_telemetry(const telemetry_snapshot_t *s, const char *device_id,
                              char *buf, size_t buflen);

/** actual: resulting real output state, e.g. "buzzer=on"; may be "". */
int telemetry_build_ack(const mqtt_command_t *cmd, bool accepted, const char *reason,
                        const char *actual, int64_t ts_ms, char *buf, size_t buflen);

int telemetry_build_status(bool online, bool buzzer_on, uint8_t led_mode, bool wifi_ok,
                           bool mqtt_ok, uint32_t uptime_s, const char *device_id,
                           char *buf, size_t buflen);

/* ---- Control priority rules ---------------------------------------------- */

/**
 * Documented priority rule: while a locally triggered mute window is active,
 * remote buzzer commands are suppressed (ack "rejected", reason
 * "local_override"); LED/LCD/ping commands always pass.
 */
bool control_command_allowed(bool local_mute_active, app_cmd_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* TELEMETRY_H */
