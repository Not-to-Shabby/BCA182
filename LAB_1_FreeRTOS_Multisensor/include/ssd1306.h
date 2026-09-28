#ifndef SSD1306_H
#define SSD1306_H

#include <stdint.h>
#include <stdbool.h>
#include "stm32f1xx_hal.h"

#define SSD1306_WIDTH       128
#define SSD1306_HEIGHT      64
#define SSD1306_PAGES       (SSD1306_HEIGHT / 8) // 8 pages
#define SSD1306_ADDR        (0x3C << 1)          // 0x78

#ifdef __cplusplus
extern "C" {
#endif

extern I2C_HandleTypeDef hi2c1;

// Driver Lifecycle
bool SSD1306_Init(void);
void SSD1306_Clear(void);
bool SSD1306_Update(void);
void SSD1306_SetPower(bool on);

// Drawing Primitives
void SSD1306_DrawPixel(int16_t x, int16_t y, uint8_t color);
void SSD1306_DrawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color);
void SSD1306_DrawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color);
void SSD1306_FillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color);
void SSD1306_DrawChar(int16_t x, int16_t y, char c, uint8_t color);
void SSD1306_DrawString(int16_t x, int16_t y, const char *str, uint8_t color);

#ifdef __cplusplus
}
#endif

#endif // SSD1306_H
