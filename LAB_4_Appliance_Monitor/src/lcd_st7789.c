/**
 * @file lcd_st7789.c
 * @brief ST7789 v3 240x240 LCD Driver via STM32F407 FSMC 8080 8-bit parallel bus.
 *        Tailored for the RT-Thread Spark Development Board ("星火 1 号").
 *
 * Course: BCA182 Embedded Systems Programming
 * Laboratory Activity 2: Personal MP3 Player
 */

#include "lcd_st7789.h"
#include "lcd_font.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <stm32f4xx.h>

/* -------------------------------------------------------------------------- */
/* Hardware Memory Addresses (FSMC Bank 1 NOR/SRAM 3, A18 = DCX)              */
/* -------------------------------------------------------------------------- */
#define LCD_CMD_ADDR            ((volatile uint8_t *)(0x6803FFFEU))
#define LCD_DATA8_ADDR          ((volatile uint8_t *)(0x68040000U))
#define LCD_DATA16_ADDR         ((volatile uint16_t *)(0x68040000U))

/* -------------------------------------------------------------------------- */
/* Static Low-Level Bus Primitives                                            */
/* -------------------------------------------------------------------------- */
static inline void lcd_write_reg(uint8_t reg)
{
    *LCD_CMD_ADDR = reg;
}

static inline void lcd_write_data8(uint8_t data)
{
    *LCD_DATA8_ADDR = data;
}

static inline void lcd_write_data16(uint16_t data)
{
    /* Swap bytes: ST7789 expects MSB first across the 8-bit FSMC bus */
    uint16_t swapped = (uint16_t)((data >> 8) | ((data & 0xFFU) << 8));
    *LCD_DATA16_ADDR = swapped;
}

