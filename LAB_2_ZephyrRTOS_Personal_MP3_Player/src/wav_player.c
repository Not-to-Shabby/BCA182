#include "wav_player.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <ff.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "audio_codec_es8388.h"

#include "helix/mp3dec.h"

#define WAV_BUFFER_SAMPLES 8192
#define MP3_INPUT_BYTES 4096
#define MP3_MAX_FRAME_SAMPLES (1152 * 2)
static int16_t s_buffer[2][WAV_BUFFER_SAMPLES];
static volatile uint16_t s_count[2];
static volatile uint16_t s_index;
static volatile uint8_t s_active_buffer;
static volatile bool s_playing;
static volatile bool s_paused;
static uint32_t s_data_remaining;
static uint32_t s_sample_rate = 44100;
static uint32_t s_resample_phase;
static int16_t s_right_sample;
static bool s_right_pending;
static bool s_file_open;
static FIL s_file;
static K_MUTEX_DEFINE(s_file_lock);
static bool s_is_mp3;
static HMP3Decoder s_mp3;
static uint8_t s_mp3_in[MP3_INPUT_BYTES];
static size_t s_mp3_in_len;
static bool s_mp3_eof;
static bool s_mp3_logged;
static uint32_t s_mp3_rate;
static int s_mp3_version;
static uint32_t s_mp3_skipped;
static uint32_t s_mp3_bad_rate;
static uint32_t s_mp3_errors;
static volatile uint32_t s_underruns;
static uint32_t s_max_fill_us;

static uint16_t le16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* The SDMMC driver uses word-wide DMA, so FatFs must never read straight into an odd address. */
static uint8_t s_read_bounce[2048] __attribute__((aligned(4)));

static ssize_t wav_read(void *buffer, size_t length)
{
    size_t total = 0;

    while (total < length) {
        UINT want = (UINT)MIN(length - total, sizeof(s_read_bounce));
        UINT got = 0;
        FRESULT result = f_read(&s_file, s_read_bounce, want, &got);
        if (result != FR_OK) {
            return total ? (ssize_t)total : -(ssize_t)result;
        }
        memcpy((uint8_t *)buffer + total, s_read_bounce, got);
        total += got;
        if (got < want) break;
    }
    return (ssize_t)total;
}

static int wav_seek_relative(off_t offset)
{
    FSIZE_t target = f_tell(&s_file) + offset;
    return f_lseek(&s_file, target) == FR_OK ? 0 : -1;
}

