/**
 * ==============================================================================
 * Hardware USART1 Telemetry Driver on PA9 (TX) and PA10 (RX)
 * ==============================================================================
 * Direct hardware register driver for ST-LINK V2.1 Virtual COM Port (115200 baud).
 * Guarantees zero-buffering, instant transmission to host PC terminal.
 */

#ifndef UART_TELEMETRY_H
#define UART_TELEMETRY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configures PA9 (TX) and PA10 (RX) in AF7 mode and enables USART1 @ 115200 baud.
 */
void uart1_telemetry_init(void);

/**
 * @brief Sends a single byte over hardware USART1 (blocking until TXE).
 */
void uart1_send_char(char c);

/**
 * @brief Transmits a null-terminated string over USART1 (auto converts \n to \r\n).
 */
void uart1_print(const char *str);

/**
 * @brief Formatted printf directly out of hardware USART1.
 */
void uart1_printf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* UART_TELEMETRY_H */
