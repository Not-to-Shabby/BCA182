#ifndef SSD1306_H
#define SSD1306_H

#include <stdint.h>
#include <stdbool.h>
#include "stm32f1xx_hal.h"

#define SSD1306_WIDTH       128
#define SSD1306_HEIGHT      64
#define SSD1306_I2C_ADDR    (0x3C << 1) // 7-bit 0x3C shifted left = 0x78

#ifdef __cplusplus
extern "C" {
#endif

extern I2C_HandleTypeDef hi2c1;

// Hardware I2C Initialization & Device Probing
uint8_t ssd1306_i2c_init(void);
uint8_t ssd1306_probe(void); // Returns 0 if ACK, 1 if NACK/Error

// Display Lifecycle
uint8_t ssd1306_init(void);
void ssd1306_clear(void);
void ssd1306_update_screen(void);

// Graphic Primitives
void ssd1306_draw_pixel(int16_t x, int16_t y, uint8_t color);
void ssd1306_draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color);
void ssd1306_draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color);
void ssd1306_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color);
void ssd1306_draw_char(int16_t x, int16_t y, char c, uint8_t color);
void ssd1306_draw_string(int16_t x, int16_t y, const char *str, uint8_t color);

#ifdef __cplusplus
}
#endif

#endif // SSD1306_H