/* Decodes whole frames as interleaved stereo into dst; returns samples written. */
static size_t mp3_fill(int16_t *dst, size_t capacity)
{
    size_t written = 0;

    while (written + MP3_MAX_FRAME_SAMPLES <= capacity) {
        if (s_mp3_in_len < sizeof(s_mp3_in) && !s_mp3_eof) {
            ssize_t got = wav_read(s_mp3_in + s_mp3_in_len, sizeof(s_mp3_in) - s_mp3_in_len);
            if (got > 0) {
                s_mp3_in_len += (size_t)got;
            } else {
                if (got < 0) printk("[MP3] Read error (FatFs result %d)\n", (int)-got);
                s_mp3_eof = true;
            }
        }
        if (s_mp3_in_len == 0) break;

        int sync = MP3FindSyncWord(s_mp3_in, (int)s_mp3_in_len);
        if (sync < 0) {
            s_mp3_in_len = 0;
            if (s_mp3_eof) break;
            continue;
        }
        if (sync > 0) {
            memmove(s_mp3_in, s_mp3_in + sync, s_mp3_in_len - (size_t)sync);
            s_mp3_in_len -= (size_t)sync;
        }

        /* Helix never checks the layer while decoding, so reject false syncs here. */
        MP3FrameInfo fi;
        bool skip = MP3GetNextFrameInfo(s_mp3, &fi, s_mp3_in) != ERR_MP3_NONE;
        if (!skip && s_mp3_rate == 0 && fi.samprate != 44100 && fi.samprate != 48000) {
            if (++s_mp3_bad_rate >= 16) {
                printk("[MP3] Unsupported sample rate %d Hz\n", fi.samprate);
                s_playing = false;
                break;
            }
            skip = true;
        }
        if (!skip && s_mp3_rate != 0 &&
            ((uint32_t)fi.samprate != s_mp3_rate || fi.version != s_mp3_version)) {
            skip = true;
        }
        if (skip) {
            s_mp3_skipped++;
            memmove(s_mp3_in, s_mp3_in + 1, s_mp3_in_len - 1);
            s_mp3_in_len -= 1;
            continue;
        }

        unsigned char *ptr = s_mp3_in;
        int left = (int)s_mp3_in_len;
        int err = MP3Decode(s_mp3, &ptr, &left, dst + written, 0);
        size_t consumed = s_mp3_in_len - (size_t)left;

        if (err == ERR_MP3_NONE) {
            if (s_mp3_rate == 0) {
                s_mp3_rate = (uint32_t)fi.samprate;
                s_mp3_version = fi.version;
            }
            s_sample_rate = s_mp3_rate;
            if (!s_mp3_logged) {
                s_mp3_logged = true;
                printk("[MP3] Stream: %d Hz, %d ch, %d kbps (Helix)\n",
                       fi.samprate, fi.nChans, fi.bitrate / 1000);
            }
            if (fi.nChans == 1) {
                for (int i = fi.outputSamps - 1; i >= 0; i--) {
                    dst[written + 2 * i] = dst[written + 2 * i + 1] = dst[written + i];
                }
                written += (size_t)fi.outputSamps * 2;
            } else {
                written += (size_t)fi.outputSamps;
            }
        } else {
            if (err != ERR_MP3_MAINDATA_UNDERFLOW && s_mp3_errors++ < 8) {
                printk("[MP3] Decode error %d near file offset %u (%u bytes skipped so far)\n",
                       err, (unsigned)(f_tell(&s_file) - s_mp3_in_len), (unsigned)s_mp3_skipped);
            }
            if (err == ERR_MP3_INDATA_UNDERFLOW && s_mp3_eof) break;
            if (err != ERR_MP3_MAINDATA_UNDERFLOW) consumed = 1;
        }

        if (consumed > s_mp3_in_len) consumed = s_mp3_in_len;
        memmove(s_mp3_in, s_mp3_in + consumed, s_mp3_in_len - consumed);
        s_mp3_in_len -= consumed;
    }
    return written;
}

static void wav_reader(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    while (true) {
        k_mutex_lock(&s_file_lock, K_FOREVER);
        if (s_playing && !s_paused && s_file_open) {
            for (int i = 0; i < 2; i++) {
                if (s_count[i] != 0) continue;
                uint32_t t0 = k_cycle_get_32();
                if (s_is_mp3) {
                    s_count[i] = (uint16_t)mp3_fill(s_buffer[i], WAV_BUFFER_SAMPLES);
                } else {
                    size_t bytes = sizeof(s_buffer[i]);
                    if (s_data_remaining < bytes) bytes = s_data_remaining;
                    ssize_t n = bytes ? wav_read(s_buffer[i], bytes) : 0;
                    if (n > 0) {
                        s_count[i] = (uint16_t)(n / sizeof(int16_t));
                        s_data_remaining -= (uint32_t)n;
                    }
                }
                uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32() - t0);
                if (us > s_max_fill_us) s_max_fill_us = us;
            }

            static uint32_t last_report_ms, last_underruns, last_misses;
            bool at_end = s_is_mp3 ? (s_mp3_eof && s_mp3_in_len == 0) : (s_data_remaining == 0);
            if (!at_end && k_uptime_get_32() - last_report_ms >= 2000U) {
                last_report_ms = k_uptime_get_32();
                uint32_t misses = audio_stage_misses();
                if (s_underruns != last_underruns || misses != last_misses) {
                    printk("[AUDIO] underruns +%u, stale DMA halves +%u, slowest fill %u us\n",
                           (unsigned)(s_underruns - last_underruns), (unsigned)(misses - last_misses),
                           (unsigned)s_max_fill_us);
                    last_underruns = s_underruns;
                    last_misses = misses;
                }
                s_max_fill_us = 0;
            }
        }
        k_mutex_unlock(&s_file_lock);
        k_msleep(2);
    }
}
K_THREAD_DEFINE(wav_reader_thread, 8192, wav_reader, NULL, NULL, NULL, 2, 0, 0);

