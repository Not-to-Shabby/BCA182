/**
 * @file audio_receiver.c
 * @brief High-throughput network audio socket receiver & ring buffer for ESP32-S3.
 */

#include "audio_receiver.h"
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/ring_buffer.h>
#include <string.h>
#include <stdio.h>

#define UDP_AUDIO_PORT          44100
#define TCP_STREAM_PORT         8080
#define JITTER_BUF_SIZE         (32U * 1024U)

static uint8_t s_jitter_raw[JITTER_BUF_SIZE];
static struct ring_buf s_jitter_ring;

static audio_chunk_cb_t s_pcm_cb = NULL;
static meta_update_cb_t s_meta_cb = NULL;

static wifi_track_meta_t s_meta = {
    .title = "Ready to Stream",
    .artist = "Connect phone to Wi-Fi",
    .album = "RT-Spark Wireless",
    .format = "PCM 16-bit 44.1k",
    .duration_ms = 0,
    .position_ms = 0
};

#define AUDIO_THREAD_STACK_SIZE 4096
#define AUDIO_THREAD_PRIORITY   5

static K_THREAD_STACK_DEFINE(s_udp_stack, AUDIO_THREAD_STACK_SIZE);
static struct k_thread s_udp_thread_data;

static void udp_receiver_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        printk("[Audio] Failed to create UDP socket: %d\n", errno);
        return;
    }

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(UDP_AUDIO_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        printk("[Audio] Failed to bind UDP socket to port %d: %d\n", UDP_AUDIO_PORT, errno);
        close(sock);
        return;
    }

    printk("[Audio] UDP streaming server listening on port %d (44.1 kHz 16-bit Stereo PCM)\n", UDP_AUDIO_PORT);

    static uint8_t packet[1024];
    while (1) {
        ssize_t received = recv(sock, packet, sizeof(packet), 0);
        if (received > 0) {
            /* Check if packet is metadata packet: magic "META" */
            if (received >= 4 && memcmp(packet, "META", 4) == 0 && received >= (4 + sizeof(wifi_track_meta_t))) {
                memcpy(&s_meta, packet + 4, sizeof(s_meta));
                if (s_meta_cb) {
                    s_meta_cb(&s_meta);
                }
            } else {
                /* Raw PCM Audio Block */
                ring_buf_put(&s_jitter_ring, packet, received);
            }
        }

        /* Drain jitter ring into 512-byte PCM chunks */
        uint8_t chunk[AUDIO_CHUNK_BYTES];
        while (ring_buf_get(&s_jitter_ring, chunk, sizeof(chunk)) == sizeof(chunk)) {
            if (s_pcm_cb) {
                s_pcm_cb(chunk, sizeof(chunk));
            }
        }
    }
}

int audio_receiver_init(audio_chunk_cb_t pcm_cb, meta_update_cb_t meta_cb)
{
    s_pcm_cb = pcm_cb;
    s_meta_cb = meta_cb;
    ring_buf_init(&s_jitter_ring, sizeof(s_jitter_raw), s_jitter_raw);

    k_thread_create(&s_udp_thread_data, s_udp_stack,
                    K_THREAD_STACK_SIZEOF(s_udp_stack),
                    udp_receiver_thread,
                    NULL, NULL, NULL,
                    AUDIO_THREAD_PRIORITY, 0, K_NO_WAIT);

    return 0;
}

void audio_receiver_handle_command(uint8_t cmd)
{
    switch (cmd) {
    case BTN_CMD_PLAY_PAUSE:
        printk("[Audio] Command from Spark: Play/Pause\n");
        break;
    case BTN_CMD_NEXT:
        printk("[Audio] Command from Spark: Next Track\n");
        break;
    case BTN_CMD_PREV:
        printk("[Audio] Command from Spark: Prev Track\n");
        break;
    case BTN_CMD_VOL_UP:
        printk("[Audio] Command from Spark: Volume Up\n");
        break;
    case BTN_CMD_VOL_DOWN:
        printk("[Audio] Command from Spark: Volume Down\n");
        break;
    default:
        break;
    }
}

void audio_receiver_get_meta(wifi_track_meta_t *out)
{
    if (out) {
        *out = s_meta;
    }
}
