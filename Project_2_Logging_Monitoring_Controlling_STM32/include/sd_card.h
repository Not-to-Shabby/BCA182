/**
 * @file sd_card.h
 * @brief microSD (FatFs) access with the proven mount / lock / recover machinery
 *        from the Lab 2 side quest, trimmed for event logging.
 */

#ifndef SD_CARD_H_
#define SD_CARD_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the SDMMC disk and mount the FatFs volume (non-fatal on failure). */
void sd_card_init(void);

/** Serialize every f_* call (FatFs is built without re-entrancy). Never in ISR. */
void sd_card_lock(void);
void sd_card_unlock(void);

/** Restart the STM32 SDIO driver after a failed transfer. Lock must be held. */
bool sd_card_recover(void);

bool sd_card_is_mounted(void);
uint32_t sd_card_get_recover_count(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_CARD_H_ */
