/**
 * @file watchdog.h
 * @brief Hardware watchdog (IWDG) + liveness supervision for the
 *        Smart Baby Monitor. Spec requirement: "initialize the logger,
 *        watchdog as appropriate ... log failures and implement sensible
 *        responses to sensor, Wi-Fi, and storage faults."
 *
 * The IWDG (16 s timeout) is fed from the audio capture thread (~8 ms
 * cadence, the fastest vital thread) ONLY when every supervised thread
 * has advanced its progress counter since the previous feed. A wedged
 * thread (the non-deterministic freeze observed at first-window compute)
 * stops the feed and the IWDG resets the board: the system SELF-RECOVERS
 * and the boot log after the reset documents the recovery.
 */
#ifndef WATCHDOG_H_
#define WATCHDOG_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One-time IWDG setup. Safe to call repeatedly; errors are non-fatal
 *  (the monitor runs unwatched rather than not at all, logged once). */
void watchdog_init(void);

/** Register a supervised thread: returns a slot index (0..7), or -1 when
 *  the table is full. Call once per thread at its startup. */
int watchdog_register_thread(const char *name);

/** Supervised threads call this each loop iteration to mark progress. */
void watchdog_progress(int slot);

/** Feed point (audio capture thread): feeds the IWDG only if every
 *  registered thread progressed since the previous call. */
void watchdog_feed_check(void);

#ifdef __cplusplus
}
#endif

#endif /* WATCHDOG_H_ */
