/**
 * @file ui_dashboard.h
 * @brief ST7789 dashboard renderer for the appliance cycle monitor.
 *
 * One render call per product mode; the UI thread calls it at 4 Hz with the
 * latest decision + context snapshot. Only changed regions are redrawn where
 * the content is static per mode.
 */

#ifndef UI_DASHBOARD_H
#define UI_DASHBOARD_H

#include <stdint.h>
#include <stdbool.h>
#include "cycle_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Product/service modes (service_mode.h owns the state machine; UI only renders) */
typedef enum {
    UI_MODE_RUN = 0,
    UI_MODE_DIAG,
    UI_MODE_COLLECT,
    UI_MODE_BENCH,
} ui_mode_t;

typedef struct {
    ui_mode_t mode;
    cycle_decision_t decision;
    float rms_g;
    float dom_hz;
    float temperature_c;    /* NaN when sensor absent */
    float humidity_pct;
    uint16_t lux;           /* 0xFFFF = absent */
    bool alert;             /* blink phase for the banner */
    uint8_t diag_page;      /* service-mode diagnostics page (KEY0 toggles) */
    uint32_t collect_samples;
    int collect_label;
    uint32_t bench_done;
    uint32_t bench_median_us;
    uint32_t bench_p95_us;
} ui_snapshot_t;

/** Initialize the LCD and paint the static frame. */
void ui_dashboard_init(void);

/** Render one frame from the snapshot (cheap enough for 4 Hz). */
void ui_dashboard_render(const ui_snapshot_t *snap);

#ifdef __cplusplus
}
#endif

#endif /* UI_DASHBOARD_H */
