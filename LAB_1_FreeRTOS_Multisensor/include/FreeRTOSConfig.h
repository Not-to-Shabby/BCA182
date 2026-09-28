#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

/* Ensure definitions are only used by the compiler, not the assembler */
#if defined(__ICCARM__) || defined(__CC_ARM) || defined(__GNUC__)
    #include "stm32f1xx.h"
#endif

#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configSUPPORT_STATIC_ALLOCATION          0
#define configSUPPORT_DYNAMIC_ALLOCATION         1
#define configUSE_IDLE_HOOK                      1
#define configUSE_TICK_HOOK                      0
#define configCPU_CLOCK_HZ                       (SystemCoreClock)
#define configTICK_RATE_HZ                       ((TickType_t)20)
#define configMAX_PRIORITIES                     (5)
#define configMINIMAL_STACK_SIZE                 ((uint16_t)128)
#define configTOTAL_HEAP_SIZE                    ((size_t)(10 * 1024)) // 10 KB of 20 KB SRAM
#define configMAX_TASK_NAME_LEN                  (16)
#define configUSE_TRACE_FACILITY                 1
#define configUSE_16_BIT_TICKS                   0
#define configIDLE_SHOULD_YIELD                  1
#define configUSE_MUTEXES                        1
#define configQUEUE_REGISTRY_SIZE                8
#define configCHECK_FOR_STACK_OVERFLOW           2
#define configUSE_RECURSIVE_MUTEXES              1
#define configUSE_MALLOC_FAILED_HOOK             1
#define configUSE_APPLICATION_TASK_TAG           0
#define configUSE_COUNTING_SEMAPHORES            1
#define configGENERATE_RUN_TIME_STATS            0

/* Co-routine definitions */
#define configUSE_CO_ROUTINES                    0
#define configMAX_CO_ROUTINE_PRIORITIES          (2)

/* Software timer definitions */
#define configUSE_TIMERS                         1
#define configTIMER_TASK_PRIORITY                (2)
#define configTIMER_QUEUE_LENGTH                 10
#define configTIMER_TASK_STACK_DEPTH             (configMINIMAL_STACK_SIZE * 2)

/* Set the following definitions to 1 to include the API function, or zero to exclude */
#define INCLUDE_vTaskPrioritySet                 1
#define INCLUDE_uxTaskPriorityGet                1
#define INCLUDE_vTaskDelete                      1
#define INCLUDE_vTaskCleanUpResources            0
#define INCLUDE_vTaskSuspend                     1
#define INCLUDE_vTaskDelayUntil                  1
#define INCLUDE_vTaskDelay                       1
#define INCLUDE_xTaskGetSchedulerState           1
#define INCLUDE_xTaskGetCurrentTaskHandle        1
#define INCLUDE_uxTaskGetStackHighWaterMark      1
#define INCLUDE_xTaskGetIdleTaskHandle           1
#define INCLUDE_eTaskGetState                    1

/* Cortex-M specific definitions */
#ifdef __NVIC_PRIO_BITS
    #define configPRIO_BITS                      __NVIC_PRIO_BITS
#else
    #define configPRIO_BITS                      4
#endif

/* The lowest interrupt priority that can be used in a call to "set priority" */
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY         15

/* The highest interrupt priority that can be used by any interrupt service
routine that makes calls to interrupt safe FreeRTOS API functions. */
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    5

/* Interrupt priorities used by the kernel port layer itself */
#define configKERNEL_INTERRUPT_PRIORITY         (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* Normal assert handling */
void vAssertCalled(const char *file, int line);
#define configASSERT(x) if ((x) == 0) { vAssertCalled(__FILE__, __LINE__); }

/* Map FreeRTOS port interrupt handlers to standard CMSIS vector names */
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
/* Note: SysTick_Handler is implemented in main.c calling xPortSysTickHandler + HAL_IncTick */

#endif /* FREERTOS_CONFIG_H */
