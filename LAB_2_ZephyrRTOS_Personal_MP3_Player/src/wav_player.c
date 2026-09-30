#include "wav_player.h"
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <stdio.h>

#define WAV_BUF_SAMPLES 2048 // 4096 bytes per buffer
static int16_t s_buffer[2][WAV_BUF_SAMPLES];
static volatile uint16_t s_play_idx = 0;
static volatile uint8_t s_play_buf = 0;
static volatile uint16_t s_samples_in_buf[2] = {0, 0};

static K_SEM_DEFINE(s_buf_empty_sem, 2, 2);
static struct fs_file_t s_file;
static volatile bool s_is_playing = false;
static volatile bool s_is_paused = false;

static void wav_reader_thread_fn(void *p1, void *p2, void *p3)
{
    while (1) {
        k_sem_take(&s_buf_empty_sem, K_FOREVER);
        
        if (!s_is_playing) {
            continue;
        }

        uint8_t fill_buf = 1 - s_play_buf;
        if (s_samples_in_buf[fill_buf] == 0) {
            ssize_t bytes_read = fs_read(&s_file, s_buffer[fill_buf], WAV_BUF_SAMPLES * 2);
            if (bytes_read > 0) {
                s_samples_in_buf[fill_buf] = bytes_read / 2;
            } else {
                s_is_playing = false;
                fs_close(&s_file);
                printk("[WAV] End of file reached.\n");
            }
        }
    }
}

K_THREAD_DEFINE(wav_thread, 2048, wav_reader_thread_fn, NULL, NULL, NULL, 4, 0, 0);

void wav_player_init(void)
{
    fs_file_t_init(&s_file);
}

bool wav_player_start(const char* filepath)
{
    if (s_is_playing) {
        wav_player_stop();
    }

    char full_path[64];
    snprintf(full_path, sizeof(full_path), "/SD:/%s", filepath);

    if (fs_open(&s_file, full_path, FS_O_READ) != 0) {
        printk("[WAV] Failed to open %s\n", full_path);
        return false;
    }

    /* Skip 44-byte WAV header roughly */
    fs_seek(&s_file, 44, FS_SEEK_SET);

    s_play_buf = 0;
    s_play_idx = 0;
    s_samples_in_buf[0] = 0;
    s_samples_in_buf[1] = 0;
    
    s_is_paused = false;
    s_is_playing = true;
    
    k_sem_give(&s_buf_empty_sem);
    k_sem_give(&s_buf_empty_sem);

    printk("[WAV] Playing %s...\n", full_path);
    return true;
}

void wav_player_stop(void)
{
    s_is_playing = false;
    fs_close(&s_file);
}

void wav_player_pause(void)
{
    s_is_paused = true;
}

void wav_player_resume(void)
{
    s_is_paused = false;
}

bool wav_player_is_active(void)
{
    return s_is_playing && !s_is_paused;
}

bool wav_player_get_next_sample(int16_t *out_sample)
{
    if (!s_is_playing || s_is_paused) {
        return false;
    }

    if (s_samples_in_buf[s_play_buf] == 0) {
        /* Underflow */
        return false;
    }

    *out_sample = s_buffer[s_play_buf][s_play_idx++];

    if (s_play_idx >= s_samples_in_buf[s_play_buf]) {
        s_samples_in_buf[s_play_buf] = 0; /* Mark empty */
        s_play_buf = 1 - s_play_buf;      /* Swap */
        s_play_idx = 0;
        k_sem_give(&s_buf_empty_sem);     /* Trigger read */
    }

    return true;
}
