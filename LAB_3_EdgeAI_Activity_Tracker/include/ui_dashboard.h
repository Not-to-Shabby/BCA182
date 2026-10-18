/**
 * ==============================================================================
 * UI Dashboard Interface for ST7789 240x240 IPS Color Display
 * ==============================================================================
 * Target: RT-Thread Spark Board (STM32F407ZGT6)
 * Display: 1.3" 240x240 ST7789 v3 IPS LCD via FSMC 8080 8-Bit Parallel Bus
 */

#ifndef UI_DASHBOARD_H
#define UI_DASHBOARD_H

#include <stdint.h>
#include <stdbool.h>
#include "activity_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the ST7789 display controller, powers on the backlight,
 *        and renders the static dashboard layout.
 */
void ui_dashboard_init(void);

/**
 * @brief Performs flicker-free differential UI updates with the latest
 *        TinyML classification and sensor telemetry.
 * 
 * @param act Classified activity (ACTIVITY_WALKING or ACTIVITY_RUNNING)
 * @param confidence Prediction confidence percentage (0..100)
 * @param cadence_spm Current step cadence in steps per minute
 * @param step_count Cumulative step count
 * @param mag_g Filtered acceleration magnitude in g
 * @param ax Instantaneous X acceleration in g
 * @param ay Instantaneous Y acceleration in g
 * @param az Instantaneous Z acceleration in g
 */
void ui_dashboard_update(
    activity_type_t act,
    uint8_t confidence,
    uint8_t cadence_spm,
    uint32_t step_count,
    float mag_g,
    float ax,
    float ay,
    float az);

/**
 * @brief Displays the Captive Portal setup screen with AP SSID and URL.
 */
void ui_dashboard_show_portal(const char *ap_ssid, const char *url);

/**
 * @brief Displays the interactive Wi-Fi reset confirmation screen.
 * 
 * @param current_ssid Name of the currently configured Wi-Fi network
 * @param countdown_sec Remaining seconds before auto-cancellation
 */
void ui_dashboard_show_reset_confirm(const char *current_ssid, int countdown_sec);

/**
 * @brief Displays the connecting status notification screen on the ST7789 LCD.
 * 
 * @param ssid Name of the Wi-Fi network
 * @param attempt Current attempt index (1..max_attempts, or 0 to reset state)
 * @param max_attempts Total allowed attempts (e.g. 3)
 * @param status_msg Informative status message (e.g. "Associating with AP...")
 */
void ui_dashboard_show_connecting(const char *ssid, int attempt, int max_attempts, const char *status_msg);

#ifdef __cplusplus
}
#endif

#endif /* UI_DASHBOARD_H */
