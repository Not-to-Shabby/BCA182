/**
 * ==============================================================================
 * Hardware USART1 Telemetry Driver Implementation
 * ==============================================================================
 */

#include "uart_telemetry.h"
#include <stdio.h>
#include <stdarg.h>

#if defined(__arm__) || defined(STM32F407xx) || defined(CONFIG_SOC_SERIES_STM32F4X) || defined(ZEPHYR_VERSION_CODE)
#include <stm32f4xx.h>

void uart1_telemetry_init(void)
{
    /* 1. Enable Clocks for GPIOA and USART1 on APB2 */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    /* 2. Configure PA9 (TX) and PA10 (RX) in AF Mode (10b) */
    GPIOA->MODER = (GPIOA->MODER & ~((3U << (9 * 2)) | (3U << (10 * 2)))) |
                   ((2U << (9 * 2)) | (2U << (10 * 2)));

    /* Push-Pull Output on TX */
    GPIOA->OTYPER &= ~(1U << 9);

    /* Very High Speed (11b) */
    GPIOA->OSPEEDR |= (3U << (9 * 2)) | (3U << (10 * 2));

    /* Pull-Up enabled (01b) */
    GPIOA->PUPDR = (GPIOA->PUPDR & ~((3U << (9 * 2)) | (3U << (10 * 2)))) |
                   ((1U << (9 * 2)) | (1U << (10 * 2)));

    /* Alternate Function AF7 (USART1) on AFR[1] (pins 8-15) */
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~((0xFU << ((9 - 8) * 4)) | (0xFU << ((10 - 8) * 4)))) |
                    ((7U << ((9 - 8) * 4)) | (7U << ((10 - 8) * 4)));

    /* 3. Configure Baud Rate: 84 MHz / 115200 = 45.5729 -> Mantissa 45 (0x2D), Fraction 9 (0x9) -> 0x2D9 */
    USART1->BRR = 0x2D9;

    /* 4. Enable Transmitter, Receiver, and USART Peripheral */
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

void uart1_send_char(char c)
{
    while (!(USART1->SR & USART_SR_TXE));
    USART1->DR = (uint8_t)c;
}

void uart1_print(const char *str)
{
    if (!str) return;
    while (*str) {
        if (*str == '\n') {
            uart1_send_char('\r');
        }
        uart1_send_char(*str++);
    }
}

void uart1_printf(const char *fmt, ...)
{
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    uart1_print(buffer);
}

#else
/* Host native simulation stub */
void uart1_telemetry_init(void) {}
void uart1_send_char(char c) { putchar(c); }
void uart1_print(const char *str) { if (str) fputs(str, stdout); }
void uart1_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}
#endif
