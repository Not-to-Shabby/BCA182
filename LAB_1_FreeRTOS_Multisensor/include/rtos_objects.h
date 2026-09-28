#ifndef RTOS_OBJECTS_H
#define RTOS_OBJECTS_H

#include <stdbool.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "event_groups.h"
#include "semphr.h"

/* ---------- Queues ---------- */

/* SensorTask -> DisplayTask: latest SensorData */
extern QueueHandle_t sensorToDisplayQueue;

/* SensorTask -> AlarmTask: latest SensorData */
extern QueueHandle_t sensorToAlarmQueue;

/* InputTask -> DisplayTask: currently selected DisplayMode */
extern QueueHandle_t displayModeQueue;

/* ---------- Event Group: systemEvents ----------
   EVENT_ACTIVE    set/cleared by StateTask;  read by Display/Input/Alarm tasks
   EVENT_MOTION    set by MotionTask when motion is seen; consumed by StateTask
   EVENT_ALARM     set/cleared by AlarmTask;  read by DisplayTask
   EVENT_PIR_LEVEL set/cleared by MotionTask (live PIR level); read by SensorTask */
#define EVENT_ACTIVE     ((EventBits_t)(1UL << 0))
#define EVENT_MOTION     ((EventBits_t)(1UL << 1))
#define EVENT_ALARM      ((EventBits_t)(1UL << 2))
#define EVENT_PIR_LEVEL  ((EventBits_t)(1UL << 3))

extern EventGroupHandle_t systemEvents;

/* ---------- Mutex ----------
   serialMutex: protects USART1 output across all concurrent tasks */
extern SemaphoreHandle_t serialMutex;

bool RTOS_Objects_Create(void);

#endif /* RTOS_OBJECTS_H */
