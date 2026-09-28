#include "stm32f1xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "rtos_objects.h"
#include "sensors.h"
#include "display.h"
#include "input.h"
#include "alarm.h"
#include "motion.h"
#include "system_state.h"
#include "log.h"
#include "diagnostics.h"

#define TASK_STACK_WORDS 256

/* Task priority assignments per lab specification:
   Urgent responsiveness tasks -> Priority 3
   Periodic acquisition and evaluation -> Priority 2
   Display update -> Priority 1                                */
#define MOTION_TASK_PRIORITY   3   /* Immediate PIR event response */
#define STATE_TASK_PRIORITY    3   /* Fast sleep/wake state transitions */
#define INPUT_TASK_PRIORITY    3   /* Responsive encoder rotation */
#define SENSOR_TASK_PRIORITY   2   /* Periodic 2000 ms acquisition (vTaskDelayUntil) */
#define ALARM_TASK_PRIORITY    2   /* Temperature threshold evaluation */
#define DISPLAY_TASK_PRIORITY  1   /* UI rendering */

static void StatusLed_Init(void) {
    __HAL_RCC_GPIOC_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Pin   = GPIO_PIN_13;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &g);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET); // ON at boot
}

int main(void) {
    // 1. Vector Table Base offset
    SCB->VTOR = FLASH_BASE;

    // 2. Hardware initialization
    HAL_Init();
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);

    StatusLed_Init();
    Log_Init();
    diag_early_init();

    Log("\r\n========================================\r\n");
    Log(" BCA182 Laboratory 1: Multisensor Node  \r\n");
    Log(" Real-Time Environmental Monitoring     \r\n");
    Log(" FreeRTOS Concurrent Task Architecture  \r\n");
    Log("========================================\r\n");

    // 3. Sensor & input peripheral initialization
    Log("[Init] Initializing hardware peripherals...\r\n");
    Sensors_Init();
    Input_Init();
    Motion_Init();
    Log("[Init] Sensors, encoder, and PIR initialized\r\n");

    // 4. Create FreeRTOS IPC objects
    if (!RTOS_Objects_Create()) {
        Log("[RTOS] ERROR: Object creation failed!\r\n");
        while (1) {}
    }
    Log("[RTOS] Queues, Mutex, and Event Group created\r\n");

    // 5. Create FreeRTOS Tasks
    BaseType_t ok = pdPASS;
    ok &= xTaskCreate(MotionTask,  "MotionTask",  TASK_STACK_WORDS, NULL, MOTION_TASK_PRIORITY,  NULL);
    ok &= xTaskCreate(StateTask,   "StateTask",   TASK_STACK_WORDS, NULL, STATE_TASK_PRIORITY,   NULL);
    ok &= xTaskCreate(InputTask,   "InputTask",   TASK_STACK_WORDS, NULL, INPUT_TASK_PRIORITY,   NULL);
    ok &= xTaskCreate(SensorTask,  "SensorTask",  TASK_STACK_WORDS, NULL, SENSOR_TASK_PRIORITY,  NULL);
    ok &= xTaskCreate(AlarmTask,   "AlarmTask",   TASK_STACK_WORDS, NULL, ALARM_TASK_PRIORITY,   NULL);
    ok &= xTaskCreate(DisplayTask, "DisplayTask", TASK_STACK_WORDS, NULL, DISPLAY_TASK_PRIORITY, NULL);

    if (ok != pdPASS) {
        Log("[RTOS] ERROR: Task creation failed!\r\n");
        while (1) {}
    }

    Log("[RTOS] All 6 tasks created successfully\r\n");
    Log("[RTOS] Launching FreeRTOS Scheduler...\r\n");
    vTaskStartScheduler();

    Log("[RTOS] ERROR: Scheduler returned!\r\n");
    while (1) {}
}

/* -------------------------------------------------------------
 * RTOS Hooks and Handlers
 * ------------------------------------------------------------- */
extern void xPortSysTickHandler(void);
extern BaseType_t xPortConsumeTickYield(void);

void SysTick_Handler(void) {
    HAL_IncTick();
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xPortSysTickHandler();
    }
}

void vApplicationIdleHook(void) {
    __WFI();
    if (xPortConsumeTickYield() != pdFALSE) {
        taskYIELD();
    }
}

void vApplicationMallocFailedHook(void) {
    Log_RawPuts("\r\n[FATAL] FreeRTOS Malloc Failed!\r\n");
    while (1);
}

/* cppcheck-suppress constParameterPointer */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    (void)xTask;
    Log_RawPuts("\r\n[FATAL] Stack Overflow: ");
    Log_RawPuts(pcTaskName);
    Log_RawPuts("\r\n");
    while (1);
}

void vAssertCalled(const char *file, int line) {
    Log_RawPuts("\r\n[ASSERT] ");
    Log_RawPuts(file);
    Log_RawPuts(" line ");
    Log_RawUint((uint32_t)line);
    Log_RawPuts("\r\n");
    while (1);
}
