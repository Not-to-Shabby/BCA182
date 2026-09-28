#include "ssd1306.h"
#include <string.h>

I2C_HandleTypeDef hi2c1;

// Framebuffer (128x64 bits = 1024 bytes)
static uint8_t ssd1306_buffer[1024];

// Standard 5x7 ASCII font table (offset from ASCII 32)
static const uint8_t font5x7[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, // 32 ' '
    0x00, 0x00, 0x5F, 0x00, 0x00, // 33 '!'
    0x00, 0x07, 0x00, 0x07, 0x00, // 34 '"'
    0x14, 0x7F, 0x14, 0x7F, 0x14, // 35 '#'
    0x24, 0x2A, 0x7F, 0x2A, 0x12, // 36 '$'
    0x23, 0x13, 0x08, 0x64, 0x62, // 37 '%'
    0x36, 0x49, 0x55, 0x22, 0x50, // 38 '&'
    0x00, 0x05, 0x03, 0x00, 0x00, // 39 '''
    0x00, 0x1C, 0x22, 0x41, 0x00, // 40 '('
    0x00, 0x41, 0x22, 0x1C, 0x00, // 41 ')'
    0x14, 0x08, 0x3E, 0x08, 0x14, // 42 '*'
    0x08, 0x08, 0x3E, 0x08, 0x08, // 43 '+'
    0x00, 0x50, 0x30, 0x00, 0x00, // 44 ','
    0x08, 0x08, 0x08, 0x08, 0x08, // 45 '-'
    0x00, 0x60, 0x60, 0x00, 0x00, // 46 '.'
    0x20, 0x10, 0x08, 0x04, 0x02, // 47 '/'
    0x3E, 0x51, 0x49, 0x45, 0x3E, // 48 '0'
    0x00, 0x42, 0x7F, 0x40, 0x00, // 49 '1'
    0x42, 0x61, 0x51, 0x49, 0x46, // 50 '2'
    0x21, 0x41, 0x45, 0x4B, 0x31, // 51 '3'
    0x18, 0x14, 0x12, 0x7F, 0x10, // 52 '4'
    0x27, 0x45, 0x45, 0x45, 0x39, // 53 '5'
    0x3C, 0x4A, 0x49, 0x49, 0x30, // 54 '6'
    0x01, 0x71, 0x09, 0x05, 0x03, // 55 '7'
    0x36, 0x49, 0x49, 0x49, 0x36, // 56 '8'
    0x06, 0x49, 0x49, 0x29, 0x1E, // 57 '9'
    0x00, 0x36, 0x36, 0x00, 0x00, // 58 ':'
    0x00, 0x56, 0x36, 0x00, 0x00, // 59 ';'
    0x08, 0x14, 0x22, 0x41, 0x00, // 60 '<'
    0x14, 0x14, 0x14, 0x14, 0x14, // 61 '='
    0x00, 0x41, 0x22, 0x14, 0x08, // 62 '>'
    0x02, 0x01, 0x51, 0x09, 0x06, // 63 '?'
    0x32, 0x49, 0x79, 0x41, 0x3E, // 64 '@'
    0x7E, 0x11, 0x11, 0x11, 0x7E, // 65 'A'
    0x7F, 0x49, 0x49, 0x49, 0x36, // 66 'B'
    0x3E, 0x41, 0x41, 0x41, 0x22, // 67 'C'
    0x7F, 0x41, 0x41, 0x22, 0x1C, // 68 'D'
    0x7F, 0x49, 0x49, 0x49, 0x41, // 69 'E'
    0x7F, 0x09, 0x09, 0x09, 0x01, // 70 'F'
    0x3E, 0x41, 0x49, 0x49, 0x7A, // 71 'G'
    0x7F, 0x08, 0x08, 0x08, 0x7F, // 72 'H'
    0x00, 0x41, 0x7F, 0x41, 0x00, // 73 'I'
    0x20, 0x40, 0x41, 0x3F, 0x01, // 74 'J'
    0x7F, 0x08, 0x14, 0x22, 0x41, // 75 'K'
    0x7F, 0x40, 0x40, 0x40, 0x40, // 76 'L'
    0x7F, 0x02, 0x0C, 0x02, 0x7F, // 77 'M'
    0x7F, 0x04, 0x08, 0x10, 0x7F, // 78 'N'
    0x3E, 0x41, 0x41, 0x41, 0x3E, // 79 'O'
    0x7F, 0x09, 0x09, 0x09, 0x06, // 80 'P'
    0x3E, 0x41, 0x51, 0x21, 0x5E, // 81 'Q'
    0x7F, 0x09, 0x19, 0x29, 0x46, // 82 'R'
    0x46, 0x49, 0x49, 0x49, 0x31, // 83 'S'
    0x01, 0x01, 0x7F, 0x01, 0x01, // 84 'T'
    0x3F, 0x40, 0x40, 0x40, 0x3F, // 85 'U'
    0x1F, 0x20, 0x40, 0x20, 0x1F, // 86 'V'
    0x3F, 0x40, 0x38, 0x40, 0x3F, // 87 'W'
    0x63, 0x14, 0x08, 0x14, 0x63, // 88 'X'
    0x07, 0x08, 0x70, 0x08, 0x07, // 89 'Y'
    0x61, 0x51, 0x49, 0x45, 0x43, // 90 'Z'
    0x00, 0x7F, 0x41, 0x41, 0x00, // 91 '['
    0x02, 0x04, 0x08, 0x10, 0x20, // 92 '\'
    0x00, 0x41, 0x41, 0x7F, 0x00, // 93 ']'
    0x04, 0x02, 0x01, 0x02, 0x04, // 94 '^'
    0x40, 0x40, 0x40, 0x40, 0x40, // 95 '_'
    0x00, 0x01, 0x02, 0x04, 0x00, // 96 '`'
    0x20, 0x54, 0x54, 0x54, 0x78, // 97 'a'
    0x7F, 0x48, 0x44, 0x44, 0x38, // 98 'b'
    0x38, 0x44, 0x44, 0x44, 0x20, // 99 'c'
    0x38, 0x44, 0x44, 0x48, 0x7F, // 100 'd'
    0x38, 0x54, 0x54, 0x54, 0x18, // 101 'e'
    0x08, 0x7E, 0x09, 0x01, 0x02, // 102 'f'
    0x0C, 0x52, 0x52, 0x52, 0x3E, // 103 'g'
    0x7F, 0x08, 0x04, 0x04, 0x78, // 104 'h'
    0x00, 0x44, 0x7D, 0x40, 0x00, // 105 'i'
    0x20, 0x40, 0x44, 0x3D, 0x00, // 106 'j'
    0x7F, 0x10, 0x28, 0x44, 0x00, // 107 'k'
    0x00, 0x41, 0x7F, 0x40, 0x00, // 108 'l'
    0x7C, 0x04, 0x18, 0x04, 0x78, // 109 'm'
    0x7C, 0x08, 0x04, 0x04, 0x78, // 110 'n'
    0x38, 0x44, 0x44, 0x44, 0x38, // 111 'o'
    0x7C, 0x14, 0x14, 0x14, 0x08, // 112 'p'
    0x08, 0x14, 0x14, 0x18, 0x7C, // 113 'q'
    0x7C, 0x08, 0x04, 0x04, 0x08, // 114 'r'
    0x48, 0x54, 0x54, 0x54, 0x20, // 115 's'
    0x04, 0x3F, 0x44, 0x40, 0x20, // 116 't'
    0x3C, 0x40, 0x40, 0x20, 0x7C, // 117 'u'
    0x1C, 0x20, 0x40, 0x20, 0x1C, // 118 'v'
    0x3C, 0x40, 0x30, 0x40, 0x3C, // 119 'w'
    0x44, 0x28, 0x10, 0x28, 0x44, // 120 'x'
    0x0C, 0x50, 0x50, 0x50, 0x3C, // 121 'y'
    0x44, 0x64, 0x54, 0x4C, 0x44, // 122 'z'
    0x00, 0x08, 0x36, 0x41, 0x00, // 123 '{'
    0x00, 0x00, 0x7F, 0x00, 0x00, // 124 '|'
    0x00, 0x41, 0x36, 0x08, 0x00, // 125 '}'
    0x08, 0x08, 0x2A, 0x1C, 0x08  // 126 '~'
};

