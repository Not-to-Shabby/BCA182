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

        /* Find an empty buffer and fill it */
        for (int i = 0; i < 2; i++) {
            if (s_samples_in_buf[i] == 0) {
                ssize_t bytes_read = fs_read(&s_file, s_buffer[i], WAV_BUF_SAMPLES * 2);
                if (bytes_read > 0) {
                    s_samples_in_buf[i] = bytes_read / 2;
                } else if (bytes_read == 0) {
                    s_is_playing = false;
                    fs_close(&s_file);
                    printk("[WAV] End of file reached.\n");
                    break;
                }
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

    fs_close(&s_file);
    fs_file_t_init(&s_file);

    char full_path[280];
    snprintf(full_path, sizeof(full_path), "/SD:/%s", filepath);
    int err = fs_open(&s_file, full_path, FS_O_READ);
    if (err != 0) {
        printk("[WAV] Failed to open '%s' (err %d)\n", full_path, err);
        return false;
    }
    printk("[WAV] Successfully opened '%s'\n", full_path);

    /* Parse WAV header to find data chunk */
    uint8_t header[12];
    fs_read(&s_file, header, 12);
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(&header[8], "WAVE", 4) != 0) {
        printk("[WAV] Invalid WAV format!\n");
        fs_close(&s_file);
        return false;
    }

    bool found_data = false;
    while (!found_data) {
        uint8_t chunk_header[8];
        if (fs_read(&s_file, chunk_header, 8) != 8) break;
        
        uint32_t chunk_size = chunk_header[4] | (chunk_header[5] << 8) | (chunk_header[6] << 16) | (chunk_header[7] << 24);
        
        if (memcmp(chunk_header, "data", 4) == 0) {
            found_data = true;
            break;
        } else {
            /* Skip this chunk */
            fs_seek(&s_file, chunk_size, FS_SEEK_CUR);
        }
    }

    if (!found_data) {
        printk("[WAV] No data chunk found!\n");
        fs_close(&s_file);
        return false;
    }

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
