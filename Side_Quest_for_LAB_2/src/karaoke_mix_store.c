/**
 * @file karaoke_mix_store.c
 * @brief FatFs storage for the mixer files, see karaoke_mixer.h.
 *
 * Course: BCA182 Embedded Systems Programming
 * Side Quest for Laboratory Activity 2: MIDI Karaoke Player
 */

#include "karaoke_mixer.h"
#include "sd_card_reader.h"

#include <string.h>
#include <ff.h>

bool mix_store_read(const char *path, char *buf, size_t size, size_t *len)
{
    FIL f;
    UINT br = 0;
    FRESULT fr;

    *len = 0;
    if (!sd_card_is_mounted()) {
        return false;
    }
    sd_card_lock();
    fr = f_open(&f, path, FA_READ);
    if (fr == FR_OK) {
        fr = f_read(&f, buf, (UINT)size, &br);
        f_close(&f);
    }
    sd_card_unlock();
    if (fr != FR_OK) {
        return false;
    }
    *len = br;
    return true;
}

bool mix_store_write(const char *path, const char *buf, size_t len)
{
    FIL f;
    UINT bw = 0;
    FRESULT fr;

    if (!sd_card_is_mounted()) {
        return false;
    }
    sd_card_lock();
    (void)f_mkdir("SD:/mix");           /* already there is fine */
    fr = f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr == FR_OK) {
        fr = f_write(&f, buf, (UINT)len, &bw);
        f_close(&f);
    }
    sd_card_unlock();
    return fr == FR_OK && bw == len;
}

void mix_store_delete(const char *path)
{
    if (!sd_card_is_mounted()) {
        return;
    }
    sd_card_lock();
    (void)f_unlink(path);
    sd_card_unlock();
}
