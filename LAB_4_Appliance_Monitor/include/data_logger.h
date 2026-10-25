/**
 * @file data_logger.h
 * @brief Timestamped appliance event log: RAM ring + microSD CSV persistence.
 *
 * Every product state transition (including DONE alerts and their
 * acknowledgment) is recorded as one CSV row so the report can reconstruct
 * cycle timelines. When the card is absent, events stay in the RAM ring and
 * are dumped to the console in service mode.
 */

#ifndef DATA_LOGGER_H
#define DATA_LOGGER_H

#include <stdint.h>
#include <stdbool.h>
#include "cycle_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DATA_LOG_RING_EVENTS 32U

/** Mount the card and open/append the log file. Safe to call once at boot. */
void data_logger_init(void);

/**
 * @brief Record one transition. from_state == to_state records a note event.
 * @param timestamp_ms Zephyr uptime in ms (RTC stamping is a Phase 6 upgrade).
 */
void data_logger_event(cycle_state_t from_state, cycle_state_t to_state,
                       uint32_t timestamp_ms, bool alert);

/** Number of events retained in RAM (0..DATA_LOG_RING_EVENTS). */
uint32_t data_logger_count(void);

/** Accessor for diagnostics rendering. */
const char *data_logger_event_text(uint32_t index, char *buf, uint32_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* DATA_LOGGER_H */
