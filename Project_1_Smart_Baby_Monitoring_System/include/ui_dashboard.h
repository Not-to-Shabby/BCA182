/**
 * @file ui_dashboard.h
 * @brief ST7789 240x240 baby-monitor dashboard renderer (presentation layer).
 *
 * Modeled on the Laboratory 4 ui_dashboard.c differential-redraw style: a
 * static frame painted once at init, then only changed regions are repainted
 * (string caches compared against the freshly formatted fields).
 */

#ifndef UI_DASHBOARD_H_
#define UI_DASHBOARD_H_

#include "monitor_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Paint the static frame: header bar, card outlines, footer disclaimer. */
void ui_dashboard_init(void);

/**
 * @brief Render one differential update from a state snapshot.
 *
 * Call at the UI cadence (500 ms) with a snapshot COPY taken under the
 * monitor state lock (never the live pointer). Rows: TEMP, HUM, LIGHT, PROX,
 * ACCEL, state banner (green/red/amber), MUTE countdown, uptime. Sensors that
 * are absent show "--" via aht_ok/aps_ok/imu_ok.
 */
void ui_dashboard_update(const monitor_state_t *s);

#ifdef __cplusplus
}
#endif

#endif /* UI_DASHBOARD_H_ */
