#include "wav_player.h"
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <stdio.h>

/* Double buffer: 4096 samples = 8192 bytes per buffer (16 KB total)
 * Provides ~93 ms of audio latency cushion per buffer against SDIO jitter.
 */
#define WAV_BUF_SAMPLES 4096
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
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (1) {
        k_sem_take(&s_buf_empty_sem, K_FOREVER);
        
        if (!s_is_playing || s_is_paused) {
            k_msleep(10);
            continue;
        }

        /* Scan both buffers and fill whichever is empty */
        for (int i = 0; i < 2; i++) {
            if (s_is_playing && !s_is_paused && s_samples_in_buf[i] == 0) {
                ssize_t bytes_read = fs_read(&s_file, s_buffer[i], sizeof(s_buffer[i]));
                if (bytes_read > 0) {
                    s_samples_in_buf[i] = (uint16_t)(bytes_read / 2);
                } else if (bytes_read == 0) {
                    s_is_playing = false;
                    printk("[WAV] Track playback finished (EOF).\n");
                    break;
                }
            }
        }
    }
}

K_THREAD_DEFINE(wav_thread, 4096, wav_reader_thread_fn, NULL, NULL, NULL, 4, 0, 0);

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

    /* Parse WAV header to locate 'data' chunk */
    uint8_t header[12];
    if (fs_read(&s_file, header, 12) != 12 ||
        memcmp(header, "RIFF", 4) != 0 || memcmp(&header[8], "WAVE", 4) != 0) {
        printk("[WAV] Invalid WAV format!\n");
        fs_close(&s_file);
        return false;
    }

    bool found_data = false;
    while (!found_data) {
        uint8_t chunk_header[8];
        if (fs_read(&s_file, chunk_header, 8) != 8) break;
        
        uint32_t chunk_size = (uint32_t)chunk_header[4] |
                             ((uint32_t)chunk_header[5] << 8) |
                             ((uint32_t)chunk_header[6] << 16) |
                             ((uint32_t)chunk_header[7] << 24);
        
        if (memcmp(chunk_header, "data", 4) == 0) {
            found_data = true;
            break;
        } else {
            /* Skip non-audio chunk (e.g. metadata/artist/tags) */
            fs_seek(&s_file, chunk_size, FS_SEEK_CUR);
        }
    }

    if (!found_data) {
        printk("[WAV] No data chunk found in WAV!\n");
        fs_close(&s_file);
        return false;
    }

    s_play_buf = 0;
    s_play_idx = 0;
    s_samples_in_buf[0] = 0;
    s_samples_in_buf[1] = 0;
    
    s_is_paused = false;
    s_is_playing = true;
    
    /* Pre-fill first buffer synchronously so audio starts with zero delay */
    ssize_t init_read = fs_read(&s_file, s_buffer[0], sizeof(s_buffer[0]));
    if (init_read > 0) {
        s_samples_in_buf[0] = (uint16_t)(init_read / 2);
    }

    /* Signal background reader thread to immediately pre-fill buffer 1 */
    k_sem_give(&s_buf_empty_sem);

    printk("[WAV] Streaming %s over I2S/ES8388 (44.1kHz 16-bit)...\n", full_path);
    return true;
}

void wav_player_stop(void)
{
    if (s_is_playing) {
        s_is_playing = false;
        s_is_paused = false;
        k_msleep(25); /* Allow background thread to exit any pending fs_read */
        fs_close(&s_file);
    }
}

void wav_player_pause(void)
{
    s_is_paused = true;
}

void wav_player_resume(void)
{
    s_is_paused = false;
    /* Kick reader thread in case buffers need refilling */
    k_sem_give(&s_buf_empty_sem);
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

    /* Check if current buffer ran dry */
    if (s_play_idx >= s_samples_in_buf[s_play_buf] || s_samples_in_buf[s_play_buf] == 0) {
        uint8_t next_buf = 1 - s_play_buf;
        if (s_samples_in_buf[next_buf] > 0) {
            /* Switch to the other filled buffer seamlessly */
            s_samples_in_buf[s_play_buf] = 0;
            s_play_buf = next_buf;
            s_play_idx = 0;
            k_sem_give(&s_buf_empty_sem); /* Request background thread fill the emptied buffer */
        } else {
            /* Temporary underflow (card reading slow): re-arm background reader */
            k_sem_give(&s_buf_empty_sem);
            return false;
        }
    }

    *out_sample = s_buffer[s_play_buf][s_play_idx++];

    /* As soon as we cross half of the active buffer, alert reader to start loading the other */
    if (s_play_idx == (WAV_BUF_SAMPLES / 2)) {
        k_sem_give(&s_buf_empty_sem);
    }

    return true;
}
