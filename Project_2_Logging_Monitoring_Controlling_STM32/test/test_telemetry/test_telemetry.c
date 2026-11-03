/**
 * @file test_telemetry.c
 * @brief Host-native Unity tests for BenchWatch's hardware-independent logic:
 *        telemetry JSON builder, strict MQTT command parser, and the
 *        local-priority control rule. The same sources run on the board.
 */

#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "../../src/telemetry.c"

/* Device id for tests (kept out of app_config.h on purpose: that header is
 * shared with firmware builds). */
#define APP_TEST_DEVICE "benchwatch-01"

void setUp(void)
{
}
void tearDown(void)
{
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static telemetry_snapshot_t make_snapshot(void)
{
    telemetry_snapshot_t s;
    memset(&s, 0, sizeof(s));
    s.seq = 42;
    s.ts_ms = 123456;
    s.temperature_c = 25.5f;
    s.humidity_pct = 60.25f;
    s.light = 512;
    s.proximity = 7;
    s.ax = 0.01f;
    s.ay = -0.02f;
    s.az = 1.0f;
    s.motion_mag = 1.001f;
    s.buzzer_on = true;
    s.led_mode = LED_MODE_STATUS;
    s.wifi_ok = true;
    s.mqtt_ok = true;
    return s;
}

/* ------------------------------------------------------------------ */
/* Telemetry JSON builder                                              */
/* ------------------------------------------------------------------ */

void test_telemetry_json_contains_all_fields(void)
{
    telemetry_snapshot_t s = make_snapshot();
    char buf[TELEMETRY_JSON_MAX];
    TEST_ASSERT_EQUAL_INT(TELEMETRY_OK,
                          telemetry_build_telemetry(&s, APP_TEST_DEVICE, buf,
                                                    sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"device_id\":\"" APP_TEST_DEVICE "\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"seq\":42"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ts\":123456"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"temperature_c\":25.5"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"humidity_pct\":60.25"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"light\":512"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"proximity\":7"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"motion_mag\":1.0"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"motion_event\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"buzzer\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"led_mode\":\"status\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"wifi\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mqtt\":true"));
}

void test_telemetry_json_buffer_too_small(void)
{
    telemetry_snapshot_t s = make_snapshot();
    char tiny[32];
    TEST_ASSERT_EQUAL_INT(TELEMETRY_ERR_BUFFER,
                          telemetry_build_telemetry(&s, APP_TEST_DEVICE, tiny,
                                                    sizeof(tiny)));
    TEST_ASSERT_EQUAL_CHAR('\0', tiny[sizeof(tiny) - 1]); /* stays terminated */
}

void test_telemetry_json_null_args(void)
{
    telemetry_snapshot_t s = make_snapshot();
    char buf[TELEMETRY_JSON_MAX];
    TEST_ASSERT_EQUAL_INT(TELEMETRY_ERR_BUFFER,
                          telemetry_build_telemetry(NULL, APP_TEST_DEVICE, buf,
                                                    sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(TELEMETRY_ERR_BUFFER,
                          telemetry_build_telemetry(&s, NULL, buf, sizeof(buf)));
}

/* ------------------------------------------------------------------ */
/* Command parser - accepted forms                                     */
/* ------------------------------------------------------------------ */

void test_parse_buzzer_on_off(void)
{
    mqtt_command_t cmd;
    const char *on = "{\"id\":7,\"cmd\":\"buzzer\",\"arg\":\"on\"}";
    const char *off = "{\"id\":8,\"cmd\":\"buzzer\",\"arg\":\"off\"}";
    TEST_ASSERT_EQUAL_INT(TELEMETRY_OK,
                          telemetry_parse_command(on, strlen(on), &cmd));
    TEST_ASSERT_EQUAL_UINT32(7, cmd.id);
    TEST_ASSERT_EQUAL_INT(APP_CMD_BUZZER, cmd.type);
    TEST_ASSERT_EQUAL_INT(1, cmd.arg);

    TEST_ASSERT_EQUAL_INT(TELEMETRY_OK,
                          telemetry_parse_command(off, strlen(off), &cmd));
    TEST_ASSERT_EQUAL_INT(0, cmd.arg);
}

void test_parse_led_modes(void)
{
    mqtt_command_t cmd;
    const char *payloads[] = { "off", "status", "rainbow" };
    const int modes[] = { (int)LED_MODE_OFF, (int)LED_MODE_STATUS,
                          (int)LED_MODE_RAINBOW };
    for (int i = 0; i < 3; i++) {
        char payload[64];
        int n = snprintf(payload, sizeof(payload),
                         "{\"id\":%d,\"cmd\":\"led\",\"arg\":\"%s\"}", 10 + i,
                         payloads[i]);
        TEST_ASSERT_EQUAL_INT(TELEMETRY_OK,
                              telemetry_parse_command(payload, (size_t)n, &cmd));
        TEST_ASSERT_EQUAL_INT(APP_CMD_LED, cmd.type);
        TEST_ASSERT_EQUAL_INT(modes[i], cmd.arg);
    }
}

void test_parse_ping_without_arg(void)
{
    mqtt_command_t cmd;
    const char *payload = "{\"id\":99,\"cmd\":\"ping\"}";
    TEST_ASSERT_EQUAL_INT(TELEMETRY_OK,
                          telemetry_parse_command(payload, strlen(payload), &cmd));
    TEST_ASSERT_EQUAL_INT(APP_CMD_PING, cmd.type);
}

void test_parse_whitespace_and_key_order_tolerated(void)
{
    mqtt_command_t cmd;
    const char *payload = " { \"arg\" : \"on\" , \"cmd\":\"lcd\" , \"id\":5 } ";
    TEST_ASSERT_EQUAL_INT(TELEMETRY_OK,
                          telemetry_parse_command(payload, strlen(payload), &cmd));
    TEST_ASSERT_EQUAL_INT(APP_CMD_LCD, cmd.type);
    TEST_ASSERT_EQUAL_INT(1, cmd.arg);
    TEST_ASSERT_EQUAL_UINT32(5, cmd.id);
    TEST_ASSERT_EQUAL_STRING("{ \"arg\" : \"on\" , \"cmd\":\"lcd\" , \"id\":5 } ",
                             cmd.raw + 1); /* raw copies the whole payload */
}

/* ------------------------------------------------------------------ */
/* Command parser - rejections                                         */
/* ------------------------------------------------------------------ */

void test_parse_reject_malformed(void)
{
    mqtt_command_t cmd;
    const char *bad[] = {
        "{",
        "{}",
        "{\"cmd\":\"buzzer\",\"arg\":\"on\"}",          /* missing id      */
        "{\"id\":1}",                                  /* missing cmd     */
        "{\"id\":1,\"cmd\":\"buzzer\",\"arg\":\"on\"", /* truncated       */
        "{\"id\":1,\"cmd\":\"buzzer\",\"arg\":\"on\"}x", /* trailing junk */
        "{\"id\":x,\"cmd\":\"buzzer\",\"arg\":\"on\"}",
        "{\"id\":1,\"extra\":true,\"cmd\":\"buzzer\",\"arg\":\"on\"}",
        "{\"id\":1,\"cmd\":\"buzzer\",\"arg\":on}", /* unquoted value */
        "[]",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(TELEMETRY_REJECT_MALFORMED,
                                      telemetry_parse_command(bad[i], strlen(bad[i]),
                                                              &cmd),
                                      bad[i]);
    }
}

void test_parse_reject_unknown_cmd_and_bad_arg(void)
{
    mqtt_command_t cmd;
    const char *servo = "{\"id\":1,\"cmd\":\"servo\",\"arg\":\"on\"}";
    const char *blink_b = "{\"id\":1,\"cmd\":\"buzzer\",\"arg\":\"blink\"}";
    const char *blink_l = "{\"id\":1,\"cmd\":\"led\",\"arg\":\"blink\"}";
    const char *ping_arg = "{\"id\":1,\"cmd\":\"ping\",\"arg\":\"on\"}";
    TEST_ASSERT_EQUAL_INT(TELEMETRY_REJECT_UNKNOWN_CMD,
                          telemetry_parse_command(servo, strlen(servo), &cmd));
    TEST_ASSERT_EQUAL_INT(TELEMETRY_REJECT_BAD_ARG,
                          telemetry_parse_command(blink_b, strlen(blink_b), &cmd));
    TEST_ASSERT_EQUAL_INT(TELEMETRY_REJECT_BAD_ARG,
                          telemetry_parse_command(blink_l, strlen(blink_l), &cmd));
    TEST_ASSERT_EQUAL_INT(TELEMETRY_REJECT_BAD_ARG,
                          telemetry_parse_command(ping_arg, strlen(ping_arg), &cmd));
}

void test_parse_reject_too_long(void)
{
    mqtt_command_t cmd;
    char big[TELEMETRY_CMD_RAW_MAX + 8];
    memset(big, 'a', sizeof(big));
    big[0] = '{';
    big[sizeof(big) - 1] = '}';
    TEST_ASSERT_EQUAL_INT(TELEMETRY_REJECT_TOO_LONG,
                          telemetry_parse_command(big, sizeof(big), &cmd));
}

void test_parse_null_args(void)
{
    mqtt_command_t cmd;
    const char *ok = "{\"id\":1,\"cmd\":\"ping\"}";
    TEST_ASSERT_EQUAL_INT(TELEMETRY_REJECT_MALFORMED,
                          telemetry_parse_command(NULL, strlen(ok), &cmd));
    TEST_ASSERT_EQUAL_INT(TELEMETRY_REJECT_MALFORMED,
                          telemetry_parse_command(ok, strlen(ok), NULL));
}

/* ------------------------------------------------------------------ */
/* Ack/status builders and priority rule                               */
/* ------------------------------------------------------------------ */

void test_ack_reflects_actual_state(void)
{
    mqtt_command_t cmd;
    cmd.id = 7;
    cmd.type = APP_CMD_BUZZER;
    cmd.arg = 1;
    char buf[TELEMETRY_JSON_MAX];
    TEST_ASSERT_EQUAL_INT(TELEMETRY_OK,
                          telemetry_build_ack(&cmd, true, "ok", "buzzer=on", 1000,
                                              buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"id\":7"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"accepted\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"actual\":\"buzzer=on\""));

    TEST_ASSERT_EQUAL_INT(TELEMETRY_OK,
                          telemetry_build_ack(&cmd, false, "local_override", "", 2000,
                                              buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"accepted\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"reason\":\"local_override\""));
}

void test_status_json(void)
{
    char buf[TELEMETRY_JSON_MAX];
    TEST_ASSERT_EQUAL_INT(TELEMETRY_OK,
                          telemetry_build_status(true, false, LED_MODE_RAINBOW, true,
                                                 false, 77, APP_TEST_DEVICE, buf,
                                                 sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"online\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"led_mode\":\"rainbow\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mqtt\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"uptime_s\":77"));
}

void test_local_priority_overrides_remote_buzzer_only(void)
{
    /* Local mute active: remote buzzer suppressed, everything else passes. */
    TEST_ASSERT_FALSE(control_command_allowed(true, APP_CMD_BUZZER));
    TEST_ASSERT_TRUE(control_command_allowed(true, APP_CMD_LED));
    TEST_ASSERT_TRUE(control_command_allowed(true, APP_CMD_LCD));
    TEST_ASSERT_TRUE(control_command_allowed(true, APP_CMD_PING));
    /* No local mute: everything passes. */
    TEST_ASSERT_TRUE(control_command_allowed(false, APP_CMD_BUZZER));
}

/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_telemetry_json_contains_all_fields);
    RUN_TEST(test_telemetry_json_buffer_too_small);
    RUN_TEST(test_telemetry_json_null_args);
    RUN_TEST(test_parse_buzzer_on_off);
    RUN_TEST(test_parse_led_modes);
    RUN_TEST(test_parse_ping_without_arg);
    RUN_TEST(test_parse_whitespace_and_key_order_tolerated);
    RUN_TEST(test_parse_reject_malformed);
    RUN_TEST(test_parse_reject_unknown_cmd_and_bad_arg);
    RUN_TEST(test_parse_reject_too_long);
    RUN_TEST(test_parse_null_args);
    RUN_TEST(test_ack_reflects_actual_state);
    RUN_TEST(test_status_json);
    RUN_TEST(test_local_priority_overrides_remote_buzzer_only);
    return UNITY_END();
}