/* -------------------------------------------------------------------------- */
/* Low-Level GPIO & FSMC Hardware Initialization                              */
/* -------------------------------------------------------------------------- */
static void lcd_hardware_init(void)
{
    /* 1. Enable AHB1 peripheral clocks for Ports D, E, F, G */
    RCC->AHB1ENR |= (RCC_AHB1ENR_GPIODEN | RCC_AHB1ENR_GPIOEEN |
                     RCC_AHB1ENR_GPIOFEN | RCC_AHB1ENR_GPIOGEN);

    /* 2. Enable AHB3 peripheral clock for FSMC */
    RCC->AHB3ENR |= RCC_AHB3ENR_FSMCEN;

    /* 3. Configure Backlight Pin (PF9) as General Purpose Output (Push-Pull) */
    GPIOF->MODER = (GPIOF->MODER & ~(3U << 18)) | (1U << 18);
    GPIOF->OTYPER &= ~(1U << 9);
    GPIOF->OSPEEDR |= (3U << 18); /* High speed */
    GPIOF->PUPDR &= ~(3U << 18);

    /* 4. Configure Reset Pin (PD3) as General Purpose Output (Push-Pull) */
    GPIOD->MODER = (GPIOD->MODER & ~(3U << 6)) | (1U << 6);
    GPIOD->OTYPER &= ~(1U << 3);
    GPIOD->OSPEEDR |= (3U << 6);  /* High speed */
    GPIOD->PUPDR &= ~(3U << 6);

    /* 5. Configure FSMC Data Lines on Port E: PE7, PE8, PE9, PE10 (D4-D7)
     *    Alternate Function 12 (AF12) */
    for (uint32_t pin = 7; pin <= 10; pin++) {
        GPIOE->MODER = (GPIOE->MODER & ~(3U << (pin * 2))) | (2U << (pin * 2));
        GPIOE->OTYPER &= ~(1U << pin);
        GPIOE->OSPEEDR |= (3U << (pin * 2));
        GPIOE->PUPDR &= ~(3U << (pin * 2));
        if (pin < 8) {
            GPIOE->AFR[0] = (GPIOE->AFR[0] & ~(0xFU << (pin * 4))) | (12U << (pin * 4));
        } else {
            GPIOE->AFR[1] = (GPIOE->AFR[1] & ~(0xFU << ((pin - 8) * 4))) | (12U << ((pin - 8) * 4));
        }
    }

    /* 6. Configure FSMC Lines on Port D:
     *    PD0 (D2), PD1 (D3), PD4 (NOE), PD5 (NWE), PD13 (A18/DCX), PD14 (D0), PD15 (D1)
     *    Alternate Function 12 (AF12) */
    static const uint8_t d_pins[] = {0, 1, 4, 5, 13, 14, 15};
    for (uint32_t i = 0; i < sizeof(d_pins); i++) {
        uint32_t pin = d_pins[i];
        GPIOD->MODER = (GPIOD->MODER & ~(3U << (pin * 2))) | (2U << (pin * 2));
        GPIOD->OTYPER &= ~(1U << pin);
        GPIOD->OSPEEDR |= (3U << (pin * 2));
        GPIOD->PUPDR &= ~(3U << (pin * 2));
        if (pin < 8) {
            GPIOD->AFR[0] = (GPIOD->AFR[0] & ~(0xFU << (pin * 4))) | (12U << (pin * 4));
        } else {
            GPIOD->AFR[1] = (GPIOD->AFR[1] & ~(0xFU << ((pin - 8) * 4))) | (12U << ((pin - 8) * 4));
        }
    }

    /* 7. Configure FSMC Chip Select Line on Port G: PG10 (NE3)
     *    Alternate Function 12 (AF12) */
    GPIOG->MODER = (GPIOG->MODER & ~(3U << (10 * 2))) | (2U << (10 * 2));
    GPIOG->OTYPER &= ~(1U << 10);
    GPIOG->OSPEEDR |= (3U << (10 * 2));
    GPIOG->PUPDR &= ~(3U << (10 * 2));
    GPIOG->AFR[1] = (GPIOG->AFR[1] & ~(0xFU << ((10 - 8) * 4))) | (12U << ((10 - 8) * 4));

    /* 8. Hardware Reset Pulse on PD3 */
    GPIOD->BSRR = (1U << (3 + 16)); /* PD3 LOW */
    k_msleep(20);
    GPIOD->BSRR = (1U << 3);        /* PD3 HIGH */
    k_msleep(120);

    /* 9. Configure FSMC Bank 1 NOR/SRAM 3 (BTCR[4] = BCR3, BTCR[5] = BTR3) */
    /* Disable Bank 3 before modifying control registers */
    FSMC_Bank1->BTCR[4] = 0;

    /* BCR3 configuration:
     * - WREN: Write enable (bit 12)
     * - EXTMOD: Extended mode enable (bit 14)
     * - MWID: 8-bit memory data width (00b at bits 5:4)
     * - MTYP: SRAM memory type (00b at bits 3:2)
     */
    FSMC_Bank1->BTCR[4] = (1U << 12) | (1U << 14);

    /* BTR3 Read Timing configuration (Mode A):
     * - ADDSET: Address setup time = 15 HCLK
     * - DATAST: Data setup time = 60 HCLK
     * - ACCMOD: Access mode A (00b at bits 29:28)
     */
    FSMC_Bank1->BTCR[5] = (15U << 0) | (60U << 8);

    /* BWTR3 Write Timing configuration (Bank1E BWTR[4]):
     * - ADDSET: Address setup time = 3 HCLK
     * - DATAST: Data setup time = 3 HCLK
     * - ACCMOD: Access mode A (00b at bits 29:28)
     */
    FSMC_Bank1E->BWTR[4] = (3U << 0) | (3U << 8);

    /* Enable Bank 3 */
    FSMC_Bank1->BTCR[4] |= (1U << 0); /* MBKEN = 1 */
    k_msleep(10);
}

