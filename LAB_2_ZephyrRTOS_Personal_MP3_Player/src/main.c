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
#include "audio_engine.h"
#include "audio_codec_es8388.h"
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
    static const char banner[] =
        "\n=====================================================\n"
        "  BCA182: Laboratory Activity 2 - Personal MP3 Player\n"
        "  Target: RT-Thread Spark Board (STM32F407ZGT6)\n"
        "  RTOS:   Zephyr RTOS v4.x (Cooperative Multithreading)\n"
        "=====================================================\n"
        "OPERATING INSTRUCTIONS (Directional D-Pad Navigation):\n"
        "  1. Track Selection (UP / DOWN):\n"
        "     - UP Button (PC5)   : Click -> Next Track (+1) | Hold -> Jump to Track 1\n"
        "     - DOWN Button (PC1) : Click -> Prev Track (-1) | Hold -> Play/Pause Toggle\n"
        "  2. Volume Adjustment (LEFT / RIGHT / AUX):\n"
        "     - LEFT Button (PC0)  : Click -> Vol -5% | Hold -> Smooth Vol Down\n"
        "     - RIGHT Button (PC4) : Click -> Vol +5% | Hold -> Smooth Vol Up\n"
        "     - AUX Button (PA1)   : Click -> Preset Cycle | Hold -> Mute/Unmute Toggle\n"
        "  3. Audio Transport Control (PLAY / PAUSE / STOP):\n"
        "     - PA0 (USER_BUTTON)  : Click -> Play/Pause | Hold -> Stop Playback\n"
        "     - DOWN (Long-Press)  : Hold >= 450ms -> Play/Pause Toggle on current track\n"
        "  4. Buzzer Mute Toggle (Headphone Mode):\n"
        "     - Hold LEFT + RIGHT together (>= 450ms) to Mute / Unmute the Buzzer\n"
        "  5. RGB LED State Indicators:\n"
        "     - BLUE LED (PF11) : Song is PLAYING\n"
        "     - RED LED (PF12)  : Song is PAUSED or STOPPED\n"
        "=====================================================\n";

    printk("%s", banner);
    uart1_direct_print(banner);
}

/* -------------------------------------------------------------------------- */
/* Main Application Entry                                                     */
/* -------------------------------------------------------------------------- */
int main(void)
{
    /* 1. Initialize GPIO buttons, LEDs, and direct hardware USART1 on PA9/PA10 */
    init_player_peripherals();

    /* 2. Transmit user operating guide via direct USART1 (ST-LINK VCP on COM7) */
    print_uart_instructions();

    /* 3. Initialize ST7789 LCD display, FSMC 8080 bus, and Backlight (PF9) */
    k_mutex_lock(&g_lcd_mutex, K_FOREVER);
    printk("[System] Initializing ST7789 240x240 LCD display...\n");
    uart1_direct_print("[System] Initializing ST7789 240x240 LCD display...\n");
    lcd_st7789_init();
    printk("[System] ST7789 hardware display ready.\n");
    uart1_direct_print("[System] ST7789 hardware display ready.\n");
    k_mutex_unlock(&g_lcd_mutex);

    /* 4. Initialize hardware audio synthesizer (ES8388 Codec + 12-bit Analog DAC1) */
    printk("[System] Initializing audio synthesizer (ES8388 CN3 + PA4 DAC)...\n");
    uart1_direct_print("[System] Initializing audio synthesizer (ES8388 CN3 + PA4 DAC)...\n");
    audio_engine_init();

    /* 5. Start all 3 cooperative Zephyr threads */
    printk("[System] Spawning 3 application threads...\n");
    uart1_direct_print("[System] Spawning 3 application threads...\n");

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
