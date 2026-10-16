/**
 * @file protocol_bridge.c
 * @brief High-Speed 2.0 Mbps UART DMA / Interrupt Communication Bridge with RT-Spark.
 */

#include "wifi_protocol.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <string.h>

#define UART_NODE DT_CHOSEN(spark_comm_uart)

static const struct device *s_uart_dev = DEVICE_DT_GET(UART_NODE);

#define RX_BUF_SIZE 256
static uint8_t s_rx_ring_data[RX_BUF_SIZE];
static struct ring_buf s_rx_ring;

static bool s_spark_detected = false;
static int64_t s_last_pong_time = 0;

typedef void (*cmd_received_cb_t)(uint8_t cmd_type, const uint8_t *payload, uint16_t len);
static cmd_received_cb_t s_cmd_cb = NULL;

static void uart_rx_isr(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);
    while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
        uint8_t byte;
        if (uart_fifo_read(dev, &byte, 1) == 1) {
            ring_buf_put(&s_rx_ring, &byte, 1);
        }
    }
}

int protocol_bridge_init(cmd_received_cb_t cb)
{
    s_cmd_cb = cb;
    ring_buf_init(&s_rx_ring, sizeof(s_rx_ring_data), s_rx_ring_data);

    if (!device_is_ready(s_uart_dev)) {
        printk("[Bridge] UART device not ready!\n");
        return -ENODEV;
    }

    uart_irq_callback_set(s_uart_dev, uart_rx_isr);
    uart_irq_rx_enable(s_uart_dev);
    printk("[Bridge] Initialized UART link to RT-Spark @ 2.0 Mbps (GPIO17 TX / GPIO18 RX)\n");
    return 0;
}

void protocol_bridge_send_frame(uint8_t type, const uint8_t *payload, uint16_t len)
{
    if (!device_is_ready(s_uart_dev)) return;

    uint8_t hdr[5];
    hdr[0] = WIFI_PROTO_SYNC1;
    hdr[1] = WIFI_PROTO_SYNC2;
    hdr[2] = type;
    hdr[3] = (uint8_t)(len & 0xFFU);
    hdr[4] = (uint8_t)((len >> 8) & 0xFFU);
    uint8_t cks = wifi_protocol_checksum(type, len, payload);

    for (int i = 0; i < 5; i++) {
        uart_poll_out(s_uart_dev, hdr[i]);
    }
    if (payload != NULL && len > 0) {
        for (uint16_t i = 0; i < len; i++) {
            uart_poll_out(s_uart_dev, payload[i]);
        }
    }
    uart_poll_out(s_uart_dev, cks);
}

void protocol_bridge_send_pcm(const uint8_t *pcm_bytes, uint16_t len)
{
    protocol_bridge_send_frame(WIFI_MSG_AUDIO_PCM, pcm_bytes, len);
}

void protocol_bridge_send_meta(const wifi_track_meta_t *meta)
{
    protocol_bridge_send_frame(WIFI_MSG_TRACK_META, (const uint8_t *)meta, sizeof(*meta));
}

void protocol_bridge_send_telemetry(const wifi_telemetry_t *telem)
{
    protocol_bridge_send_frame(WIFI_MSG_TELEMETRY, (const uint8_t *)telem, sizeof(*telem));
}

void protocol_bridge_send_ping(void)
{
    protocol_bridge_send_frame(WIFI_MSG_PING, NULL, 0);
}

bool protocol_bridge_is_spark_detected(void)
{
    return s_spark_detected;
}

/* Parse incoming packets from RT-Spark */
void protocol_bridge_process_rx(void)
{
    uint8_t b;
    static uint8_t state = 0;
    static uint8_t msg_type = 0;
    static uint16_t msg_len = 0;
    static uint8_t payload[128];
    static uint16_t rx_idx = 0;

    while (ring_buf_get(&s_rx_ring, &b, 1) == 1) {
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
                state = 0; /* Invalid payload length */
            } else if (msg_len == 0) {
                state = 6; /* Go straight to checksum */
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
                if (msg_type == WIFI_MSG_PONG) {
                    s_spark_detected = true;
                    s_last_pong_time = k_uptime_get();
                } else if (s_cmd_cb != NULL) {
                    s_cmd_cb(msg_type, payload, msg_len);
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

    if (s_spark_detected && (k_uptime_get() - s_last_pong_time > 4000)) {
        s_spark_detected = false; /* Timeout: Spark disconnected */
    }
}
