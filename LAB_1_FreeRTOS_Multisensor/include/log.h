#ifndef LOG_H
#define LOG_H

#include <stdint.h>

void Log_Init(void);

/* Mutex-protected thread-safe serial output */
void Log(const char *msg);
void Log_Tenths(int32_t v);     /* Prints 254 as "25.4" */
void Log_Uint(uint32_t v);
void Log_Begin(void);
void Log_End(void);

/* Direct register-level fallback (works in asserts/faults) */
void Log_RawPuts(const char *s);
void Log_RawUint(uint32_t v);

#endif /* LOG_H */
