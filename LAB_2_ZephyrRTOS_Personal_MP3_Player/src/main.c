/**
 * @file main.c
 * @brief Main entry point for Laboratory Activity 2: Personal MP3 Player.
 *        Implements cooperative multi-threading on Zephyr RTOS for the
 *        RT-Thread Spark Development Board (STM32F407ZGT6).
 *
 * Course: BCA182 Embedded Systems Programming
 * Evaluation: Paul Rodolf P. Castor, M.Sc.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "app_config.h"
#include "player_logic.h"
#include "lcd_st7789.h"
#include "threads.h"

/* -------------------------------------------------------------------------- */
/* Thread Stack Allocation & Thread Control Blocks                            */
/* -------------------------------------------------------------------------- */
K_THREAD_STACK_DEFINE(stack_lcd_leds, THREAD_STACK_SIZE_LCD_LEDS);
K_THREAD_STACK_DEFINE(stack_buttons,  THREAD_STACK_SIZE_BUTTONS);
K_THREAD_STACK_DEFINE(stack_volume,   THREAD_STACK_SIZE_VOLUME);

static struct k_thread thread_lcd_leds_data;
static struct k_thread thread_buttons_data;
static struct k_thread thread_volume_data;

/* -------------------------------------------------------------------------- */
/* UART Instructions & User Guide                                             */
/* -------------------------------------------------------------------------- */
static void print_uart_instructions(void)
{
    printk("\n%s", APP_BANNER_LINE);
    printk("  BCA182: Laboratory Activity 2 - Personal MP3 Player\n");
    printk("  Target: RT-Thread Spark Board (STM32F407ZGT6)\n");
    printk("  RTOS:   Zephyr RTOS v4.x (Cooperative Multithreading)\n");
    printk("%s", APP_BANNER_LINE);
    printk("OPERATING INSTRUCTIONS:\n");
    printk("  1. Binary Song Selection (8 Songs Available):\n");
    printk("     - Button 2 (LSB) | Button 3 (Bit 1) | Button 4 (MSB)\n");
    printk("     - While holding Buttons 2-4 in your chosen binary combination,\n");
    printk("       press Button 1 to select the prospective song.\n");
    printk("     - Release all buttons.\n");
    printk("  2. 5-Second Confirmation Window:\n");
    printk("     - Press Button 1 again within 5 seconds to confirm your choice.\n");
    printk("     - If no action is taken within 5 seconds, selection aborts.\n");
    printk("  3. Play / Pause / Replay Control:\n");
    printk("     - Press USER_BUTTON to toggle Play and Pause.\n");
    printk("  4. Volume Adjustment:\n");
    printk("     - Rotate 10k potentiometer to adjust volume (0%% - 100%%).\n");
    printk("  5. RGB LED State Indicators:\n");
    printk("     - BLUE LED  : Song is PLAYING\n");
    printk("     - RED LED   : Song is PAUSED or STOPPED\n");
    printk("     - GREEN LED : Song selection in progress (CONFIRMING)\n");
    printk("%s\n", APP_BANNER_LINE);
}

/* -------------------------------------------------------------------------- */
/* Main Application Entry                                                     */
/* -------------------------------------------------------------------------- */
int main(void)
{
    /* 1. Transmit user operating guide via UART1 */
    print_uart_instructions();

    /* 2. Initialize GPIO buttons, LEDs, and ADC1 potentiometer */
    printk("[System] Initializing buttons (PC0,PC1,PC4,PC5,PA0) and ADC1 (PA1)...\n");
    init_player_peripherals();

    /* 3. Initialize ST7789 LCD display, FSMC 8080 bus, and Backlight (PF9) */
    k_mutex_lock(&g_lcd_mutex, K_FOREVER);
    printk("[System] Initializing ST7789 240x240 LCD display...\n");
    lcd_st7789_init();
    printk("[System] ST7789 hardware display ready.\n");
    k_mutex_unlock(&g_lcd_mutex);

    /* 4. Start all 3 cooperative Zephyr threads */
    printk("[System] Spawning 3 application threads...\n");

    /* Thread 1: Update LCD and RGB LEDs */
    k_thread_create(&thread_lcd_leds_data, stack_lcd_leds,
                    K_THREAD_STACK_SIZEOF(stack_lcd_leds),
                    update_lcd_leds_thread,
                    NULL, NULL, NULL,
                    THREAD_PRIORITY_LCD_LEDS, 0, K_NO_WAIT);
    k_thread_name_set(&thread_lcd_leds_data, "lcd_led_thread");

    /* Thread 2: Polling buttons & 5-second confirmation */
    k_thread_create(&thread_buttons_data, stack_buttons,
                    K_THREAD_STACK_SIZEOF(stack_buttons),
                    polling_buttons,
                    NULL, NULL, NULL,
                    THREAD_PRIORITY_BUTTONS, 0, K_NO_WAIT);
    k_thread_name_set(&thread_buttons_data, "button_thread");

    /* Thread 3: Volume potentiometer adjustment */
    k_thread_create(&thread_volume_data, stack_volume,
                    K_THREAD_STACK_SIZEOF(stack_volume),
                    adjust_volume,
                    NULL, NULL, NULL,
                    THREAD_PRIORITY_VOLUME, 0, K_NO_WAIT);
    k_thread_name_set(&thread_volume_data, "volume_thread");

    printk("[System] All threads started. Entering low-power sleep loop.\n\n");

    /* 4. Main thread enters low-power sleep loop to conserve energy */
    while (1) {
        k_sleep(K_FOREVER);
    }

    return 0;
}
