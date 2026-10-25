/**
 * @file service_mode.h
 * @brief Service-mode state machine: RUN product, DIAGnostics, COLLECT (training
 *        data capture) and BENCH (inference timing) — plus the debounced buttons.
 */

#ifndef SERVICE_MODE_H
#define SERVICE_MODE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SVC_RUN = 0,
    SVC_DIAG,
    SVC_COLLECT,
    SVC_BENCH,
} service_mode_t;

/** Initialize button GPIOs (PC0 = KEY0, PC1 = KEY1, active LOW). */
void service_mode_init(void);

/**
 * @brief Poll the buttons; call from the service thread at 50 ms.
 *
 * Button semantics:
 *   RUN:     KEY0 = acknowledge DONE alert; KEY1 long (>= 1.5 s) = enter DIAG.
 *   DIAG:    KEY0 = toggle diagnostics page; KEY1 = next mode (COLLECT).
 *   COLLECT: KEY0 = cycle training label (IDLE->WASH->SPIN); KEY1 = next mode (BENCH).
 *   BENCH:   KEY1 (short or long) = exit to RUN. KEY0 ignored while running.
 */
void service_mode_poll(void);

service_mode_t service_mode_get(void);

/** Active training label for COLLECT mode (0..2). */
int service_mode_collect_label(void);

/** true while COLLECT is streaming samples. */
bool service_mode_is_collecting(void);

/** Increments once per collected sample (for the UI counter). */
uint32_t service_mode_collect_samples(void);

/** Bench progress accessors (0 when idle). */
uint32_t service_mode_bench_done(void);
uint32_t service_mode_bench_median_us(void);
uint32_t service_mode_bench_p95_us(void);

/** Current diagnostics page (KEY0 toggled). */
uint8_t service_mode_diag_page(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_MODE_H */
