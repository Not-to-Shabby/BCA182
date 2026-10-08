/**
 * @file karaoke_settings.c
 * @brief Persistent configuration manager for master volume and instrument/drum balance.
 *        Survives software reset (via STM32 RTC Backup domain) and power-off shutdown
 *        (via SD card configuration file SD:/karaoke.cfg).
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include "karaoke_settings.h"
#include "audio_codec_es8388.h"
#include "yamaha_fm_synth.h"
#include "sd_card_reader.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <ff.h>

#if defined(__arm__)
#include <stm32f4xx.h>
#endif

#define SETTINGS_MAGIC          0x4B415241U /* "KARA" in ASCII */
#define SETTINGS_CFG_PATH       "SD:/karaoke.cfg"

static karaoke_settings_t s_settings = {
    .master_volume = 80,
    .instrument_gain = 100,
    .drum_gain = 100
};

static void apply_hardware_settings(void)
{
    audio_set_volume(s_settings.master_volume);
    yamaha_fm_set_instrument_gain(s_settings.instrument_gain);
    yamaha_fm_set_drum_gain(s_settings.drum_gain);
}

static void rtc_save_backup(void)
{
#if defined(__arm__)
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    PWR->CR |= PWR_CR_DBP;

    uint32_t packed = (uint32_t)s_settings.master_volume |
                      ((uint32_t)s_settings.instrument_gain << 8) |
                      ((uint32_t)s_settings.drum_gain << 16);

    RTC->BKP0R = SETTINGS_MAGIC;
    RTC->BKP1R = packed;
#endif
}

static bool rtc_load_backup(void)
{
#if defined(__arm__)
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    PWR->CR |= PWR_CR_DBP;

    if (RTC->BKP0R == SETTINGS_MAGIC) {
        uint32_t packed = RTC->BKP1R;
        uint8_t vol  = (uint8_t)(packed & 0xFFU);
        uint8_t inst = (uint8_t)((packed >> 8) & 0xFFU);
        uint8_t drum = (uint8_t)((packed >> 16) & 0xFFU);

        if (vol <= 100 && inst >= 20 && inst <= 200 && drum >= 20 && drum <= 200) {
            s_settings.master_volume = vol;
            s_settings.instrument_gain = inst;
            s_settings.drum_gain = drum;
            printk("[Settings] Restored from RTC Backup: Vol=%u%% Inst=%u%% Drum=%u%%\n",
                   vol, inst, drum);
            return true;
        }
    }
#endif
    return false;
}

static bool sd_load_cfg(void)
{
    if (!sd_card_is_mounted()) {
        return false;
    }

    FIL f;
    if (f_open(&f, SETTINGS_CFG_PATH, FA_READ) != FR_OK) {
        return false;
    }

    char buf[128];
    UINT br = 0;
    FRESULT fr = f_read(&f, buf, sizeof(buf) - 1, &br);
    f_close(&f);

    if (fr != FR_OK || br == 0) {
        return false;
    }
    buf[br] = '\0';

    int vol = -1, inst = -1, drum = -1;
    char *line = strtok(buf, "\r\n");
    while (line != NULL) {
        if (strncmp(line, "VOL=", 4) == 0) {
            vol = atoi(line + 4);
        } else if (strncmp(line, "INST=", 5) == 0) {
            inst = atoi(line + 5);
        } else if (strncmp(line, "DRUM=", 5) == 0) {
            drum = atoi(line + 5);
        }
        line = strtok(NULL, "\r\n");
    }

    bool updated = false;
    if (vol >= 0 && vol <= 100) {
        s_settings.master_volume = (uint8_t)vol;
        updated = true;
    }
    if (inst >= 20 && inst <= 200) {
        s_settings.instrument_gain = (uint8_t)inst;
        updated = true;
    }
    if (drum >= 20 && drum <= 200) {
        s_settings.drum_gain = (uint8_t)drum;
        updated = true;
    }

    if (updated) {
        printk("[Settings] Restored from " SETTINGS_CFG_PATH ": Vol=%u%% Inst=%u%% Drum=%u%%\n",
               s_settings.master_volume, s_settings.instrument_gain, s_settings.drum_gain);
    }
    return updated;
}

static void sd_save_cfg(void)
{
    if (!sd_card_is_mounted()) {
        return;
    }

    FIL f;
    if (f_open(&f, SETTINGS_CFG_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        return;
    }

    char buf[96];
    int len = snprintf(buf, sizeof(buf),
                       "# RT-Spark Karaoke Audio Settings\n"
                       "VOL=%u\n"
                       "INST=%u\n"
                       "DRUM=%u\n",
                       s_settings.master_volume,
                       s_settings.instrument_gain,
                       s_settings.drum_gain);

    if (len > 0) {
        UINT bw = 0;
        f_write(&f, buf, (UINT)len, &bw);
    }
    f_close(&f);
    printk("[Settings] Saved to " SETTINGS_CFG_PATH "\n");
}

void karaoke_settings_init(void)
{
    /* 1. Try restore from RTC Backup register (persists across warm reset) */
    bool from_rtc = rtc_load_backup();

    /* 2. Try restore from SD card file (persists across complete power shutdown) */
    bool from_sd = sd_load_cfg();

    if (!from_rtc && !from_sd) {
        printk("[Settings] Using defaults: Vol=%u%% Inst=%u%% Drum=%u%%\n",
               s_settings.master_volume, s_settings.instrument_gain, s_settings.drum_gain);
    }

    apply_hardware_settings();
}

const karaoke_settings_t* karaoke_settings_get(void)
{
    return &s_settings;
}

void karaoke_settings_set_volume(uint8_t vol)
{
    if (vol > 100) vol = 100;
    s_settings.master_volume = vol;
    audio_set_volume(vol);
    rtc_save_backup();
}

void karaoke_settings_set_instrument_gain(uint8_t gain)
{
    if (gain < 20) gain = 20;
    if (gain > 200) gain = 200;
    s_settings.instrument_gain = gain;
    yamaha_fm_set_instrument_gain(gain);
    rtc_save_backup();
}

void karaoke_settings_set_drum_gain(uint8_t gain)
{
    if (gain < 20) gain = 20;
    if (gain > 200) gain = 200;
    s_settings.drum_gain = gain;
    yamaha_fm_set_drum_gain(gain);
    rtc_save_backup();
}

void karaoke_settings_save(void)
{
    rtc_save_backup();
    sd_save_cfg();
}
