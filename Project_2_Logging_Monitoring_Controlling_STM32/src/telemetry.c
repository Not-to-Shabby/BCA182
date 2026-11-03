/**
 * @file telemetry.c
 * @brief Hardware-independent JSON builders and the strict MQTT command
 *        parser for BenchWatch. Runs unchanged on the board (Zephyr) and in
 *        host-native Unity tests.
 */

#include "telemetry.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ==========================================================================
 * Small string helpers
 * ========================================================================== */

static bool sbuf_eq(const char *s, size_t len, const char *literal)
{
    size_t n = strlen(literal);
    return len == n && memcmp(s, literal, n) == 0;
}

const char *telemetry_cmd_name(app_cmd_type_t type)
{
    switch (type) {
    case APP_CMD_BUZZER: return "buzzer";
    case APP_CMD_LED:    return "led";
    case APP_CMD_LCD:    return "lcd";
    case APP_CMD_PING:   return "ping";
    default:             return "none";
    }
}

const char *telemetry_led_mode_name(uint8_t mode)
{
    switch (mode) {
    case LED_MODE_OFF:     return "off";
    case LED_MODE_STATUS:  return "status";
    case LED_MODE_RAINBOW: return "rainbow";
    default:               return "unknown";
    }
}

bool telemetry_led_mode_from_name(const char *name, size_t len, uint8_t *out_mode)
{
    if (sbuf_eq(name, len, "off")) { *out_mode = LED_MODE_OFF; return true; }
    if (sbuf_eq(name, len, "status")) { *out_mode = LED_MODE_STATUS; return true; }
    if (sbuf_eq(name, len, "rainbow")) { *out_mode = LED_MODE_RAINBOW; return true; }
    return false;
}

/* snprintf-append helper: returns bytes appended (>=0) or TELEMETRY_ERR_BUFFER */
static int jappend(char *buf, size_t buflen, size_t *used, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *used, buflen - *used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= buflen - *used) {
        if (buflen > *used) {
            buf[*used] = '\0';
        }
        return TELEMETRY_ERR_BUFFER;
    }
    *used += (size_t)n;
    return TELEMETRY_OK;
}

/* ==========================================================================
 * Command parsing  {"id":N,"cmd":"...","arg":"..."}
 * ========================================================================== */

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        p++;
    }
    return p;
}

/** Parse a quoted string at *p; returns value span [vs,ve) or NULL on error. */
static const char *parse_quoted(const char **p, const char *end, const char **vs,
                                size_t *vl)
{
    if (*p >= end || **p != '"') {
        return NULL;
    }
    (*p)++;
    *vs = *p;
    while (*p < end && **p != '"') {
        if (**p == '\\') { /* no escapes supported -> strictly reject */
            return NULL;
        }
        (*p)++;
    }
    if (*p >= end) {
        return NULL;
    }
    *vl = (size_t)(*p - *vs);
    (*p)++; /* closing quote */
    return *p;
}

static const char *parse_uint(const char **p, const char *end, uint32_t *out)
{
    uint32_t v = 0;
    const char *start = *p;
    while (*p < end && **p >= '0' && **p <= '9') {
        v = v * 10u + (uint32_t)(**p - '0');
        (*p)++;
    }
    if (*p == start) {
        return NULL;
    }
    *out = v;
    return *p;
}

int telemetry_parse_command(const char *payload, size_t len, mqtt_command_t *out)
{
    if (payload == NULL || out == NULL) {
        return TELEMETRY_REJECT_MALFORMED;
    }
    if (len >= TELEMETRY_CMD_RAW_MAX) {
        return TELEMETRY_REJECT_TOO_LONG;
    }

    const char *p = payload;
    const char *end = payload + len;

    uint32_t id = 0;
    bool have_id = false, have_cmd = false;
    app_cmd_type_t type = APP_CMD_NONE;
    int arg = 0;
    bool have_arg = false;

    p = skip_ws(p, end);
    if (p >= end || *p != '{') {
        return TELEMETRY_REJECT_MALFORMED;
    }
    p++;

    for (;;) {
        p = skip_ws(p, end);
        if (p < end && *p == '}') {
            p++;
            break;
        }
        const char *vs;
        size_t vl;
        p = parse_quoted(&p, end, &vs, &vl);
        if (p == NULL) {
            return TELEMETRY_REJECT_MALFORMED;
        }
        p = skip_ws(p, end);
        if (p >= end || *p != ':') {
            return TELEMETRY_REJECT_MALFORMED;
        }
        p++;

        if (sbuf_eq(vs, vl, "id")) {
            p = skip_ws(p, end);
            p = parse_uint(&p, end, &id);
            if (p == NULL) {
                return TELEMETRY_REJECT_MALFORMED;
            }
            have_id = true;
        } else if (sbuf_eq(vs, vl, "cmd")) {
            p = skip_ws(p, end);
            p = parse_quoted(&p, end, &vs, &vl);
            if (p == NULL) {
                return TELEMETRY_REJECT_MALFORMED;
            }
            if (sbuf_eq(vs, vl, "buzzer")) {
                type = APP_CMD_BUZZER;
            } else if (sbuf_eq(vs, vl, "led")) {
                type = APP_CMD_LED;
            } else if (sbuf_eq(vs, vl, "lcd")) {
                type = APP_CMD_LCD;
            } else if (sbuf_eq(vs, vl, "ping")) {
                type = APP_CMD_PING;
            } else {
                return TELEMETRY_REJECT_UNKNOWN_CMD;
            }
            have_cmd = true;
        } else if (sbuf_eq(vs, vl, "arg")) {
            p = skip_ws(p, end);
            p = parse_quoted(&p, end, &vs, &vl);
            if (p == NULL) {
                return TELEMETRY_REJECT_MALFORMED;
            }
            if (sbuf_eq(vs, vl, "on")) {
                arg = 1;
            } else if (sbuf_eq(vs, vl, "off")) {
                arg = 0;
            } else if (sbuf_eq(vs, vl, "status")) {
                arg = (int)LED_MODE_STATUS;
            } else if (sbuf_eq(vs, vl, "rainbow")) {
                arg = (int)LED_MODE_RAINBOW;
            } else {
                return TELEMETRY_REJECT_BAD_ARG;
            }
            have_arg = true;
        } else {
            return TELEMETRY_REJECT_MALFORMED; /* unknown key: strictly reject */
        }

        p = skip_ws(p, end);
        if (p < end && *p == ',') {
            p++;
            continue;
        }
        if (p < end && *p == '}') {
            p++;
            break;
        }
        return TELEMETRY_REJECT_MALFORMED;
    }

    if (skip_ws(p, end) != end) {
        return TELEMETRY_REJECT_MALFORMED; /* trailing garbage */
    }
    if (!have_id || !have_cmd) {
        return TELEMETRY_REJECT_MALFORMED;
    }

    /* Validate arg against the command (arg is otherwise meaningless). */
    switch (type) {
    case APP_CMD_BUZZER:
    case APP_CMD_LCD:
        if (!have_arg || (arg != 0 && arg != 1)) {
            return TELEMETRY_REJECT_BAD_ARG;
        }
        break;
    case APP_CMD_LED:
        if (!have_arg || (arg != (int)LED_MODE_OFF && arg != (int)LED_MODE_STATUS &&
                          arg != (int)LED_MODE_RAINBOW)) {
            return TELEMETRY_REJECT_BAD_ARG;
        }
        break;
    case APP_CMD_PING:
        if (have_arg) {
            return TELEMETRY_REJECT_BAD_ARG; /* ping takes no argument */
        }
        break;
    default:
        return TELEMETRY_REJECT_MALFORMED;
    }

    out->id = id;
    out->type = type;
    out->arg = arg;
    memcpy(out->raw, payload, len);
    out->raw[len] = '\0';
    return TELEMETRY_OK;
}