void wav_player_init(void) { memset(&s_file, 0, sizeof(s_file)); }

bool wav_player_start(const char *filepath)
{
    wav_player_stop();
    if (s_file_open) {
        f_close(&s_file);
        s_file_open = false;
    }

    char path[160];
    snprintf(path, sizeof(path), "SD:/%s", filepath);
    printk("[WAV] Opening '%s' (len=%u)...\n", path, (unsigned)strlen(path));
    FRESULT open_result = f_open(&s_file, path, FA_READ);
    if (open_result != FR_OK) {
        printk("[WAV] FatFs open failed: %s (result %d = %s)\n", path, (int)open_result,
               open_result == FR_DISK_ERR ? "DISK_ERR" :
               open_result == FR_NO_FILE ? "NO_FILE" :
               open_result == FR_NO_PATH ? "NO_PATH" :
               open_result == FR_NOT_READY ? "NOT_READY" :
               open_result == FR_INVALID_NAME ? "INVALID_NAME" : "OTHER");
        return false;
    }
    s_file_open = true;
    printk("[WAV] Open OK (file size %u bytes)\n", (unsigned)f_size(&s_file));

    const char *ext = strrchr(filepath, '.');
    if (ext != NULL && strcasecmp(ext, ".mp3") == 0) {
        uint8_t id3[10];
        if (wav_read(id3, sizeof(id3)) == (ssize_t)sizeof(id3) && memcmp(id3, "ID3", 3) == 0) {
            uint32_t tag = ((uint32_t)(id3[6] & 0x7F) << 21) | ((uint32_t)(id3[7] & 0x7F) << 14) |
                           ((uint32_t)(id3[8] & 0x7F) << 7) | (id3[9] & 0x7F);
            f_lseek(&s_file, 10 + tag);
        } else {
            f_lseek(&s_file, 0);
        }
        /* Helix uses static buffers, so init only resets decoder state. */
        s_mp3 = MP3InitDecoder();
        if (s_mp3 == NULL) {
            printk("[MP3] Decoder init failed\n");
            f_close(&s_file);
            s_file_open = false;
            return false;
        }
        s_mp3_in_len = 0;
        s_mp3_eof = false;
        s_mp3_logged = false;
        s_mp3_rate = 0;
        s_mp3_version = 0;
        s_mp3_skipped = 0;
        s_mp3_bad_rate = 0;
        s_mp3_errors = 0;
        s_is_mp3 = true;
        s_count[0] = s_count[1] = 0;
        s_index = 0;
        s_active_buffer = 0;
        s_sample_rate = 44100;
        s_resample_phase = 0;
        s_right_pending = false;
        s_paused = false;
        s_playing = true;
        printk("[MP3] Playing %s\n", path);
        return true;
    }
    s_is_mp3 = false;

    uint8_t riff[12];
    if (wav_read(riff, sizeof(riff)) != sizeof(riff) ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        printk("[WAV] Invalid RIFF header\n");
        f_close(&s_file);
        s_file_open = false;
        return false;
    }
    printk("[WAV] RIFF/WAVE header OK\n");

    bool fmt_found = false, data_found = false;
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0, data_size = 0;
    while (!data_found) {
        uint8_t chunk[8];
        if (wav_read(chunk, sizeof(chunk)) != sizeof(chunk)) {
            printk("[WAV] Chunk read failed/EOF at offset %u\n", (unsigned)f_tell(&s_file));
            break;
        }
        uint32_t size = le32(chunk + 4);
        printk("[WAV] Chunk '%c%c%c%c' size=%u\n",
               chunk[0], chunk[1], chunk[2], chunk[3], size);
        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (size < sizeof(fmt) || wav_read(fmt, sizeof(fmt)) != sizeof(fmt)) {
                printk("[WAV] fmt chunk read failed\n");
                break;
            }
            format = le16(fmt); channels = le16(fmt + 2); rate = le32(fmt + 4); bits = le16(fmt + 14);
            fmt_found = true;
            if (size > sizeof(fmt) && wav_seek_relative(size - sizeof(fmt)) != 0) break;
        } else if (memcmp(chunk, "data", 4) == 0) {
            data_size = size;
            data_found = true;
            break;
        } else if (wav_seek_relative(size) != 0) {
            printk("[WAV] Chunk seek failed at offset %u\n", (unsigned)f_tell(&s_file));
            break;
        }
        if (size & 1U) wav_seek_relative(1);
    }

    if (!fmt_found || !data_found || format != 1 || channels != 2 || bits != 16 ||
        (rate != 44100U && rate != 48000U)) {
        printk("[WAV] Unsupported: fmt=%u channels=%u rate=%u bits=%u\n", format, channels, rate, bits);
        f_close(&s_file);
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
    s_right_pending = false;
    size_t bytes = s_data_remaining < sizeof(s_buffer[0]) ? s_data_remaining : sizeof(s_buffer[0]);
    ssize_t n = bytes ? wav_read(s_buffer[0], bytes) : 0;
    if (n <= 0) {
        printk("[WAV] Buffer pre-fill failed (requested %u, FatFs result %d, file offset %u)\n",
               (unsigned)bytes, (int)-n, (unsigned)f_tell(&s_file));
        f_close(&s_file);
        s_file_open = false;
        return false;
    }
    s_count[0] = (uint16_t)(n / sizeof(int16_t));
    s_data_remaining -= (uint32_t)n;
    printk("[WAV] Pre-fill OK: %u samples, %u bytes remaining\n",
           s_count[0], (unsigned)s_data_remaining);
    s_paused = false;
    s_playing = true;
    printk("[WAV] Playing %s\n", path);
    return true;
}