/* -------------------------------------------------------------------------- */
/* ST7789 Initialization Sequence                                             */
/* -------------------------------------------------------------------------- */
void lcd_st7789_init(void)
{
    printk("[ST7789] Initializing FSMC bus and hardware GPIOs...\n");
    lcd_hardware_init();

    printk("[ST7789] Transmitting controller configuration sequence...\n");

    /* Memory Data Access Control */
    lcd_write_reg(0x36);
    lcd_write_data8(0x00);

    /* RGB 5-6-5 color format (16-bit) */
    lcd_write_reg(0x3A);
    lcd_write_data8(0x55);

    /* Porch Setting */
    lcd_write_reg(0xB2);
    lcd_write_data8(0x0C);
    lcd_write_data8(0x0C);
    lcd_write_data8(0x00);
    lcd_write_data8(0x33);
    lcd_write_data8(0x33);

    /* Gate Control */
    lcd_write_reg(0xB7);
    lcd_write_data8(0x35);

    /* VCOM Setting */
    lcd_write_reg(0xBB);
    lcd_write_data8(0x37);

    /* LCM Control */
    lcd_write_reg(0xC0);
    lcd_write_data8(0x2C);

    /* VDV and VRH Command Enable */
    lcd_write_reg(0xC2);
    lcd_write_data8(0x01);

    /* VRH Set */
    lcd_write_reg(0xC3);
    lcd_write_data8(0x12);

    /* VDV Set */
    lcd_write_reg(0xC4);
    lcd_write_data8(0x20);

    /* Frame Rate Control in Normal Mode (60 Hz) */
    lcd_write_reg(0xC6);
    lcd_write_data8(0x0F);

    /* Power Control 1 */
    lcd_write_reg(0xD0);
    lcd_write_data8(0xA4);
    lcd_write_data8(0xA1);

    /* Positive Voltage Gamma Control */
    lcd_write_reg(0xE0);
    lcd_write_data8(0xD0);
    lcd_write_data8(0x04);
    lcd_write_data8(0x0D);
    lcd_write_data8(0x11);
    lcd_write_data8(0x13);
    lcd_write_data8(0x2B);
    lcd_write_data8(0x3F);
    lcd_write_data8(0x54);
    lcd_write_data8(0x4C);
    lcd_write_data8(0x18);
    lcd_write_data8(0x0D);
    lcd_write_data8(0x0B);
    lcd_write_data8(0x1F);
    lcd_write_data8(0x23);

    /* Negative Voltage Gamma Control */
    lcd_write_reg(0xE1);
    lcd_write_data8(0xD0);
    lcd_write_data8(0x04);
    lcd_write_data8(0x0C);
    lcd_write_data8(0x11);
    lcd_write_data8(0x13);
    lcd_write_data8(0x2C);
    lcd_write_data8(0x3F);
    lcd_write_data8(0x44);
    lcd_write_data8(0x51);
    lcd_write_data8(0x2F);
    lcd_write_data8(0x1F);
    lcd_write_data8(0x1F);
    lcd_write_data8(0x20);
    lcd_write_data8(0x23);

    /* Display Inversion On */
    lcd_write_reg(0x21);

    /* Sleep Out */
    lcd_write_reg(0x11);
    k_msleep(120);

    /* Display ON */
    lcd_write_reg(0x29);
    k_msleep(20);

    /* Clear screen to black background */
    lcd_clear(LCD_COLOR_BLACK);

    /* Turn Backlight ON (PF9) */
    lcd_backlight_on();
    printk("[ST7789] LCD display initialized and backlight ON.\n");
}

void lcd_backlight_on(void)
{
    GPIOF->BSRR = (1U << 9); /* PF9 HIGH */
}

void lcd_backlight_off(void)
{
    GPIOF->BSRR = (1U << (9 + 16)); /* PF9 LOW */
}

