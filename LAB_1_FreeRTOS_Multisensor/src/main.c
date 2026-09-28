#include "stm32f1xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "ssd1306.h"
#include "diagnostics.h"
#include <stdio.h>
#include <string.h>

UART_HandleTypeDef huart1;

void SystemClock_Config(void) {
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    // Standard Blue Pill 8 MHz crystal oscillator -> 72 MHz PLL clock tree
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9; // 8 MHz * 9 = 72 MHz
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
        RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
        RCC_OscInitStruct.HSIState = RCC_HSI_ON;
        RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
        RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
        HAL_RCC_OscConfig(&RCC_OscInitStruct);
    }

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2; // APB1 = 36 MHz
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1; // APB2 = 72 MHz
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK) {
        RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
        RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
        RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
        RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
        HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0);
    }
}

static void MX_GPIO_Init(void) {
    __HAL_RCC_GPIOC_CLK_ENABLE();

    // PC13 LED (Active LOW on Blue Pill)
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = GPIO_PIN_13;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);
}

static void MX_USART1_UART_Init(void) {
    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart1);
}

/* -------------------------------------------------------------
 * Phase 2: Baseline FreeRTOS Tasks (Task A & Task B)
 * ------------------------------------------------------------- */
static void TaskA(void *pvParameters) {
    (void)pvParameters;
    char buffer[64];
    uint32_t count = 0;

    diag_puts("[Task A] Starting task loop...\r\n");

    for (;;) {
        count++;
        snprintf(buffer, sizeof(buffer), "[Task A] Running | Iteration: %lu | Tick: %lu\r\n",
                 count, (unsigned long)xTaskGetTickCount());
        diag_puts(buffer);

        // Toggle on-board LED
        HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);

        // Block for 1000 ms
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void TaskB(void *pvParameters) {
    (void)pvParameters;
    char buffer[64];
    uint32_t count = 0;

    diag_puts("[Task B] Waiting initial 500 ms...\r\n");
    // Offset so Task B alternates cleanly with Task A
    vTaskDelay(pdMS_TO_TICKS(500));

    for (;;) {
        count++;
        snprintf(buffer, sizeof(buffer), "[Task B] Running | Iteration: %lu | Tick: %lu\r\n",
                 count, (unsigned long)xTaskGetTickCount());
        diag_puts(buffer);

        // Update SSD1306 Display with live task status
        SSD1306_Clear();
        SSD1306_DrawRect(0, 0, SSD1306_WIDTH, SSD1306_HEIGHT, 1);
        SSD1306_FillRect(2, 2, SSD1306_WIDTH - 4, 11, 1);
        SSD1306_DrawString(6, 4, "BCA182 FREERTOS INIT", 0);

        SSD1306_DrawString(10, 18, "PHASE 2: MULTI-TASK", 1);
        snprintf(buffer, sizeof(buffer), "TASK A: RUNNING");
        SSD1306_DrawString(10, 30, buffer, 1);
        snprintf(buffer, sizeof(buffer), "TASK B: COUNT %lu", count);
        SSD1306_DrawString(10, 42, buffer, 1);

        SSD1306_Update();

        // Block for 1000 ms
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

int main(void) {
    // 1. STM32 HAL Hardware Init
    HAL_Init();
    SystemClock_Config();

    // 2. Set NVIC Priority Grouping to Group 4 (all 4 bits for preemption)
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);

    // 3. Ensure Vector Table Base points to Flash for Cortex-M3 SVC-0 context restore
    SCB->VTOR = FLASH_BASE;

    // 4. Initialize Peripherals
    MX_GPIO_Init();
    MX_USART1_UART_Init();

    // 5. Initialize Diagnostic and Fault Handlers
    diag_early_init();

    diag_puts("\r\n========================================\r\n");
    diag_puts(" BCA182 Laboratory 1: Phase 2 Baseline  \r\n");
    diag_puts(" Real-Time Multisensor Room Monitoring  \r\n");
    diag_puts(" FreeRTOS v10.3.1 Cortex-M3 Scheduler   \r\n");
    diag_puts("========================================\r\n");

    // Initialize OLED hardware display
    diag_puts("[Display] Initializing SSD1306 OLED over Hardware I2C1...\r\n");
    if (SSD1306_Init()) {
        diag_puts("[Display] SSD1306 Hardware I2C initialized successfully!\r\n");
        SSD1306_Clear();
        SSD1306_DrawString(14, 26, "STARTING RTOS...", 1);
        SSD1306_Update();
    } else {
        diag_puts("[Display] SSD1306 initialization failed or NACK.\r\n");
    }

    // 6. Create Baseline RTOS Tasks
    diag_puts("[RTOS] Creating Task A (Priority 2)...\r\n");
    BaseType_t resA = xTaskCreate(TaskA, "TaskA", 256, NULL, 2, NULL);
    if (resA != pdPASS) {
        diag_puts("[RTOS] ERROR: Task A creation failed!\r\n");
    }

    diag_puts("[RTOS] Creating Task B (Priority 1)...\r\n");
    BaseType_t resB = xTaskCreate(TaskB, "TaskB", 256, NULL, 1, NULL);
    if (resB != pdPASS) {
        diag_puts("[RTOS] ERROR: Task B creation failed!\r\n");
    }

    // 7. Start the FreeRTOS Scheduler
    diag_puts("[RTOS] Starting FreeRTOS Scheduler...\r\n");
    vTaskStartScheduler();

    // Should never reach here unless heap is exhausted
    diag_puts("[RTOS] ERROR: Scheduler returned! Insufficient heap.\r\n");
    while (1) {
    }
}

/* -------------------------------------------------------------
 * SysTick and RTOS Exception Handlers
 * ------------------------------------------------------------- */
extern void xPortSysTickHandler(void);

void SysTick_Handler(void) {
    HAL_IncTick();
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xPortSysTickHandler();
    }
}

void vApplicationMallocFailedHook(void) {
    diag_puts("\r\n[FATAL] FreeRTOS Malloc Failed!\r\n");
    while (1);
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    (void)xTask;
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "\r\n[FATAL] Stack Overflow in task: %s\r\n", pcTaskName);
    diag_puts(buffer);
    while (1);
}

extern BaseType_t xPortConsumeTickYield(void);

void vApplicationIdleHook(void) {
    __WFI();
    if (xPortConsumeTickYield() != pdFALSE) {
        taskYIELD();
    }
}

void vAssertCalled(const char *file, int line) {
    char buffer[80];
    snprintf(buffer, sizeof(buffer), "\r\n[ASSERT] %s:%d\r\n", file, line);
    diag_puts(buffer);
    while (1);
}