void wav_player_stop(void)
{
    s_playing = false;
    s_paused = false;
    k_mutex_lock(&s_file_lock, K_FOREVER);
    s_count[0] = s_count[1] = 0;
    if (s_file_open) {
        f_close(&s_file);
        s_file_open = false;
    }
    k_mutex_unlock(&s_file_lock);
}
void wav_player_pause(void) { s_paused = true; }
void wav_player_resume(void) { s_paused = false; }
bool wav_player_is_active(void) { return s_playing && !s_paused; }

static bool wav_player_take_source_sample(int16_t *sample)
{
    if (s_index >= s_count[s_active_buffer]) {
        uint8_t next = 1U - s_active_buffer;
        if (s_count[next] == 0) return false;
        s_count[s_active_buffer] = 0;
        s_active_buffer = next;
        s_index = 0;
    }
    *sample = s_buffer[s_active_buffer][s_index++];
    return true;
}

bool wav_player_get_next_sample(int16_t *sample)
{
    if (!wav_player_is_active()) return false;

    if (s_right_pending) {
        *sample = s_right_sample;
        s_right_pending = false;
        return true;
    }

    int16_t left, right;
    if (!wav_player_take_source_sample(&left) || !wav_player_take_source_sample(&right)) {
        s_underruns++;
        return false;
    }

    /* Drop whole L/R frames so channels stay aligned. */
    if (s_sample_rate == 48000U) {
        s_resample_phase += 48000U - 44100U;
        while (s_resample_phase >= 44100U) {
            s_resample_phase -= 44100U;
            int16_t skipped;
            if (!wav_player_take_source_sample(&skipped) || !wav_player_take_source_sample(&skipped)) break;
        }
    }

    *sample = left;
    s_right_sample = right;
    s_right_pending = true;
    return true;
}
