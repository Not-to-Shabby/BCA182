/**
 * @file test_baby.c
 * @brief Host-native Unity tests for the Smart Baby Monitor decision logic:
 *        environmental hysteresis boundaries, the FAULT > ALERT > NORMAL
 *        state machine, the buzzer mute window, and the display names.
 *
 * The production src/baby_logic.c is compiled into this suite by PlatformIO
 * (test_build_src = yes), so a passing run proves the shipped decision
 * logic, not a mock.
 */

#include <unity.h>
#include <string.h>

#include "baby_logic.h"

void setUp(void)
{
    baby_logic_reset();
}

void tearDown(void)
{
}

/* --------------------------------------- environment: enter boundary edges */
/* With prev == ENV_OK an alert is entered only when the raw limit is
 * strictly crossed: 15.9 C enters TEMP_LOW, 16.0 C does not. */
void test_temp_low_enter_boundary(void)
{
    TEST_ASSERT_EQUAL_INT(ENV_TEMP_LOW, evaluate_environment(ENV_OK, 159, 400));
    TEST_ASSERT_EQUAL_INT(ENV_OK, evaluate_environment(ENV_OK, 160, 400));
    TEST_ASSERT_EQUAL_INT(ENV_OK, evaluate_environment(ENV_OK, 161, 400));
}

void test_temp_high_enter_boundary(void)
{
    TEST_ASSERT_EQUAL_INT(ENV_OK, evaluate_environment(ENV_OK, 319, 400));
    TEST_ASSERT_EQUAL_INT(ENV_OK, evaluate_environment(ENV_OK, 320, 400));
    TEST_ASSERT_EQUAL_INT(ENV_TEMP_HIGH, evaluate_environment(ENV_OK, 321, 400));
}

/* Mirrors the temp-high convention: 80.0 %RH itself is not "above 80", so
 * entering happens at 801. */
void test_humidity_enter_boundary(void)
{
    TEST_ASSERT_EQUAL_INT(ENV_OK, evaluate_environment(ENV_OK, 250, 800));
    TEST_ASSERT_EQUAL_INT(ENV_HUMIDITY_HIGH, evaluate_environment(ENV_OK, 250, 801));
}

/* -------------------------------------------------- environment: hysteresis */
/* An active TEMP_LOW holds until the reading recovers to
 * LOW_LIMIT + HYST = 17.0 C; 16.9 C and the 16.0 C limit still hold it. */
void test_temp_low_hysteresis_hold_and_clear(void)
{
    TEST_ASSERT_EQUAL_INT(ENV_TEMP_LOW, evaluate_environment(ENV_TEMP_LOW, 169, 400));
    TEST_ASSERT_EQUAL_INT(ENV_TEMP_LOW, evaluate_environment(ENV_TEMP_LOW, 160, 400));
    TEST_ASSERT_EQUAL_INT(ENV_OK, evaluate_environment(ENV_TEMP_LOW, 170, 400));
}

/* An active TEMP_HIGH holds until the reading falls below
 * HIGH_LIMIT - HYST = 31.0 C; 31.0 C still holds it. */
void test_temp_high_hysteresis_hold_and_clear(void)
{
    TEST_ASSERT_EQUAL_INT(ENV_TEMP_HIGH, evaluate_environment(ENV_TEMP_HIGH, 320, 400));
    TEST_ASSERT_EQUAL_INT(ENV_TEMP_HIGH, evaluate_environment(ENV_TEMP_HIGH, 310, 400));
    TEST_ASSERT_EQUAL_INT(ENV_OK, evaluate_environment(ENV_TEMP_HIGH, 309, 400));
}

/* An active HUMIDITY_HIGH holds until the reading falls below
 * HUM_HIGH_LIMIT - HUM_HYST = 75.0 %RH; 75.0 %RH still holds it. */
void test_humidity_hysteresis_hold_and_clear(void)
{
    TEST_ASSERT_EQUAL_INT(ENV_HUMIDITY_HIGH, evaluate_environment(ENV_HUMIDITY_HIGH, 250, 800));
    TEST_ASSERT_EQUAL_INT(ENV_HUMIDITY_HIGH, evaluate_environment(ENV_HUMIDITY_HIGH, 250, 750));
    TEST_ASSERT_EQUAL_INT(ENV_OK, evaluate_environment(ENV_HUMIDITY_HIGH, 250, 749));
}

/* ---------------------------------------- environment: co-occurrence rules */
/* Documented choice in baby_logic.c: temperature outranks humidity, so a
 * reading violating both limits reports the temperature alert. */
