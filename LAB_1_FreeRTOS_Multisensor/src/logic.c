#include "logic.h"

/* ---------- Display View Navigation ---------- */

DisplayMode nextDisplayMode(DisplayMode mode)
{
    if (mode >= DISPLAY_MODE_COUNT) { return DISPLAY_TEMPERATURE; }
    return (DisplayMode)(((int)mode + 1) % (int)DISPLAY_MODE_COUNT);
}

DisplayMode previousDisplayMode(DisplayMode mode)
{
    if (mode >= DISPLAY_MODE_COUNT) { return DISPLAY_TEMPERATURE; }
    return (DisplayMode)(((int)mode + (int)DISPLAY_MODE_COUNT - 1) % (int)DISPLAY_MODE_COUNT);
}

const char *displayModeName(DisplayMode mode)
{
    switch (mode)
    {
        case DISPLAY_TEMPERATURE: return "Temp";
        case DISPLAY_HUMIDITY:    return "Humidity";
        case DISPLAY_LIGHT:       return "Light Level";
        case DISPLAY_MOTION:      return "Motion";
        default:                  return "?";
    }
}

/* ---------- Temperature Limit Evaluation ---------- */

AlarmState evaluateTemperature(float temperature)
{
    if (temperature < LOW_TEMPERATURE_LIMIT)
    {
        return ALARM_LOW_TEMPERATURE;
    }
    if (temperature > HIGH_TEMPERATURE_LIMIT)
    {
        return ALARM_HIGH_TEMPERATURE;
    }
    return ALARM_NORMAL;
}

const char *alarmStateName(AlarmState state)
{
    switch (state)
    {
        case ALARM_NORMAL:           return "NORMAL";
        case ALARM_LOW_TEMPERATURE:  return "TOO_COLD";
        case ALARM_HIGH_TEMPERATURE: return "TOO_HOT";
        default:                     return "?";
    }
}

/* ---------- Activity State Machine ---------- */

SystemState evaluateSystemState(SystemState current,
                                bool motionDetected,
                                uint32_t msSinceLastMotion,
                                uint32_t timeoutMs)
{
    if (motionDetected)
    {
        return SYSTEM_ACTIVE;
    }
    if (current == SYSTEM_ACTIVE && msSinceLastMotion >= timeoutMs)
    {
        return SYSTEM_INACTIVE;
    }
    return current;
}

const char *systemStateName(SystemState state)
{
    return (state == SYSTEM_ACTIVE) ? "ACTIVE" : "INACTIVE";
}
