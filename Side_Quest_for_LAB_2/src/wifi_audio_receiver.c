/**
 * @file wifi_audio_receiver.c
 * @brief High-speed UART2 DMA / IRQ receiver for incoming Wi-Fi audio stream from ESP32-S3.
 */

#include "wifi_audio_receiver.h"
#include <string.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#if defined(__arm__)
#include <stm32f4xx.h>
#endif

#define PCM_RING_FRAMES     (1024U)
#define PCM_RING_SAMPLES    (PCM_RING_FRAMES * 2U)
#define PCM_PRIME_SAMPLES   (512U * 2U)

static int16_t s_pcm_ring[PCM_RING_SAMPLES];
static volatile uint32_t s_pcm_head = 0;
static volatile uint32_t s_pcm_tail = 0;

static wifi_track_meta_t s_meta = {
    .title = "Ready to Stream",
    .artist = "Connect phone to Wi-Fi",
    .album = "RT-Spark Wireless",
    .format = "PCM 16-bit 44.1k",
    .duration_ms = 0,
    .position_ms = 0
};

static wifi_telemetry_t s_telem = {
    .wifi_mode = WIFI_STATE_IDLE,
    .ip_addr = "192.168.4.1",
    .rssi_dbm = 0,
    .stream_state = STREAM_STATE_STOPPED,
    .volume_percent = 80
};

static bool s_esp32_connected = false;
static int64_t s_last_ping_time = 0;
static int64_t s_last_audio_time = 0;
static int16_t s_peak_l = 0;
static int16_t s_peak_r = 0;

static void wifi_audio_rx_byte(uint8_t b);

#if defined(__arm__)

static void usart2_send_byte(uint8_t b)
{
    while (!(USART2->SR & USART_SR_TXE)) {
    }
    USART2->DR = b;
}

static void usart2_send_packet(uint8_t type, const uint8_t *payload, uint16_t len)
{
    uint8_t hdr[5];
    hdr[0] = WIFI_PROTO_SYNC1;
    hdr[1] = WIFI_PROTO_SYNC2;
    hdr[2] = type;
    hdr[3] = (uint8_t)(len & 0xFFU);
    hdr[4] = (uint8_t)((len >> 8) & 0xFFU);
    uint8_t cks = wifi_protocol_checksum(type, len, payload);

    for (int i = 0; i < 5; i++) {
        usart2_send_byte(hdr[i]);
    }
    if (payload != NULL && len > 0) {
        for (uint16_t i = 0; i < len; i++) {
            usart2_send_byte(payload[i]);
        }
    }
    usart2_send_byte(cks);
}

static void usart2_isr(const void *arg)
{
    ARG_UNUSED(arg);
    uint32_t sr = USART2->SR;
    while (sr & (USART_SR_RXNE | USART_SR_ORE)) {
        /* Reading DR clears both RXNE and ORE. At 178 KB/s the stream cannot wait for a polling thread. */
        wifi_audio_rx_byte((uint8_t)USART2->DR);
        sr = USART2->SR;
    }
}

int wifi_audio_receiver_init(void)
{
    /* 1. Clocks: GPIOA, USART2 */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;

    /* 2. Configure PA2 (TX) and PA3 (RX) as AF7 */
    GPIOA->MODER &= ~((3U << (2 * 2)) | (3U << (3 * 2)));
    GPIOA->MODER |=  ((2U << (2 * 2)) | (2U << (3 * 2)));
    GPIOA->OSPEEDR |= ((3U << (2 * 2)) | (3U << (3 * 2)));
    GPIOA->PUPDR &= ~((3U << (2 * 2)) | (3U << (3 * 2)));
    GPIOA->PUPDR |=  (1U << (3 * 2)); /* Pull-up on RX */

    GPIOA->AFR[0] &= ~((0xFU << (2 * 4)) | (0xFU << (3 * 4)));
    GPIOA->AFR[0] |=  ((7U << (2 * 4)) | (7U << (3 * 4))); /* AF7 */

    /* 3. USART2 configuration: 2.0 Mbps, 8N1
     * APB1 is 30 MHz. With OVER8 = 1, USARTDIV = 30 MHz / (8 * 2 Mbps) = 1.875:
     * mantissa 1, fraction 0.875 * 8 = 7 (BRR[3] must stay clear), so BRR = 0x17.
     */
    USART2->CR1 = 0;
    USART2->CR2 = 0;
    USART2->CR3 = 0;
    USART2->CR1 |= USART_CR1_OVER8;
    USART2->BRR = 0x17;

    USART2->CR1 |= (USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE);
    USART2->CR1 |= USART_CR1_UE;

    IRQ_CONNECT(USART2_IRQn, 4, usart2_isr, NULL, 0);
    irq_enable(USART2_IRQn);

    printk("[WiFi Audio] USART2 Link Initialized @ 2.0 Mbps (PA2 TX / PA3 RX)\n");
    return 0;
}

#else
int wifi_audio_receiver_init(void) { return 0; }
static void usart2_send_packet(uint8_t type, const uint8_t *payload, uint16_t len) { ARG_UNUSED(type); ARG_UNUSED(payload); ARG_UNUSED(len); }
#endif

bool wifi_audio_receiver_is_connected(void)
{
    return s_esp32_connected;
}

