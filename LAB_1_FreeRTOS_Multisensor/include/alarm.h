#ifndef ALARM_H
#define ALARM_H

/* AlarmTask: monitors sensor data against 18.0 C .. 30.0 C thresholds
   and drives the buzzer when active */
void AlarmTask(void *argument);

#endif /* ALARM_H */
