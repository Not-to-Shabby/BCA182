/**
 * @file data_logger.c
 * @brief Event ring + SD CSV writer.
 */

#include "data_logger.h"
#include "sd_card.h"
#include <string.h>
#include <stdio.h>

#if defined(__arm__) || defined(CONFIG_SOC_SERIES_STM32F4X) || defined(ZEPHYR_VERSION_CODE)
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <ff.h>
#else
#include <stdio.h>
#include <stdarg.h>
#define printk printf
#endif

#define LOG_PATH "SD:/cycle_log.csv"

typedef struct {
    uint32_t timestamp_ms;
    uint8_t from_state;
    uint8_t to_state;
    bool alert;
} log_event_t;

static log_event_t s_ring[DATA_LOG_RING_EVENTS];
static uint32_t s_head;   /* next write index */
static uint32_t s_total;  /* events since boot (wraps the ring) */

#if defined(__arm__) || defined(CONFIG_SOC_SERIES_STM32F4X) || defined(ZEPHYR_VERSION_CODE)
static bool append_sd(uint32_t ts, uint8_t from_state, uint8_t to_state, bool alert)
{
    if (!sd_card_is_mounted()) {
        return false;
    }

    bool ok = false;
    sd_card_lock();
    FIL file;
    FRESULT fr = f_open(&file, LOG_PATH, FA_OPEN_APPEND | FA_WRITE);
    if (fr == FR_OK) {
        char line[64];
        int len = snprintf(line, sizeof(line), "%u,%u,%u,%u\n",
                           (unsigned)ts, from_state, to_state, alert ? 1U : 0U);
        UINT written = 0;
        if (f_write(&file, line, (UINT)len, &written) == FR_OK && written == (UINT)len) {
            ok = true;
        }
        f_close(&file);
    } else {
        (void)sd_card_recover();
    }
    sd_card_unlock();
    return ok;
}
#else
static bool append_sd(uint32_t ts, uint8_t from_state, uint8_t to_state, bool alert)
{
    (void)ts; (void)from_state; (void)to_state; (void)alert;
    return false;
}
#endif

void data_logger_init(void)
{
    s_head = 0;
    s_total = 0;
    sd_card_init();

    if (sd_card_is_mounted()) {
        /* Fresh card: write the header once per boot session is noisy, so only
         * create the file with a header when it does not exist yet. */
        sd_card_lock();
        FIL file;
        FRESULT fr = f_open(&file, LOG_PATH, FA_OPEN_EXISTING | FA_READ);
        if (fr == FR_OK) {
            f_close(&file);
        } else {
            if (f_open(&file, LOG_PATH, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
                static const char header[] = "uptime_ms,from_state,to_state,alert\n";
                UINT written = 0;
                (void)f_write(&file, header, sizeof(header) - 1U, &written);
                f_close(&file);
            }
        }
        sd_card_unlock();
        printk("[LOG] Event log at %s\n", LOG_PATH);
    } else {
        printk("[LOG] No SD card: events kept in RAM ring only\n");
    }
}

void data_logger_event(cycle_state_t from_state, cycle_state_t to_state,
                       uint32_t timestamp_ms, bool alert)
{
    log_event_t *ev = &s_ring[s_head % DATA_LOG_RING_EVENTS];
    ev->timestamp_ms = timestamp_ms;
    ev->from_state = (uint8_t)from_state;
    ev->to_state = (uint8_t)to_state;
    ev->alert = alert;
    s_head++;
    s_total++;

    bool sd_ok = append_sd(timestamp_ms, (uint8_t)from_state, (uint8_t)to_state, alert);
    printk("[LOG] %u ms: %s -> %s%s%s\n",
           (unsigned)timestamp_ms, cycle_state_name(from_state), cycle_state_name(to_state),
           alert ? " ALERT" : "", sd_ok ? " (sd)" : "");
}

uint32_t data_logger_count(void)
{
    return s_total < DATA_LOG_RING_EVENTS ? s_total : DATA_LOG_RING_EVENTS;
}

const char *data_logger_event_text(uint32_t index, char *buf, uint32_t buf_len)
{
    uint32_t stored = data_logger_count();
    if (buf == NULL || buf_len == 0 || index >= stored) { /* cppcheck-suppress knownConditionTrueFalse ; events arrive at runtime */
        return NULL;
    }
    uint32_t ring_index = (s_head - 1U - index) % DATA_LOG_RING_EVENTS;
    const log_event_t *ev = &s_ring[ring_index];
    snprintf(buf, buf_len, "%8us %s->%s%s",
             (unsigned)(ev->timestamp_ms / 1000U),
             cycle_state_name((cycle_state_t)ev->from_state),
             cycle_state_name((cycle_state_t)ev->to_state),
             ev->alert ? " !" : "");
    return buf;
}
