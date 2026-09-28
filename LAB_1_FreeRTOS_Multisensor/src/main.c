#include "stm32f1xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "event_groups.h"
#include "ssd1306.h"
#include "sensors.h"
#include "rtos_objects.h"
#include "log.h"
#include "logic.h"
#include <stdio.h>
#include <string.h>

#define TASK_STACK_WORDS 256

/* PC13 LED on Blue Pill */
static void StatusLed_Init(void) {
    __HAL_RCC_GPIOC_CLK_ENABLE();
    GPIO_InitTypeDef g = {0};
    g.Pin   = GPIO_PIN_13;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &g);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET); // ON at boot
}

/* DisplayTask: exclusively owns the SSD1306 OLED display */
static void DisplayTask(void *argument) {
    (void)argument;
    SensorData data = {0};
    char buf[32];
    uint32_t frameCount = 0;

    if (SSD1306_Init()) {
        Log("[Display] SSD1306 Hardware I2C initialized\r\n");
    } else {
        Log("[Display] ERROR: SSD1306 init failed\r\n");
    }

    for (;;) {
        frameCount++;
        /* Check for new sensor telemetry from queue (100 ms timeout) */
        (void)xQueueReceive(sensorToDisplayQueue, &data, pdMS_TO_TICKS(100));

        // Toggle PC13 LED as activity heartbeat
        HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);

        SSD1306_Clear();
        SSD1306_DrawRect(0, 0, SSD1306_WIDTH, SSD1306_HEIGHT, 1);
        SSD1306_FillRect(2, 2, SSD1306_WIDTH - 4, 11, 1);
        SSD1306_DrawString(6, 4, "BCA182 ROOM MONITOR", 0);

        snprintf(buf, sizeof(buf), "TEMP: %.1f C", data.temperature);
        SSD1306_DrawString(10, 18, buf, 1);

        snprintf(buf, sizeof(buf), "HUM : %.1f %%", data.humidity);
        SSD1306_DrawString(10, 30, buf, 1);

        snprintf(buf, sizeof(buf), "LIGHT: %d %%", data.lightLevel);
        SSD1306_DrawString(10, 42, buf, 1);

        snprintf(buf, sizeof(buf), "FRAME: %lu", frameCount);
        SSD1306_DrawString(10, 54, buf, 1);

        SSD1306_Update();
    }
}

int main(void) {
    SCB->VTOR = FLASH_BASE;

    HAL_Init();
    StatusLed_Init();
    Log_Init();
    Sensors_Init();

    Log("\r\n========================================\r\n");
    Log(" BCA182 Laboratory 1: Phase 3 Sensors   \r\n");
    Log(" Real-Time Multisensor Room Monitoring  \r\n");
    Log(" DHT22 (PA1) | LDR (PA0) | Queue IPC    \r\n");
    Log("========================================\r\n");

    if (!RTOS_Objects_Create()) {
        Log("[RTOS] ERROR: Object creation failed!\r\n");
        while (1) {}
    }

    Log("[RTOS] Creating SensorTask (Priority 2)...\r\n");
    xTaskCreate(SensorTask, "SensorTask", TASK_STACK_WORDS, NULL, 2, NULL);

    Log("[RTOS] Creating DisplayTask (Priority 1)...\r\n");
    xTaskCreate(DisplayTask, "DisplayTask", TASK_STACK_WORDS, NULL, 1, NULL);

    Log("[RTOS] Launching FreeRTOS Scheduler...\r\n");
    vTaskStartScheduler();

    Log("[RTOS] ERROR: Scheduler returned!\r\n");
    while (1) {}
}

/* -------------------------------------------------------------
 * RTOS Hooks and Handlers
 * ------------------------------------------------------------- */
extern BaseType_t xPortConsumeTickYield(void);

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
