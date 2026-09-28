#include "sensors.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "event_groups.h"
#include "dht22.h"
#include "ldr.h"
#include "rtos_objects.h"
#include "log.h"

#define SENSOR_PERIOD_MS  2000U

void Sensors_Init(void)
{
    DHT22_Init();
    LDR_Init();
}

/* SensorTask samples the DHT22 and LDR every 2 seconds.
   vTaskDelayUntil() guarantees drift-free periodic execution per lab specification. */
void SensorTask(void *argument)
{
    (void)argument;
    SensorData data = {0};
    Dht22Reading r = {0};
    uint16_t raw;
    TickType_t lastWakeTime = xTaskGetTickCount();

    for (;;)
    {
        if (DHT22_Read(&r))
        {
            data.temperature = (float)r.temp_x10 / 10.0f;
            data.humidity    = (float)r.hum_x10 / 10.0f;
            data.dhtValid    = true;
        }
        else
        {
            data.dhtValid = false;
            Log("[Sensor] DHT22 sample failed\r\n");
        }

        if (LDR_ReadRaw(&raw))
        {
            data.lightLevel = LDR_RawToPercent(raw);
        }

        /* Motion state from event group (set by MotionTask once implemented) */
        if (systemEvents != NULL)
        {
            data.motionDetected = (xEventGroupGetBits(systemEvents) & EVENT_PIR_LEVEL) != 0;
        }

        /* Stream diagnostic telemetry via thread-safe logger */
        Log_Begin();
        Log("[Sensor] Temp: ");
        if (data.dhtValid) { Log_Tenths(r.temp_x10); } else { Log("--"); }
        Log(" C | Hum: ");
        if (data.dhtValid) { Log_Tenths((int32_t)r.hum_x10); } else { Log("--"); }
        Log(" % | Light: ");
        Log_Uint((uint32_t)data.lightLevel);
        Log(" %\r\n");
        Log_End();

        /* Length-1 overwrite queues distribute data to Display and Alarm consumers */
        if (sensorToDisplayQueue != NULL)
        {
            xQueueOverwrite(sensorToDisplayQueue, &data);
        }
        if (sensorToAlarmQueue != NULL)
        {
            xQueueOverwrite(sensorToAlarmQueue, &data);
        }

        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(SENSOR_PERIOD_MS));
    }
}
