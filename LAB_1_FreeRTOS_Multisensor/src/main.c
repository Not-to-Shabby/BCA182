#include "stm32f1xx_hal.h"
#include "ssd1306.h"
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
        // Fallback to HSI if HSE is not running in emulator
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
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2; // APB1 = 36 MHz (I2C1 clock)
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1; // APB2 = 72 MHz
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK) {
        // Fallback clock config
        RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
        RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
        RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
        RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
        HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0);
    }
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

static void uart_print(const char *msg) {
    HAL_UART_Transmit(&huart1, (uint8_t *)msg, strlen(msg), 1000);
}

int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART1_UART_Init();

    uart_print("\r\n========================================\r\n");
    uart_print(" BCA182 Laboratory 1: Hardware I2C Test \r\n");
    uart_print(" Target: STM32 Blue Pill (STM32F103C8T6)\r\n");
    uart_print(" Framework: STM32Cube HAL (HAL_I2C1)   \r\n");
    uart_print("========================================\r\n");

    uart_print("[I2C1] Initializing Hardware I2C1 (PB6/PB7) & SSD1306...\r\n");
    bool init_ok = SSD1306_Init();
    if (init_ok) {
        uart_print("[I2C1] SUCCESS: SSD1306 Hardware I2C initialized!\r\n");
    } else {
        uart_print("[I2C1] ERROR: SSD1306 initialization failed or NACK!\r\n");
    }

    uint32_t frame_count = 0;
    char buffer[80];
    int16_t bar_pos = 4;
    int16_t bar_dir = 2;

    while (1) {
        frame_count++;

        // Blink PC13 LED
        HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);

        // Draw Frame using hardware I2C buffer
        SSD1306_Clear();

        // Screen border
        SSD1306_DrawRect(0, 0, SSD1306_WIDTH, SSD1306_HEIGHT, 1);

        // Header
        SSD1306_FillRect(2, 2, SSD1306_WIDTH - 4, 11, 1);
        SSD1306_DrawString(6, 4, "MSU-IIT CCS - BCA182", 0); // Inverted

        // Body
        SSD1306_DrawString(14, 18, "HARDWARE I2C1 TEST", 1);
        SSD1306_DrawString(14, 30, init_ok ? "STATUS: HARDWARE OK" : "STATUS: RETRYING", 1);

        // Frame / Uptime
        snprintf(buffer, sizeof(buffer), "FRAME: %lu", frame_count);
        SSD1306_DrawString(14, 42, buffer, 1);

        // Animated bouncing bar
        SSD1306_DrawRect(bar_pos, 54, 20, 5, 1);
        bar_pos += bar_dir;
        if (bar_pos > (SSD1306_WIDTH - 26) || bar_pos < 4) {
            bar_dir = -bar_dir;
        }

        // Push frame over hardware I2C
        bool update_ok = SSD1306_Update();

        // Output to serial
        snprintf(buffer, sizeof(buffer), "[FRAME %lu] Update: %s | Uptime: %lu ms\r\n",
                 frame_count, update_ok ? "OK" : "FAIL", HAL_GetTick());
        uart_print(buffer);

        HAL_Delay(500);
    }
}

void SysTick_Handler(void) {
    HAL_IncTick();
}
