#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "display.h"
#include "ssd1306.h"
#include "sensors.h"
#include "logic.h"
#include "rtos_objects.h"
#include "log.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "event_groups.h"

#define DISPLAY_WAIT_MS  100U

/* Integer formatting (no float printf required) */
static void FormatTenths(char *buf, size_t size, int32_t v)
{
    uint32_t magnitude = (uint32_t)(v < 0 ? -v : v);
    snprintf(buf, size, "%s%lu.%lu",
             v < 0 ? "-" : "",
             (unsigned long)(magnitude / 10U),
             (unsigned long)(magnitude % 10U));
}

static void FormatUint(char *buf, size_t size, uint32_t v)
{
    snprintf(buf, size, "%lu", (unsigned long)v);
}

static void Render(const SensorData *d, bool haveData, DisplayMode mode, bool alarm)
{
    char value[24];

    if (!haveData)
    {
        strcpy(value, "READING...");
    }
    else
    {
        switch (mode)
        {
            case DISPLAY_TEMPERATURE:
                if (d->dhtValid)
                {
                    FormatTenths(value, sizeof(value), (int32_t)(d->temperature * 10.0f + (d->temperature >= 0 ? 0.5f : -0.5f)));
                    strcat(value, " C");
                }
                else
                {
                    strcpy(value, "--.- C");
                }
                break;

            case DISPLAY_HUMIDITY:
                if (d->dhtValid)
                {
                    FormatTenths(value, sizeof(value), (int32_t)(d->humidity * 10.0f + 0.5f));
                    strcat(value, " %");
                }
                else
                {
                    strcpy(value, "--.- %");
                }
                break;

            case DISPLAY_LIGHT:
                FormatUint(value, sizeof(value), (uint32_t)d->lightLevel);
                strcat(value, " %");
                break;

            case DISPLAY_MOTION:
            default:
                strcpy(value, d->motionDetected ? "DETECTED" : "CLEAR");
                break;
        }
    }

    SSD1306_Clear();
    SSD1306_DrawRect(0, 0, SSD1306_WIDTH, SSD1306_HEIGHT, 1);
    SSD1306_FillRect(2, 2, SSD1306_WIDTH - 4, 11, 1);
    SSD1306_DrawString(6, 4, "BCA182 ROOM MONITOR", 0);

    SSD1306_DrawString(10, 18, "SELECTED VIEW:", 1);
    SSD1306_DrawString(10, 30, displayModeName(mode), 1);
    SSD1306_DrawString(10, 42, value, 1);

    if (alarm)
    {
        SSD1306_DrawString(10, 54, "! TEMP ALARM !", 1);
    }

    (void)SSD1306_Update();
}

void DisplayTask(void *argument)
{
    (void)argument;
    SensorData data = {0};
    bool haveData = false;
    DisplayMode mode = DISPLAY_TEMPERATURE;
    bool lastAlarm = false;

    if (SSD1306_Init())
    {
        Log("[Display] SSD1306 Hardware I2C initialized successfully\r\n");
    }
    else
    {
        Log("[Display] ERROR: SSD1306 initialization failed\r\n");
    }

    Render(&data, haveData, mode, lastAlarm);

    for (;;)
    {
        EventBits_t bits = (systemEvents != NULL) ? xEventGroupGetBits(systemEvents) : EVENT_ACTIVE;

        /* If system is INACTIVE, power down OLED and sleep until ACTIVE */
        if ((bits & EVENT_ACTIVE) == 0)
        {
            SSD1306_SetPower(false);
            Log("[OLED] Panel standby - INACTIVE mode\r\n");

            /* Block indefinitely until motion reactivates the system */
            (void)xEventGroupWaitBits(systemEvents, EVENT_ACTIVE, pdFALSE, pdTRUE, portMAX_DELAY);

            SSD1306_SetPower(true);
            Log("[OLED] Panel active - ACTIVE mode restored\r\n");

            /* Refresh latest readings upon wake */
            (void)xQueueReceive(sensorToDisplayQueue, &data, 0);
            DisplayMode m;
            if (xQueueReceive(displayModeQueue, &m, 0) == pdPASS) { mode = m; }
            lastAlarm = (xEventGroupGetBits(systemEvents) & EVENT_ALARM) != 0;
            Render(&data, haveData, mode, lastAlarm);
            continue;
        }

        bool changed = false;
        DisplayMode newMode;

        if (sensorToDisplayQueue != NULL &&
            xQueueReceive(sensorToDisplayQueue, &data, pdMS_TO_TICKS(DISPLAY_WAIT_MS)) == pdPASS)
        {
            haveData = true;
            changed = true;
        }

        if (displayModeQueue != NULL &&
            xQueueReceive(displayModeQueue, &newMode, 0) == pdPASS && newMode != mode)
        {
            mode = newMode;
            changed = true;
        }

        bool alarm = (systemEvents != NULL) &&
                     ((xEventGroupGetBits(systemEvents) & EVENT_ALARM) != 0);
        if (alarm != lastAlarm)
        {
            lastAlarm = alarm;
            changed = true;
        }

        if (changed)
        {
            Render(&data, haveData, mode, lastAlarm);
        }
    }
}
