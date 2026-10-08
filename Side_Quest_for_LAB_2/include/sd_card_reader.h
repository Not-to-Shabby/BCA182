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
bool sd_card_is_mounted(void);
uint8_t sd_card_get_track_count(void);
const sd_track_t *sd_card_get_track(uint8_t index);

#ifdef __cplusplus
}
#endif

#endif