bool wifi_audio_receiver_is_streaming(void)
{
    return (s_esp32_connected && (k_uptime_get() - s_last_audio_time < 1500));
}

void wifi_audio_receiver_get_metadata(wifi_track_meta_t *out)
{
    if (out) {
        *out = s_meta;
    }
}

void wifi_audio_receiver_get_telemetry(wifi_telemetry_t *out)
{
    if (out) {
        *out = s_telem;
    }
}

void wifi_audio_send_command(uint8_t cmd)
{
    uint8_t payload = cmd;
    usart2_send_packet(WIFI_CMD_BUTTON, &payload, 1);
}

void wifi_audio_send_wifi_config(uint8_t mode, const char *ssid, const char *pass)
{
    uint8_t buf[128];
    buf[0] = mode;
    uint16_t len = 1;
    if (ssid != NULL) {
        strncpy((char *)(buf + 1), ssid, 32);
        len += 32;
    }
    if (pass != NULL) {
        strncpy((char *)(buf + 33), pass, 64);
        len += strlen(pass);
    }
    usart2_send_packet(WIFI_CMD_SET_WIFI, buf, len);
}

void wifi_audio_render(int16_t *out_pcm, size_t frames)
{
    if (out_pcm == NULL || frames == 0) return;

    size_t samples = frames * 2U;
    int16_t pl = 0;
    int16_t pr = 0;
    static bool primed = false;

    /* Hold playback until about 12 ms is queued so Wi-Fi jitter does not chop the start of every burst. */
    uint32_t fill = (s_pcm_head + PCM_RING_SAMPLES - s_pcm_tail) % PCM_RING_SAMPLES;
    if (!primed && fill >= PCM_PRIME_SAMPLES) {
        primed = true;
    }

    for (size_t i = 0; i < samples; i += 2) {
        if (primed && s_pcm_head != s_pcm_tail) {
            int16_t l = s_pcm_ring[s_pcm_tail];
            int16_t r = s_pcm_ring[(s_pcm_tail + 1U) % PCM_RING_SAMPLES];
            s_pcm_tail = (s_pcm_tail + 2U) % PCM_RING_SAMPLES;

            out_pcm[i] = l;
            out_pcm[i + 1] = r;

            if (l > pl) pl = l;
            if (r > pr) pr = r;
        } else {
            if (primed) {
                primed = false;
            }
            out_pcm[i] = 0;
            out_pcm[i + 1] = 0;
        }
    }
    s_peak_l = pl;
    s_peak_r = pr;
}

void wifi_audio_get_peaks(int16_t *peak_l, int16_t *peak_r)
{
    if (peak_l) *peak_l = s_peak_l;
    if (peak_r) *peak_r = s_peak_r;
}

void wifi_audio_process(void)
{
    if (s_esp32_connected && (k_uptime_get() - s_last_ping_time > 4000)) {
        s_esp32_connected = false;
    }
}

static void wifi_audio_rx_byte(uint8_t b)
{
    static uint8_t state = 0;
    static uint8_t msg_type = 0;
    static uint16_t msg_len = 0;
    static uint8_t payload[520];
    static uint16_t rx_idx = 0;

    switch (state) {
    case 0:
        if (b == WIFI_PROTO_SYNC1) state = 1;
        break;
    case 1:
        if (b == WIFI_PROTO_SYNC2) state = 2;
        else state = 0;
        break;
    case 2:
        msg_type = b;
        state = 3;
        break;
    case 3:
        msg_len = b;
        state = 4;
        break;
    case 4:
        msg_len |= ((uint16_t)b << 8);
        if (msg_len > sizeof(payload)) {
            state = 0;
        } else if (msg_len == 0) {
            state = 6;
        } else {
            rx_idx = 0;
            state = 5;
        }
        break;
    case 5:
        payload[rx_idx++] = b;
        if (rx_idx >= msg_len) {
            state = 6;
        }
        break;
    case 6: {
        uint8_t cks = wifi_protocol_checksum(msg_type, msg_len, payload);
        if (cks == b) {
            s_esp32_connected = true;
            s_last_ping_time = k_uptime_get();

            if (msg_type == WIFI_MSG_PING) {
                /* Respond with PONG handshake */
                usart2_send_packet(WIFI_MSG_PONG, NULL, 0);
            } else if (msg_type == WIFI_MSG_AUDIO_PCM) {
                s_last_audio_time = k_uptime_get();
                /* Push raw 16-bit PCM block into audio ring */
                const int16_t *pcm = (const int16_t *)payload;
                uint16_t sample_count = msg_len / 2U;
                for (uint16_t i = 0; i < sample_count; i++) {
                    uint32_t next = (s_pcm_head + 1U) % PCM_RING_SAMPLES;
                    if (next != s_pcm_tail) {
                        s_pcm_ring[s_pcm_head] = pcm[i];
                        s_pcm_head = next;
                    }
                }
            } else if (msg_type == WIFI_MSG_TRACK_META && msg_len >= sizeof(wifi_track_meta_t)) {
                memcpy(&s_meta, payload, sizeof(s_meta));
            } else if (msg_type == WIFI_MSG_TELEMETRY && msg_len >= sizeof(wifi_telemetry_t)) {
                memcpy(&s_telem, payload, sizeof(s_telem));
            }
        }
        state = 0;
        break;
    }
    default:
        state = 0;
        break;
    }
}