void test_temp_alert_outranks_humidity(void)
{
    TEST_ASSERT_EQUAL_INT(ENV_TEMP_HIGH,
                          evaluate_environment(ENV_OK, 330, 850));
    TEST_ASSERT_EQUAL_INT(ENV_TEMP_LOW,
                          evaluate_environment(ENV_HUMIDITY_HIGH, 150, 850));
}

/* The two temperature alerts cannot co-occur from one reading (< 16.0 C and
 * > 32.0 C are mutually exclusive). A stale prev combined with a reading
 * past the opposite limit resolves to the current extreme. */
void test_temp_alerts_cannot_cooccur(void)
{
    TEST_ASSERT_EQUAL_INT(ENV_TEMP_HIGH,
                          evaluate_environment(ENV_TEMP_LOW, 330, 400));
    TEST_ASSERT_EQUAL_INT(ENV_TEMP_LOW,
                          evaluate_environment(ENV_TEMP_HIGH, 150, 400));
}

/* --------------------------------------------------- system state machine */
/* All calls pass env_sensor_ok=true unless the test is specifically about a
 * missing/failing environment sensor (spec: a missing sensor is a defined
 * device FAULT, never evidence that the room is safe). */

void test_state_normal_stays_normal(void)
{
    TEST_ASSERT_EQUAL_INT(BABY_STATE_NORMAL,
                          evaluate_system_state(BABY_STATE_NORMAL, ENV_OK, false, true));
}

void test_state_alert_on_each_env_violation(void)
{
    TEST_ASSERT_EQUAL_INT(BABY_STATE_ALERT,
                          evaluate_system_state(BABY_STATE_NORMAL, ENV_TEMP_LOW, false, true));
    TEST_ASSERT_EQUAL_INT(BABY_STATE_ALERT,
                          evaluate_system_state(BABY_STATE_NORMAL, ENV_TEMP_HIGH, false, true));
    TEST_ASSERT_EQUAL_INT(BABY_STATE_ALERT,
                          evaluate_system_state(BABY_STATE_NORMAL, ENV_HUMIDITY_HIGH, false, true));
}

void test_state_fault_on_motion_even_with_ok_env(void)
{
    TEST_ASSERT_EQUAL_INT(BABY_STATE_FAULT,
                          evaluate_system_state(BABY_STATE_NORMAL, ENV_OK, true, true));
}

/* Missing/failing environment sensor => FAULT even with a nominally OK env. */
void test_state_fault_on_missing_env_sensor(void)
{
    TEST_ASSERT_EQUAL_INT(BABY_STATE_FAULT,
                          evaluate_system_state(BABY_STATE_NORMAL, ENV_OK, false, false));
    /* Stale readings must never masquerade as an environmental alert. */
    TEST_ASSERT_EQUAL_INT(BABY_STATE_FAULT,
                          evaluate_system_state(BABY_STATE_ALERT, ENV_TEMP_LOW, false, false));
}

void test_state_fault_overrides_alert(void)
{
    TEST_ASSERT_EQUAL_INT(BABY_STATE_FAULT,
                          evaluate_system_state(BABY_STATE_ALERT, ENV_TEMP_HIGH, true, true));
}

/* Stateless: with a healthy environment, no motion and a present sensor,
 * even a FAULT/ALERT input returns NORMAL immediately (no latching). */
void test_state_returns_to_normal_when_env_recovers(void)
{
    TEST_ASSERT_EQUAL_INT(BABY_STATE_NORMAL,
                          evaluate_system_state(BABY_STATE_ALERT, ENV_OK, false, true));
    TEST_ASSERT_EQUAL_INT(BABY_STATE_NORMAL,
                          evaluate_system_state(BABY_STATE_FAULT, ENV_OK, false, true));
}

/* ---------------------------------------------------- buzzer / mute window */
void test_buzzer_sounds_on_alert_without_ack(void)
{
    TEST_ASSERT_TRUE(baby_logic_buzzer_on(BABY_STATE_ALERT, 0U));
    TEST_ASSERT_TRUE(baby_logic_buzzer_on(BABY_STATE_ALERT, 123456U));
}

