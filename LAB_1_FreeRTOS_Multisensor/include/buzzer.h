#ifndef BUZZER_H
#define BUZZER_H

#include <stdbool.h>

/* Passive/active buzzer on PA2 driven by TIM2 Channel 3 PWM */
bool Buzzer_Init(void);
void Buzzer_Set(bool on);

#endif /* BUZZER_H */
