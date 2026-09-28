#include <stdbool.h>
#include "alarm.h"
#include "buzzer.h"
#include "logic.h"
#include "sensors.h"
#include "rtos_objects.h"
#include "log.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "event_groups.h"

#define ALARM_WAIT_MS  250U

void AlarmTask(void *argument)
{
    (void)argument;
    SensorData d = {0};
    AlarmState state = ALARM_NORMAL;

    if (!Buzzer_Init())
    {
        Log("[Alarm] Buzzer initialization failed\r\n");
    }

    for (;;)
    {
        /* Wait for updated sensor data */
        if (sensorToAlarmQueue != NULL &&
            xQueueReceive(sensorToAlarmQueue, &d, pdMS_TO_TICKS(ALARM_WAIT_MS)) == pdPASS)
        {
            if (d.dhtValid)
            {
                AlarmState newState = evaluateTemperature(d.temperature);

                if (newState != state)
                {
                    state = newState;

                    Log_Begin();
                    Log("[Alarm] State changed: ");
                    Log(alarmStateName(state));
                    Log("\r\n");
                    Log_End();

                    if (state != ALARM_NORMAL)
                    {
                        (void)xEventGroupSetBits(systemEvents, EVENT_ALARM);
                    }
                    else
                    {
                        (void)xEventGroupClearBits(systemEvents, EVENT_ALARM);
                    }
                }
            }
        }

        /* Audible alarm is enabled only while the system is in ACTIVE state */
        bool active = (systemEvents != NULL) &&
                      ((xEventGroupGetBits(systemEvents) & EVENT_ACTIVE) != 0);
        Buzzer_Set(active && (state != ALARM_NORMAL));
    }
}