/* ==========================================================================
 * JSON builders
 * ========================================================================== */

int telemetry_build_telemetry(const telemetry_snapshot_t *s, const char *device_id,
                              char *buf, size_t buflen)
{
    if (s == NULL || device_id == NULL || buf == NULL || buflen == 0) {
        return TELEMETRY_ERR_BUFFER;
    }
    size_t used = 0;
    int rc;
    rc = jappend(buf, buflen, &used,
                 "{\"device_id\":\"%s\",\"seq\":%u,\"ts\":%lld,"
                 "\"temperature_c\":%.2f,\"humidity_pct\":%.2f,"
                 "\"light\":%u,\"proximity\":%u,"
                 "\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,"
                 "\"gx\":%.3f,\"gy\":%.3f,\"gz\":%.3f,"
                 "\"motion_mag\":%.3f,\"motion_event\":%s,"
                 "\"buzzer\":%s,\"led_mode\":\"%s\",\"wifi\":%s,\"mqtt\":%s}",
                 device_id, (unsigned)s->seq, (long long)s->ts_ms,
                 (double)s->temperature_c, (double)s->humidity_pct,
                 (unsigned)s->light, (unsigned)s->proximity,
                 (double)s->ax, (double)s->ay, (double)s->az,
                 (double)s->gx, (double)s->gy, (double)s->gz,
                 (double)s->motion_mag, s->motion_event ? "true" : "false",
                 s->buzzer_on ? "true" : "false", telemetry_led_mode_name(s->led_mode),
                 s->wifi_ok ? "true" : "false", s->mqtt_ok ? "true" : "false");
    return rc;
}

int telemetry_build_ack(const mqtt_command_t *cmd, bool accepted, const char *reason,
                        const char *actual, int64_t ts_ms, char *buf, size_t buflen)
{
    if (cmd == NULL || reason == NULL || actual == NULL || buf == NULL || buflen == 0) {
        return TELEMETRY_ERR_BUFFER;
    }
    size_t used = 0;
    return jappend(buf, buflen, &used,
                   "{\"id\":%u,\"accepted\":%s,\"reason\":\"%s\","
                   "\"actual\":\"%s\",\"ts\":%lld}",
                   (unsigned)cmd->id, accepted ? "true" : "false", reason, actual,
                   (long long)ts_ms);
}

int telemetry_build_status(bool online, bool buzzer_on, uint8_t led_mode, bool wifi_ok,
                           bool mqtt_ok, uint32_t uptime_s, const char *device_id,
                           char *buf, size_t buflen)
{
    if (device_id == NULL || buf == NULL || buflen == 0) {
        return TELEMETRY_ERR_BUFFER;
    }
    size_t used = 0;
    return jappend(buf, buflen, &used,
                   "{\"device_id\":\"%s\",\"online\":%s,\"buzzer\":%s,"
                   "\"led_mode\":\"%s\",\"wifi\":%s,\"mqtt\":%s,\"uptime_s\":%u}",
                   device_id, online ? "true" : "false", buzzer_on ? "true" : "false",
                   telemetry_led_mode_name(led_mode), wifi_ok ? "true" : "false",
                   mqtt_ok ? "true" : "false", (unsigned)uptime_s);
}

/* ==========================================================================
 * Priority rules
 * ========================================================================== */

bool control_command_allowed(bool local_mute_active, app_cmd_type_t type)
{
    if (local_mute_active && type == APP_CMD_BUZZER) {
        return false;
    }
    return true;
}