// -------------------------------------------------------------
// STM32 HAL Hardware I2C1 Bus Functions (PB6 = SCL, PB7 = SDA)
// -------------------------------------------------------------

uint8_t ssd1306_i2c_init(void) {
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_I2C1_CLK_ENABLE();

    // PB6 (SCL) & PB7 (SDA) configured as Alternate Function Open Drain
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    // Initialize I2C1 peripheral in Fast Mode (400 kHz)
    hi2c1.Instance = I2C1;
    hi2c1.Init.ClockSpeed = 400000;
    hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
    hi2c1.Init.OwnAddress1 = 0;
    hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;

    if (HAL_I2C_Init(&hi2c1) != HAL_OK) {
        return 1;
    }
    return 0;
}

uint8_t ssd1306_probe(void) {
    // Check if SSD1306 responds to its 7-bit slave address
    HAL_StatusTypeDef status = HAL_I2C_IsDeviceReady(&hi2c1, SSD1306_I2C_ADDR, 2, 50);
    return (status == HAL_OK) ? 0 : 1;
}

static void ssd1306_write_command(uint8_t cmd) {
    // Control byte 0x00 = Command mode
    HAL_I2C_Mem_Write(&hi2c1, SSD1306_I2C_ADDR, 0x00, I2C_MEMADD_SIZE_8BIT, &cmd, 1, 100);
}

