/**
 * @file lcd_st7789.h
 * @brief ST7789 v3 240x240 LCD driver interface for RT-Thread Spark Board
 *        interfaced via STM32F407 FSMC 8080 8-bit parallel bus.
 *
 * Course: BCA182 Embedded Systems Programming
 * Laboratory Activity 2: Personal MP3 Player
 */

#ifndef LCD_ST7789_H_
#define LCD_ST7789_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Display Geometry                                                           */
/* -------------------------------------------------------------------------- */
#define LCD_WIDTH               240
#define LCD_HEIGHT              240

/* -------------------------------------------------------------------------- */
/* Standard 16-Bit RGB565 Color Definitions                                   */
/* -------------------------------------------------------------------------- */
#define LCD_COLOR_BLACK         0x0000
#define LCD_COLOR_WHITE         0xFFFF
#define LCD_COLOR_RED           0xF800
#define LCD_COLOR_GREEN         0x07E0
#define LCD_COLOR_BLUE          0x001F
#define LCD_COLOR_YELLOW        0xFFE0
#define LCD_COLOR_CYAN          0x07FF
#define LCD_COLOR_MAGENTA       0xF81F
#define LCD_COLOR_GRAY          0x8410
#define LCD_COLOR_NAVY          0x000F
#define LCD_COLOR_DARKGREEN     0x03E0
#define LCD_COLOR_DARKCYAN      0x03EF
#define LCD_COLOR_MAROON        0x7800
#define LCD_COLOR_PURPLE        0x780F
#define LCD_COLOR_OLIVE         0x7BE0
#define LCD_COLOR_LIGHTGREY     0xC618
#define LCD_COLOR_DARKGREY      0x39E7
#define LCD_COLOR_ORANGE        0xFD20

/* -------------------------------------------------------------------------- */
/* Driver Lifecycle & Power Control                                           */
/* -------------------------------------------------------------------------- */
/**
 * @brief Initialize STM32F407 FSMC Bank 3, GPIOs, reset the ST7789 controller,
 *        transmit the startup sequence, and activate the backlight on PF9.
 */
void lcd_st7789_init(void);

/**
 * @brief Turn LCD backlight ON (Pin PF9).
 */
void lcd_backlight_on(void);

/**
 * @brief Turn LCD backlight OFF (Pin PF9).
 */
void lcd_backlight_off(void);

/* -------------------------------------------------------------------------- */
/* Graphics & Drawing Primitives                                              */
/* -------------------------------------------------------------------------- */
/**
 * @brief Clear the entire 240x240 screen with a single background color.
 */
void lcd_clear(uint16_t color);

/**
 * @brief Set the active drawing / window area on the ST7789 GRAM.
 */
void lcd_address_set(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);

/**
 * @brief Draw a single pixel.
 */
void lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color);

/**
 * @brief Draw a straight line using Bresenham's algorithm.
 */
void lcd_draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);

/**
 * @brief Draw an unfilled rectangle outline.
 */
void lcd_draw_rect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);

/**
 * @brief Fill a rectangular area with solid color.
 */
void lcd_fill_rect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);

/**
 * @brief Render a single 16x8 ASCII character at specified coordinate.
 */
void lcd_show_char(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg_color);

/**
 * @brief Render a string of text starting at (x, y) with 16x8 font.
 */
void lcd_show_string(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg_color);

#ifdef __cplusplus
}
#endif

#endif /* LCD_ST7789_H_ */
