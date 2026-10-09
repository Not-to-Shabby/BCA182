/**
 * @file midi_source.h
 * @brief Where the bytes of a Standard MIDI File come from: a flash/RAM array, or a read
 *        callback that fetches them from the SD card on demand.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#ifndef MIDI_SOURCE_H_
#define MIDI_SOURCE_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** Whole file in memory (built-in songs). When NULL, @ref read supplies the bytes. */
    const uint8_t *mem;
    /** File length in bytes. */
    uint32_t size;
    /** Reads exactly @p len bytes starting at @p offset into @p dst; false on any failure.
     *  @p dst is always ordinary SRAM, so it is safe as a DMA target. */
    bool (*read)(void *ctx, uint32_t offset, uint8_t *dst, uint32_t len);
    void *ctx;
} midi_source_t;

#ifdef __cplusplus
}
#endif

#endif /* MIDI_SOURCE_H_ */
