#ifndef SD_CARD_READER_H_
#define SD_CARD_READER_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SD_TRACKS 16
#define MAX_FILENAME_LEN 64

typedef struct {
    char filename[MAX_FILENAME_LEN];
    uint32_t size_bytes;
} sd_track_t;

void sd_card_reader_init(void);

/**
 * @brief Serialize FatFs access. FatFs is built without re-entrancy, and the sequencer thread
 *        reads the song from the card while other threads read the index or save settings.
 *        Hold it around every f_* call. It is a mutex, so never take it in an ISR.
 */
void sd_card_lock(void);
void sd_card_unlock(void);

/**
 * @brief Bring the card back after a failed transfer.
 *
 * One bad transfer can leave the STM32 SDIO driver stuck for good: the DMA channel stays claimed,
 * the driver reports the disk as uninitialised so FatFs refuses every call before touching the
 * card, or a stale completion makes the next read return before its data arrived. This restarts
 * the driver (which re-runs the card initialisation) and clears the completion semaphore. Files
 * that are open stay valid in FatFs, but a file that failed must still be reopened because FatFs
 * keeps the error on the file object. Safe to call with sd_card_lock() held.
 *
 * @return true if the card answers again.
 */
bool sd_card_recover(void);

/** Number of times sd_card_recover() has run since boot. */
uint32_t sd_card_get_recover_count(void);

bool sd_card_is_mounted(void);
uint8_t sd_card_get_track_count(void);
const sd_track_t *sd_card_get_track(uint8_t index);

#ifdef __cplusplus
}
#endif

#endif
