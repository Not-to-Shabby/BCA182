#include "wav_player.h"
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <stdio.h>

#define WAV_BUFFER_SAMPLES 8192
static int16_t s_buffer[2][WAV_BUFFER_SAMPLES];
static volatile uint16_t s_count[2];
static volatile uint16_t s_index;
static volatile uint8_t s_active_buffer;
static volatile bool s_playing;
static volatile bool s_paused;
static uint32_t s_data_remaining;
static uint32_t s_sample_rate = 44100;
static uint32_t s_resample_phase;
static bool s_file_open;
static struct fs_file_t s_file;
static K_SEM_DEFINE(s_empty, 2, 2);

static uint16_t le16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void wav_reader(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    while (true) {
        k_sem_take(&s_empty, K_FOREVER);
        if (!s_playing || s_paused) {
            k_msleep(5);
            continue;
        }
        for (int i = 0; i < 2; i++) {
            if (s_count[i] != 0) continue;
            size_t bytes = sizeof(s_buffer[i]);
            if (s_data_remaining < bytes) bytes = s_data_remaining;
            ssize_t n = bytes ? fs_read(&s_file, s_buffer[i], bytes) : 0;
            if (n > 0) {
                s_count[i] = (uint16_t)(n / sizeof(int16_t));
                s_data_remaining -= (uint32_t)n;
            }
        }
    }
}
K_THREAD_DEFINE(wav_reader_thread, 4096, wav_reader, NULL, NULL, NULL, 4, 0, 0);

void wav_player_init(void) { fs_file_t_init(&s_file); }

bool wav_player_start(const char *filepath)
{
    wav_player_stop();
    if (s_file_open) {
        fs_close(&s_file);
        s_file_open = false;
    }
    fs_file_t_init(&s_file);

    char path[160];
    snprintf(path, sizeof(path), "/SD:/%s", filepath);
    int open_err = fs_open(&s_file, path, FS_O_READ);
    if (open_err != 0) {
        printk("[WAV] Open failed: %s (err %d)\n", path, open_err);
        return false;
    }
    s_file_open = true;

    uint8_t riff[12];
    if (fs_read(&s_file, riff, sizeof(riff)) != sizeof(riff) ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        printk("[WAV] Invalid RIFF header\n");
        fs_close(&s_file);
        s_file_open = false;
        return false;
    }

    bool fmt_found = false, data_found = false;
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0, data_size = 0;
    while (!data_found) {
        uint8_t chunk[8];
        if (fs_read(&s_file, chunk, sizeof(chunk)) != sizeof(chunk)) break;
        uint32_t size = le32(chunk + 4);
        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (size < sizeof(fmt) || fs_read(&s_file, fmt, sizeof(fmt)) != sizeof(fmt)) break;
            format = le16(fmt); channels = le16(fmt + 2); rate = le32(fmt + 4); bits = le16(fmt + 14);
            fmt_found = true;
            if (size > sizeof(fmt) && fs_seek(&s_file, size - sizeof(fmt), FS_SEEK_CUR) != 0) break;
        } else if (memcmp(chunk, "data", 4) == 0) {
            data_size = size;
            data_found = true;
            break;
        } else if (fs_seek(&s_file, size, FS_SEEK_CUR) != 0) {
            break;
        }
        if (size & 1U) fs_seek(&s_file, 1, FS_SEEK_CUR);
    }

    if (!fmt_found || !data_found || format != 1 || channels != 2 || bits != 16 ||
        (rate != 44100U && rate != 48000U)) {
        printk("[WAV] Unsupported: fmt=%u channels=%u rate=%u bits=%u\n", format, channels, rate, bits);
        fs_close(&s_file);
        s_file_open = false;
        return false;
    }

    printk("[WAV] Metadata: PCM=%u channels=%u rate=%u Hz bits=%u data=%u bytes\n",
           format, channels, rate, bits, data_size);

    s_count[0] = s_count[1] = 0;
    s_index = 0;
    s_active_buffer = 0;
    s_data_remaining = data_size;
    s_sample_rate = rate;
    s_resample_phase = 0;
    size_t bytes = s_data_remaining < sizeof(s_buffer[0]) ? s_data_remaining : sizeof(s_buffer[0]);
    ssize_t n = bytes ? fs_read(&s_file, s_buffer[0], bytes) : 0;
    if (n <= 0) { fs_close(&s_file); s_file_open = false; return false; }
    s_count[0] = (uint16_t)(n / sizeof(int16_t));
    s_data_remaining -= (uint32_t)n;
    s_paused = false;
    s_playing = true;
    k_sem_give(&s_empty);
    printk("[WAV] Playing %s\n", path);
    return true;
}

void wav_player_stop(void)
{
    s_playing = false;
    s_paused = false;
    s_count[0] = s_count[1] = 0;
    k_msleep(5);
    if (s_file_open) {
        fs_close(&s_file);
        s_file_open = false;
    }
}
void wav_player_pause(void) { s_paused = true; }
void wav_player_resume(void) { s_paused = false; k_sem_give(&s_empty); }
bool wav_player_is_active(void) { return s_playing && !s_paused; }

static bool wav_player_take_source_sample(int16_t *sample)
{
    if (s_index >= s_count[s_active_buffer]) {
        uint8_t next = 1U - s_active_buffer;
        if (s_count[next] == 0) { k_sem_give(&s_empty); return false; }
        s_count[s_active_buffer] = 0;
        s_active_buffer = next;
        s_index = 0;
        k_sem_give(&s_empty);
    }
    *sample = s_buffer[s_active_buffer][s_index++];
    if (s_index == WAV_BUFFER_SAMPLES / 2) k_sem_give(&s_empty);
    return true;
}

bool wav_player_get_next_sample(int16_t *sample)
{
    if (!wav_player_is_active() || !wav_player_take_source_sample(sample)) return false;

    if (s_sample_rate == 48000U) {
        s_resample_phase += 48000U;
        while (s_resample_phase >= 44100U) {
            s_resample_phase -= 44100U;
            int16_t discarded;
            if (!wav_player_take_source_sample(&discarded)) break;
        }
    }
    return true;
}
