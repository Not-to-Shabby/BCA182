#ifndef SENSORS_H
#define SENSORS_H

#include <stdbool.h>

/* Sensor telemetry data structure transmitted via FreeRTOS Queues */
typedef struct
{
    float temperature;      /* Degrees Celsius */
    float humidity;         /* % Relative humidity */
    int   lightLevel;       /* Relative light level, 0..100 % */
    bool  motionDetected;   /* Current PIR motion detection state */
    bool  dhtValid;         /* True if latest DHT22 read succeeded */
} SensorData;

void Sensors_Init(void);
void SensorTask(void *argument);

#endif /* SENSORS_H */
