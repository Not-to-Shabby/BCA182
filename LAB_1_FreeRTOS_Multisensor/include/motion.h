#ifndef MOTION_H
#define MOTION_H

/* PIR motion sensor input on PA3 */
void Motion_Init(void);
void MotionTask(void *argument);

#endif /* MOTION_H */
