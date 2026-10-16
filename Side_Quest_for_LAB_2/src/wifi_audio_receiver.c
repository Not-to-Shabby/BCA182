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
static volatile uint32_t s_stat_frames = 0;     /* valid audio frames received */
static volatile uint32_t s_stat_bad = 0;        /* frames dropped for a bad checksum */
static volatile uint32_t s_stat_underruns = 0;  /* times the DAC ran out of queued audio */
static volatile uint32_t s_stat_overrun = 0;    /* UART overrun errors: a byte arrived before the last was read */
static volatile uint32_t s_stat_fifo_full = 0;  /* bytes dropped because the receive FIFO was full */
static volatile uint32_t s_stat_overflow = 0;   /* samples dropped because the ring was full */
static int16_t s_peak_l = 0;
static int16_t s_peak_r = 0;

static void wifi_audio_rx_byte(uint8_t b);

#if defined(__arm__)

/* The F4 USART has no RX FIFO: at 2 Mbps a byte must be read within about 5 us. The interrupt is
 * zero-latency (it runs even while the LED driver or synth hold irq_lock) and so may not call any
 * kernel API; it only stores the byte, and a thread parses the frames. */
#define UART_RX_FIFO_SIZE   2048U
#define RX_THREAD_PRIORITY  2
static volatile uint8_t s_uart_rx_fifo[UART_RX_FIFO_SIZE];
static volatile uint32_t s_uart_rx_head = 0;
static volatile uint32_t s_uart_rx_tail = 0;
K_THREAD_STACK_DEFINE(s_rx_stack, 1024);
static struct k_thread s_rx_thread_data;

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

ISR_DIRECT_DECLARE(usart2_isr)
{
    uint32_t sr = USART2->SR;
    while (sr & (USART_SR_RXNE | USART_SR_ORE)) {
        if (sr & USART_SR_ORE) {
            s_stat_overrun++;
        }
        /* Reading DR clears both RXNE and ORE. */
        uint8_t b = (uint8_t)USART2->DR;
        uint32_t next = (s_uart_rx_head + 1U) % UART_RX_FIFO_SIZE;
        if (next != s_uart_rx_tail) {
            s_uart_rx_fifo[s_uart_rx_head] = b;
            s_uart_rx_head = next;
        } else {
            s_stat_fifo_full++;
        }
        sr = USART2->SR;
    }
    return 0;
}

static void rx_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    while (true) {
        while (s_uart_rx_head != s_uart_rx_tail) {
            uint8_t byte = s_uart_rx_fifo[s_uart_rx_tail];
            s_uart_rx_tail = (s_uart_rx_tail + 1U) % UART_RX_FIFO_SIZE;
            wifi_audio_rx_byte(byte);
        }
        k_msleep(1);
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

    k_thread_create(&s_rx_thread_data, s_rx_stack, K_THREAD_STACK_SIZEOF(s_rx_stack),
                    rx_thread, NULL, NULL, NULL, RX_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&s_rx_thread_data, "wifi_rx");

    IRQ_DIRECT_CONNECT(USART2_IRQn, 0, usart2_isr, IRQ_ZERO_LATENCY);
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
                s_stat_underruns++;
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
    static int64_t last_report = 0;
    static bool was_streaming = false;

    bool streaming = wifi_audio_receiver_is_streaming();
    if (streaming != was_streaming) {
        was_streaming = streaming;
        printk("[WiFi Audio] Stream %s (title '%s')\n", streaming ? "started" : "stopped", s_meta.title);
    }
    if (streaming && k_uptime_get() - last_report >= 2000) {
        static uint32_t last_frames = 0;
        last_report = k_uptime_get();
        uint32_t frames = s_stat_frames;
        uint32_t fill = (s_pcm_head + PCM_RING_SAMPLES - s_pcm_tail) % PCM_RING_SAMPLES / 2U;
        printk("[WiFi Audio] %u good frames/2s (need ~689), queued %u, underruns %u, bad %u, "
               "overrun %u, fifo-full %u, ring-overflow %u\n",
               (unsigned)(frames - last_frames), (unsigned)fill, (unsigned)s_stat_underruns,
               (unsigned)s_stat_bad, (unsigned)s_stat_overrun, (unsigned)s_stat_fifo_full,
               (unsigned)s_stat_overflow);
        last_frames = frames;
    }

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
                s_stat_frames++;
                /* Push raw 16-bit PCM block into audio ring */
                const int16_t *pcm = (const int16_t *)payload;
                uint16_t sample_count = msg_len / 2U;
                for (uint16_t i = 0; i < sample_count; i++) {
                    uint32_t next = (s_pcm_head + 1U) % PCM_RING_SAMPLES;
                    if (next != s_pcm_tail) {
                        s_pcm_ring[s_pcm_head] = pcm[i];
                        s_pcm_head = next;
                    } else {
                        s_stat_overflow++;
                    }
                }
            } else if (msg_type == WIFI_MSG_TRACK_META && msg_len >= sizeof(wifi_track_meta_t)) {
                memcpy(&s_meta, payload, sizeof(s_meta));
            } else if (msg_type == WIFI_MSG_TELEMETRY && msg_len >= sizeof(wifi_telemetry_t)) {
                memcpy(&s_telem, payload, sizeof(s_telem));
            }
        } else {
            s_stat_bad++;
        }
        state = 0;
        break;
    }
    default:
        state = 0;
        break;
    }
}
