#include "stm32f1xx_hal.h"
#include "ssd1306.h"
#include <stdio.h>
#include <string.h>

UART_HandleTypeDef huart1;

void SystemClock_Config(void) {
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
    HAL_RCC_OscConfig(&RCC_OscInitStruct);

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0);
}

static void MX_GPIO_Init(void) {
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    // PC13 LED (Active LOW on Blue Pill)
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = GPIO_PIN_13;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET); // Turn ON LED initially
}

static void MX_USART1_UART_Init(void) {
    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    // PA9 = USART1_TX (Alternate Function Push-Pull)
    GPIO_InitStruct.Pin = GPIO_PIN_9;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    // PA10 = USART1_RX (Input Floating)
    GPIO_InitStruct.Pin = GPIO_PIN_10;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

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

static void uart_print(const char *msg) {
    HAL_UART_Transmit(&huart1, (uint8_t *)msg, strlen(msg), 1000);
}

int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART1_UART_Init();

    uart_print("\r\n========================================\r\n");
    uart_print(" BCA182 Laboratory 1: OLED Diagnostic  \r\n");
    uart_print(" Target: STM32 Blue Pill (STM32F103C8T6)\r\n");
    uart_print(" Simulation: Wokwi in VS Code Insiders \r\n");
    uart_print("========================================\r\n");

    uart_print("[OLED] Probing I2C address 0x3C (0x78)...\r\n");
    ssd1306_i2c_init();

    uint8_t probe = ssd1306_probe();
    if (probe == 0) {
        uart_print("[OLED] SUCCESS: Device ACK received at 0x3C!\r\n");
    } else {
        uart_print("[OLED] WARNING: Device NACK at 0x3C. Initializing anyway...\r\n");
    }

    uart_print("[OLED] Initializing SSD1306 controller...\r\n");
    uint8_t init_res = ssd1306_init();
    if (init_res == 0) {
        uart_print("[OLED] SSD1306 initialization completed successfully!\r\n");
    } else {
        uart_print("[OLED] SSD1306 initialization reported bus error!\r\n");
    }

    uint32_t frame_count = 0;
    char buffer[80];
    int16_t bar_pos = 4;
    int16_t bar_dir = 2;

    while (1) {
        frame_count++;

        // Blink PC13 LED (heartbeat)
        HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);

        // Draw Frame to SSD1306
        ssd1306_clear();

        // Screen border
        ssd1306_draw_rect(0, 0, SSD1306_WIDTH, SSD1306_HEIGHT, 1);

        // Header
        ssd1306_fill_rect(2, 2, SSD1306_WIDTH - 4, 11, 1);
        ssd1306_draw_string(6, 4, "MSU-IIT CCS - BCA182", 0); // Inverted text

        // Title and Status
        ssd1306_draw_string(14, 18, "OLED DISPLAY TEST", 1);
        ssd1306_draw_string(14, 30, "STATUS: WORKING!", 1);

        // Uptime counter
        snprintf(buffer, sizeof(buffer), "FRAME: %lu", frame_count);
        ssd1306_draw_string(14, 42, buffer, 1);

        // Animated bouncing bar at the bottom
        ssd1306_draw_rect(bar_pos, 54, 20, 5, 1);
        bar_pos += bar_dir;
        if (bar_pos > (SSD1306_WIDTH - 26) || bar_pos < 4) {
            bar_dir = -bar_dir;
        }

        // Push buffer to OLED
        ssd1306_update_screen();

        // Print status to USART1
        snprintf(buffer, sizeof(buffer), "[FRAME %lu] Uptime: %lu ms | LED Toggled\r\n", 
                 frame_count, HAL_GetTick());
        uart_print(buffer);

        HAL_Delay(500);
    }
}

void SysTick_Handler(void) {
    HAL_IncTick();
}