static void ssd1306_write_commands(const uint8_t *cmds, uint16_t count) {
    for (uint16_t i = 0; i < count; i++) {
        ssd1306_write_command(cmds[i]);
    }
}

uint8_t ssd1306_init(void) {
    if (ssd1306_i2c_init() != 0) {
        return 1;
    }

    if (ssd1306_probe() != 0) {
        return 2; // Device not acknowledging
    }

    // Standard SSD1306 128x64 display configuration table
    static const uint8_t init_cmds[] = {
        0xAE,       // Display OFF
        0xD5, 0x80, // Set Display Clock Divide Ratio / Oscillator Frequency
        0xA8, 0x3F, // Set Multiplex Ratio (64 lines)
        0xD3, 0x00, // Set Display Offset (0)
        0x40,       // Set Display Start Line to 0
        0x8D, 0x14, // Enable internal Charge Pump
        0x20, 0x00, // Set Horizontal Memory Addressing Mode
        0xA1,       // Set Segment Re-map (column 127 is mapped to SEG0)
        0xC8,       // Set COM Output Scan Direction (remapped mode)
        0xDA, 0x12, // Set COM Pins Hardware Configuration
        0x81, 0xCF, // Set Contrast Control
        0xD9, 0xF1, // Set Pre-charge Period
        0xDB, 0x40, // Set VCOMH Deselect Level
        0xA4,       // Entire Display ON (follow RAM)
        0xA6,       // Set Normal Display mode (non-inverted)
        0xAF        // Display ON
    };

    ssd1306_write_commands(init_cmds, sizeof(init_cmds));
    ssd1306_clear();
    ssd1306_update_screen();
    return 0;
}

void ssd1306_clear(void) {
    memset(ssd1306_buffer, 0, sizeof(ssd1306_buffer));
}

void ssd1306_update_screen(void) {
    // Set column address range 0..127
    ssd1306_write_command(0x21);
    ssd1306_write_command(0x00);
    ssd1306_write_command(127);

    // Set page address range 0..7
    ssd1306_write_command(0x22);
    ssd1306_write_command(0x00);
    ssd1306_write_command(0x07);

    // Stream entire 1024-byte framebuffer via Hardware I2C (control byte 0x40 = Data stream)
    HAL_I2C_Mem_Write(&hi2c1, SSD1306_I2C_ADDR, 0x40, I2C_MEMADD_SIZE_8BIT, ssd1306_buffer, sizeof(ssd1306_buffer), 1000);
}

void ssd1306_draw_pixel(int16_t x, int16_t y, uint8_t color) {
    if (x < 0 || x >= SSD1306_WIDTH || y < 0 || y >= SSD1306_HEIGHT) {
        return;
    }
    uint16_t index = x + (y / 8) * SSD1306_WIDTH;
    if (color) {
        ssd1306_buffer[index] |= (1 << (y % 8));
    } else {
        ssd1306_buffer[index] &= ~(1 << (y % 8));
    }
}

void ssd1306_draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color) {
    int16_t dx = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
    int16_t dy = (y1 >= y0) ? (y1 - y0) : (y0 - y1);
    int16_t sx = (x0 < x1) ? 1 : -1;
    int16_t sy = (y0 < y1) ? 1 : -1;
    int16_t err = dx - dy;

    while (1) {
        ssd1306_draw_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int16_t e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void ssd1306_draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color) {
    ssd1306_draw_line(x, y, x + w - 1, y, color);
    ssd1306_draw_line(x, y + h - 1, x + w - 1, y + h - 1, color);
    ssd1306_draw_line(x, y, x, y + h - 1, color);
    ssd1306_draw_line(x + w - 1, y, x + w - 1, y + h - 1, color);
}

void ssd1306_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color) {
    for (int16_t i = 0; i < h; i++) {
        ssd1306_draw_line(x, y + i, x + w - 1, y + i, color);
    }
}

void ssd1306_draw_char(int16_t x, int16_t y, char c, uint8_t color) {
    if (c < 32 || c > 126) {
        c = '?';
    }
    uint16_t font_index = (c - 32) * 5;
    for (uint8_t col = 0; col < 5; col++) {
        uint8_t line = font5x7[font_index + col];
        for (uint8_t row = 0; row < 7; row++) {
            if (line & (1 << row)) {
                ssd1306_draw_pixel(x + col, y + row, color);
            } else {
                ssd1306_draw_pixel(x + col, y + row, !color);
            }
        }
    }
    // Column 6 is 1-pixel horizontal font spacing
    for (uint8_t row = 0; row < 7; row++) {
        ssd1306_draw_pixel(x + 5, y + row, !color);
    }
}

void ssd1306_draw_string(int16_t x, int16_t y, const char *str, uint8_t color) {
    while (*str) {
        ssd1306_draw_char(x, y, *str++, color);
        x += 6;
        if (x + 6 > SSD1306_WIDTH) {
            break;
        }
    }
}