void test_normal_and_fault_never_sound_buzzer(void)
{
    TEST_ASSERT_FALSE(baby_logic_buzzer_on(BABY_STATE_NORMAL, 0U));
    TEST_ASSERT_FALSE(baby_logic_buzzer_on(BABY_STATE_FAULT, 0U));
    TEST_ASSERT_FALSE(baby_logic_buzzer_on(BABY_STATE_NORMAL, 999999U));
    TEST_ASSERT_FALSE(baby_logic_buzzer_on(BABY_STATE_FAULT, 999999U));
}

void test_acknowledge_mutes_until_expiry(void)
{
    baby_logic_acknowledge(1000U);
    TEST_ASSERT_FALSE(baby_logic_buzzer_on(BABY_STATE_ALERT, 2000U));
    /* One ms before the deadline the window is still active. */
    TEST_ASSERT_FALSE(baby_logic_buzzer_on(BABY_STATE_ALERT,
                                           1000U + ALERT_MUTE_MS - 1U));
    /* Exactly at the deadline the window has expired: buzzer sounds again. */
    TEST_ASSERT_TRUE(baby_logic_buzzer_on(BABY_STATE_ALERT,
                                          1000U + ALERT_MUTE_MS));
}

void test_reacknowledge_extends_mute_window(void)
{
    baby_logic_acknowledge(1000U);
    baby_logic_acknowledge(5000U); /* second press re-arms from now: 65000 */
    /* 61000 would have expired under the first ack alone. */
    TEST_ASSERT_FALSE(baby_logic_buzzer_on(BABY_STATE_ALERT, 61000U));
    TEST_ASSERT_FALSE(baby_logic_buzzer_on(BABY_STATE_ALERT, 64999U));
    TEST_ASSERT_TRUE(baby_logic_buzzer_on(BABY_STATE_ALERT, 65000U));
}

void test_reset_clears_mute_window(void)
{
    baby_logic_acknowledge(1000U);
    baby_logic_reset();
    TEST_ASSERT_TRUE(baby_logic_buzzer_on(BABY_STATE_ALERT, 2000U));
}

/* ------------------------------------------------------------ display names */
static void assert_names_non_null_and_distinct(const char *const *names,
                                               size_t count)
{
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_NOT_NULL(names[i]);
        TEST_ASSERT_TRUE(names[i][0] != '\0');
        for (size_t j = i + 1; j < count; j++) {
            TEST_ASSERT_TRUE(strcmp(names[i], names[j]) != 0);
        }
    }
}

void test_env_alert_names_are_non_null_and_distinct(void)
{
    const char *names[] = {
        env_alert_name(ENV_OK),
        env_alert_name(ENV_TEMP_LOW),
        env_alert_name(ENV_TEMP_HIGH),
        env_alert_name(ENV_HUMIDITY_HIGH),
    };
    assert_names_non_null_and_distinct(names, sizeof(names) / sizeof(names[0]));
}

void test_baby_state_names_are_non_null_and_distinct(void)
{
    const char *names[] = {
        baby_state_name(BABY_STATE_NORMAL),
        baby_state_name(BABY_STATE_ALERT),
        baby_state_name(BABY_STATE_FAULT),
    };
    assert_names_non_null_and_distinct(names, sizeof(names) / sizeof(names[0]));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_temp_low_enter_boundary);
    RUN_TEST(test_temp_high_enter_boundary);
    RUN_TEST(test_humidity_enter_boundary);
    RUN_TEST(test_temp_low_hysteresis_hold_and_clear);
    RUN_TEST(test_temp_high_hysteresis_hold_and_clear);
    RUN_TEST(test_humidity_hysteresis_hold_and_clear);
    RUN_TEST(test_temp_alert_outranks_humidity);
    RUN_TEST(test_temp_alerts_cannot_cooccur);
    RUN_TEST(test_state_normal_stays_normal);
    RUN_TEST(test_state_alert_on_each_env_violation);
    RUN_TEST(test_state_fault_on_motion_even_with_ok_env);
    RUN_TEST(test_state_fault_on_missing_env_sensor);
    RUN_TEST(test_state_fault_overrides_alert);
    RUN_TEST(test_state_returns_to_normal_when_env_recovers);
    RUN_TEST(test_buzzer_sounds_on_alert_without_ack);
    RUN_TEST(test_normal_and_fault_never_sound_buzzer);
    RUN_TEST(test_acknowledge_mutes_until_expiry);
    RUN_TEST(test_reacknowledge_extends_mute_window);
    RUN_TEST(test_reset_clears_mute_window);
    RUN_TEST(test_env_alert_names_are_non_null_and_distinct);
    RUN_TEST(test_baby_state_names_are_non_null_and_distinct);
    return UNITY_END();
}
