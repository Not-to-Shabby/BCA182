/**
 * @file synth_sections.h
 * @brief The mixer sections and the rule that puts a General MIDI program or a drum key into one.
 *        Header-only so the synthesizer, the mixer model and the host tests share one definition.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#ifndef SYNTH_SECTIONS_H_
#define SYNTH_SECTIONS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Every melodic note belongs to one section by its General MIDI program and every drum hit by
 * its key, so a section can be turned up or down whatever channel plays it.
 */
typedef enum {
    SYNTH_SECT_PIANO = 0,   /* programs 0-15: pianos, chromatic percussion */
    SYNTH_SECT_ORGAN,       /* 16-23 */
    SYNTH_SECT_GUITAR,      /* 24-31 */
    SYNTH_SECT_BASS,        /* 32-39 */
    SYNTH_SECT_STRINGS,     /* 40-55: strings, ensembles, choir */
    SYNTH_SECT_BRASS,       /* 56-63 */
    SYNTH_SECT_REEDS,       /* 64-79: reeds and pipes */
    SYNTH_SECT_LEAD,        /* 80-87 */
    SYNTH_SECT_PADS,        /* 88-103: pads and synth effects */
    SYNTH_SECT_OTHER,       /* 104-127: ethnic, percussive, sound effects */
    SYNTH_SECT_KICK,        /* drum keys 35, 36 */
    SYNTH_SECT_SNARE,       /* 37-40: stick, snare, clap */
    SYNTH_SECT_HATS,        /* 42, 44, 46 */
    SYNTH_SECT_CYMBALS,     /* 49, 51-53, 55, 57, 59 */
    SYNTH_SECT_TOMS,        /* toms and all other percussion */
    SYNTH_SECT_COUNT
} synth_section_t;

#define SYNTH_SECT_DRUMS_FIRST  SYNTH_SECT_KICK

static inline uint8_t synth_section_of_program(uint8_t program)
{
    program &= 127U;
    if (program < 16U) return SYNTH_SECT_PIANO;
    if (program < 24U) return SYNTH_SECT_ORGAN;
    if (program < 32U) return SYNTH_SECT_GUITAR;
    if (program < 40U) return SYNTH_SECT_BASS;
    if (program < 56U) return SYNTH_SECT_STRINGS;
    if (program < 64U) return SYNTH_SECT_BRASS;
    if (program < 80U) return SYNTH_SECT_REEDS;
    if (program < 88U) return SYNTH_SECT_LEAD;
    if (program < 104U) return SYNTH_SECT_PADS;
    return SYNTH_SECT_OTHER;
}

static inline uint8_t synth_section_of_drum(uint8_t note)
{
    switch (note) {
    case 35: case 36:
        return SYNTH_SECT_KICK;
    case 37: case 38: case 39: case 40:
        return SYNTH_SECT_SNARE;
    case 42: case 44: case 46:
        return SYNTH_SECT_HATS;
    case 49: case 51: case 52: case 53: case 55: case 57: case 59:
        return SYNTH_SECT_CYMBALS;
    default:
        return SYNTH_SECT_TOMS;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* SYNTH_SECTIONS_H_ */
