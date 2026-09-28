#ifndef LOGIC_H
#define LOGIC_H

#include <stdbool.h>
#include <stdint.h>

/* Hardware-independent decision logic (pure C, unit-testable on PC) */

/* ---------- Display Navigation ---------- */
typedef enum
{
    DISPLAY_TEMPERATURE = 0,
    DISPLAY_HUMIDITY,
    DISPLAY_LIGHT,
    DISPLAY_MOTION,
    DISPLAY_MODE_COUNT
} DisplayMode;

DisplayMode nextDisplayMode(DisplayMode mode);
DisplayMode previousDisplayMode(DisplayMode mode);
const char *displayModeName(DisplayMode mode);

/* ---------- Temperature Alarm Logic ---------- */
#define LOW_TEMPERATURE_LIMIT   18.0f   /* degrees C */
#define HIGH_TEMPERATURE_LIMIT  30.0f   /* degrees C */

typedef enum
{
    ALARM_NORMAL = 0,
    ALARM_LOW_TEMPERATURE,
    ALARM_HIGH_TEMPERATURE
} AlarmState;

AlarmState evaluateTemperature(float temperature);
const char *alarmStateName(AlarmState state);

/* ---------- Activity State Logic ---------- */
#define INACTIVITY_TIMEOUT_MS  15000U   /* 15 s timeout per lab requirements */

typedef enum
{
    SYSTEM_ACTIVE = 0,
    SYSTEM_INACTIVE
} SystemState;

SystemState evaluateSystemState(SystemState current,
                                bool motionDetected,
                                uint32_t msSinceLastMotion,
                                uint32_t timeoutMs);

const char *systemStateName(SystemState state);

#endif /* LOGIC_H */
