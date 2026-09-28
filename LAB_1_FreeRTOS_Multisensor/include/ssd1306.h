#ifndef SSD1306_H
#define SSD1306_H

#include <stdint.h>
#include <stdbool.h>
#include "stm32f1xx_hal.h"

#define SSD1306_WIDTH   128
#define SSD1306_HEIGHT  64
#define SSD1306_I2C_ADDR 0x78 // 7-bit 0x3C << 1

#ifdef __cplusplus
extern "C" {
#endif

// Hardware / I2C Bus control
void ssd1306_i2c_init(void);
uint8_t ssd1306_probe(void); // Returns 0 if OLED ACKs, 1 if NACK

// Display driver APIs
uint8_t ssd1306_init(void);
void ssd1306_clear(void);
void ssd1306_update_screen(void);

// Graphic primitives
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
