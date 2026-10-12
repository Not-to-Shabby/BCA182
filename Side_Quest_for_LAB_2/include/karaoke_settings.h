/**
 * @file karaoke_settings.h
 * @brief Persistent configuration manager for master volume and instrument/drum balance.
 *        Survives software reset (via STM32 RTC Backup domain) and power-off shutdown
 *        (via SD card configuration file SD:/karaoke.cfg).
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#ifndef KARAOKE_SETTINGS_H_
#define KARAOKE_SETTINGS_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t master_volume;    /* 0% to 100%, default 80% */
    uint8_t instrument_gain;  /* 50% to 200%, default 100% */
    uint8_t drum_gain;        /* 20% to 200%, default 100% */
    uint8_t melody_gain;      /* 20% to 250%, default 100%, scales the lyric-following channel */
    uint8_t sound_bank;       /* 0 = Yamaha OPL4, 1 = Roland SC-55 */
    uint8_t sensme_channel;   /* 0 = All Songs, 1..12 = SensMe Mood Channel */
} karaoke_settings_t;

/**
 * @brief Initialize settings. Restores values from SD:/karaoke.cfg (shutdown persistence)
 *        or STM32 RTC Backup registers (reset persistence), or applies defaults.
 */
void karaoke_settings_init(void);

/**
 * @brief Get read-only pointer to active settings.
 */
const karaoke_settings_t* karaoke_settings_get(void);

/**
 * @brief Update master volume and apply immediately to audio hardware.
 */
void karaoke_settings_set_volume(uint8_t vol);

/**
 * @brief Update instrument/melody gain (50-200%) and apply immediately to FM synth.
 */
void karaoke_settings_set_instrument_gain(uint8_t gain);

/**
 * @brief Update drum/rhythm gain (50-200%) and apply immediately to FM synth.
 */
void karaoke_settings_set_drum_gain(uint8_t gain);

/**
 * @brief Update the lead melody gain (20-250%) and apply immediately to FM synth.
 */
void karaoke_settings_set_melody_gain(uint8_t gain);

/**
 * @brief Update active sound card bank (0 = Yamaha OPL4, 1 = Roland SC-55).
 */
void karaoke_settings_set_sound_bank(uint8_t bank);

/**
 * @brief Update active SensMe mood channel filter (0 = All Songs, 1..12 = Channel).
 */
void karaoke_settings_set_sensme_channel(uint8_t ch);

/**
 * @brief Save active settings to non-volatile storage (RTC Backup Domain + SD card file).
 *        Called automatically when leaving Settings or adjusting values.
 */
void karaoke_settings_save(void);

#ifdef __cplusplus
}
#endif

#endif /* KARAOKE_SETTINGS_H_ */