/* -------------------------------------------------------------------------- */
/* Graphics & Drawing API                                                     */
/* -------------------------------------------------------------------------- */
void lcd_address_set(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    lcd_write_reg(0x2A); /* Column Address Set */
    lcd_write_data8((uint8_t)(x1 >> 8));
    lcd_write_data8((uint8_t)(x1 & 0xFFU));
    lcd_write_data8((uint8_t)(x2 >> 8));
    lcd_write_data8((uint8_t)(x2 & 0xFFU));

    lcd_write_reg(0x2B); /* Row Address Set */
    lcd_write_data8((uint8_t)(y1 >> 8));
    lcd_write_data8((uint8_t)(y1 & 0xFFU));
    lcd_write_data8((uint8_t)(y2 >> 8));
    lcd_write_data8((uint8_t)(y2 & 0xFFU));

    lcd_write_reg(0x2C); /* Memory Write */
}

void lcd_clear(uint16_t color)
{
    uint32_t total_pixels = LCD_WIDTH * LCD_HEIGHT;
    lcd_address_set(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    for (uint32_t i = 0; i < total_pixels; i++) {
        lcd_write_data16(color);
    }
}

void lcd_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT) {
        return;
    }
    lcd_address_set(x, y, x, y);
    lcd_write_data16(color);
}

void lcd_fill_rect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    if (x1 >= LCD_WIDTH)  x1 = LCD_WIDTH - 1;
    if (x2 >= LCD_WIDTH)  x2 = LCD_WIDTH - 1;
    if (y1 >= LCD_HEIGHT) y1 = LCD_HEIGHT - 1;
    if (y2 >= LCD_HEIGHT) y2 = LCD_HEIGHT - 1;

    uint32_t width = (x2 >= x1) ? (x2 - x1 + 1) : (x1 - x2 + 1);
    uint32_t height = (y2 >= y1) ? (y2 - y1 + 1) : (y1 - y2 + 1);
    uint32_t count = width * height;

    lcd_address_set(x1, y1, x2, y2);
    for (uint32_t i = 0; i < count; i++) {
        lcd_write_data16(color);
    }
}

void lcd_draw_rect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    lcd_draw_line(x1, y1, x2, y1, color);
    lcd_draw_line(x1, y2, x2, y2, color);
    lcd_draw_line(x1, y1, x1, y2, color);
    lcd_draw_line(x2, y1, x2, y2, color);
}

void lcd_draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    int dx = (x2 >= x1) ? (x2 - x1) : (x1 - x2);
    int dy = (y2 >= y1) ? (y2 - y1) : (y1 - y2);
    int sx = (x1 < x2) ? 1 : -1;
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx - dy;

    while (1) {
        lcd_draw_pixel(x1, y1, color);
        if (x1 == x2 && y1 == y2) {
            break;
        }
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x1 = (uint16_t)(x1 + sx);
        }
        if (e2 < dx) {
            err += dx;
            y1 = (uint16_t)(y1 + sy);
        }
    }
}

void lcd_show_char(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg_color)
{
    if (x > (LCD_WIDTH - 8) || y > (LCD_HEIGHT - 16)) {
        return;
    }
    if (c < ' ' || c > '~') {
        c = ' ';
    }
    uint8_t char_index = (uint8_t)(c - ' ');

    lcd_address_set(x, y, x + 7, y + 15);
    for (uint8_t row = 0; row < 16; row++) {
        uint8_t row_bits = asc2_1608[(uint16_t)char_index * 16U + row];
        for (uint8_t col = 0; col < 8; col++) {
            if (row_bits & 0x80U) {
                lcd_write_data16(color);
            } else {
                lcd_write_data16(bg_color);
            }
            row_bits <<= 1;
        }
    }
}

void lcd_show_string(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg_color)
{
    if (str == NULL) {
        return;
    }
    uint16_t cur_x = x;
    uint16_t cur_y = y;

    while (*str != '\0') {
        if (*str == '\n') {
            cur_x = x;
            cur_y += 18;
            str++;
            continue;
        }
        if (cur_x > (LCD_WIDTH - 8)) {
            cur_x = x;
            cur_y += 18;
        }
        if (cur_y > (LCD_HEIGHT - 16)) {
            break;
        }
        lcd_show_char(cur_x, cur_y, *str, color, bg_color);
        cur_x += 8;
        str++;
    }
}
