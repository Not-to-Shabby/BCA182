/**
 * ==============================================================================
 * Realtek RW007 SPI Wi-Fi Driver for Zephyr RTOS
 * ==============================================================================
 * Re-implementation of the two-stage SPI protocol used by the RT-Thread package
 * (rw007-v2.0.1, spi_wifi_rw007.c). Wire formats below are taken from that source.
 *
 * Pins (RT-Thread Spark board):
 *   SPI2  SCK=PB13  MISO=PC2  MOSI=PC3   (mode 0, MSB first)
 *   CS=PF10  RST=PG15  INT/BUSY=PG11 (falling edge = slave needs service)
 *   PB12 is the onboard SPI-flash chip select on the same bus; held high.
 * ==============================================================================
 */

#include "rw007_wifi.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <stm32f4xx.h>
#include <string.h>

/* ---- Wire protocol (spi_wifi_rw007.h) ---------------------------------- */
#define MASTER_MAGIC1       0x67452301U
#define MASTER_MAGIC2       0xEFCDAB89U
#define SLAVE_MAGIC1        0x98BADCFEU
#define SLAVE_MAGIC2        0x10325476U
#define MASTER_FLAG_MRDY    0x01
#define SLAVE_FLAG_SRDY     0x01

#define PHASE_MASTER_CMD    1
#define PHASE_MASTER_DATA   2
#define PHASE_SLAVE_CMD     3
#define PHASE_SLAVE_DATA    4

#define SPI_MAX_DATA_LEN    1520
#define PKT_HDR_LEN         8
#define MAX_SPI_PACKET_SIZE (PKT_HDR_LEN + SPI_MAX_DATA_LEN)

#define DATA_TYPE_STA_ETH   1
#define DATA_TYPE_AP_ETH    2
#define DATA_TYPE_CMD       4
#define DATA_TYPE_RESP      5
#define DATA_TYPE_CB        6

#define CMD_INIT        0
#define CMD_SET_MODE    1
#define CMD_MAC_GET     2
#define CMD_GET_SN      4
#define CMD_GET_VSR     5
#define CMD_SCAN        6
#define CMD_JOIN        7
#define CMD_SOFTAP      8
#define CMD_DISCONNECT  9
#define CMD_AP_STOP     10
#define CMD_AP_MAC_GET  22

#define EVT_CONNECT         1
#define EVT_CONNECT_FAIL    2
#define EVT_DISCONNECT      3
#define EVT_AP_START        4
#define EVT_AP_STOP         5
#define EVT_AP_ASSOC        6
#define EVT_SCAN_REPORT     9
#define EVT_SCAN_DONE       10

#define MODE_STATION    1
#define MODE_AP         2

#define SEC_OPEN        0x00000000U
#define SEC_WPA2_AES    0x00400004U

#define AP_INFO_LEN     92  /* struct rw007_ap_info_value: rt_wlan_info(60) + passwd[32] */

struct spi_data_packet {
    uint32_t data_len;
    uint32_t data_type;
    uint8_t  buffer[SPI_MAX_DATA_LEN];
};

/* ---- Hardware handles --------------------------------------------------- */
static const struct device *const gpiof = DEVICE_DT_GET(DT_NODELABEL(gpiof));
static const struct device *const gpiog = DEVICE_DT_GET(DT_NODELABEL(gpiog));
#define PIN_CS   10
#define PIN_INT  11
#define PIN_RST  15

static struct gpio_callback s_int_cb;

/* ---- Driver state ------------------------------------------------------- */
static struct spi_data_packet s_tx __aligned(4);
static struct spi_data_packet s_rx __aligned(4);
static volatile bool s_tx_pending;

static uint8_t  s_resp[SPI_MAX_DATA_LEN] __aligned(4);
static volatile uint32_t s_wait_cmd = 0xFFFFFFFFU;

static K_SEM_DEFINE(s_int_sem, 0, 1);
static K_SEM_DEFINE(s_wake_sem, 0, 1);
static K_SEM_DEFINE(s_resp_sem, 0, 1);
static K_SEM_DEFINE(s_conn_sem, 0, 1);
static K_SEM_DEFINE(s_scan_sem, 0, 1);
static K_MUTEX_DEFINE(s_cmd_lock);
static K_MUTEX_DEFINE(s_xfer_lock);

static K_THREAD_STACK_DEFINE(s_thread_stack, 2560);
static struct k_thread s_thread;

static char s_version[32];
static char s_sn[48];
static uint8_t s_mac[6];
static volatile bool s_connected;
static volatile bool s_connect_failed;
static volatile bool s_ap_up;
static volatile int s_scan_count;
static rw007_ap_entry_t s_scan_aps[RW007_MAX_SCAN_APS];
static int s_scan_ap_count = 0;
static bool s_initialized;
static volatile bool s_ap_mode;
static uint8_t s_ap_mac[6];
static rw007_rx_cb_t s_rx_cb;

/* Takes s_xfer_lock and returns true once the single TX slot is free. */
static bool tx_slot_take(int timeout_ms)
{
    int64_t end = k_uptime_get() + timeout_ms;
    for (;;) {
        k_mutex_lock(&s_xfer_lock, K_FOREVER);
        if (!s_tx_pending) {
            return true;
        }
        k_mutex_unlock(&s_xfer_lock);
        if (k_uptime_get() >= end) {
            return false;
        }
        k_msleep(2);
    }
}

static inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

static inline uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- SPI2 byte engine --------------------------------------------------- */
static void spi2_hw_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOCEN;
    RCC->APB1ENR |= RCC_APB1ENR_SPI2EN;

    /* PB12 (onboard flash CS): drive high so the flash ignores our traffic */
    GPIOB->MODER = (GPIOB->MODER & ~(3U << 24)) | (1U << 24);
    GPIOB->BSRR = (1U << 12);

    /* PB13 = SCK, AF5 */
    GPIOB->MODER = (GPIOB->MODER & ~(3U << 26)) | (2U << 26);
    GPIOB->OSPEEDR |= (3U << 26);
    GPIOB->AFR[1] = (GPIOB->AFR[1] & ~(0xFU << 20)) | (5U << 20);

    /* PC2 = MISO, PC3 = MOSI, AF5 */
    GPIOC->MODER = (GPIOC->MODER & ~((3U << 4) | (3U << 6))) | ((2U << 4) | (2U << 6));
    GPIOC->OSPEEDR |= (3U << 4) | (3U << 6);
    GPIOC->AFR[0] = (GPIOC->AFR[0] & ~((0xFU << 8) | (0xFU << 12))) | ((5U << 8) | (5U << 12));

    /* Master, software NSS, mode 0, fPCLK1/8 = 5.25 MHz (conservative bring-up speed) */
    SPI2->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | (2U << 3);
    SPI2->CR1 |= SPI_CR1_SPE;
}

static void spi2_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        uint32_t guard = 200000;
        while (!(SPI2->SR & SPI_SR_TXE) && --guard) { }
        SPI2->DR = tx ? tx[i] : 0xFF;
        guard = 200000;
        while (!(SPI2->SR & SPI_SR_RXNE) && --guard) { }
        uint8_t b = (uint8_t)SPI2->DR;
        if (rx) rx[i] = b;
    }
}

static inline void cs_take(void)    { gpio_pin_set_raw(gpiof, PIN_CS, 0); }
static inline void cs_release(void) { gpio_pin_set_raw(gpiof, PIN_CS, 1); }

static void int_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    k_sem_give(&s_int_sem);
    k_sem_give(&s_wake_sem);
}

/* ---- Two-stage transfer (wifi_data_transfer in the RT-Thread driver) ---- */
enum { XFER_OK = 0, XFER_ERR, XFER_CONTINUE };

static void build_req(uint8_t *h, uint8_t flag, uint8_t type, uint16_t seq, uint16_t m2s)
{
    memset(h, 0, 16);
    h[1] = (uint8_t)((flag & 0x0F) | (type << 4));
    h[4] = seq & 0xFF;  h[5] = seq >> 8;
    h[6] = m2s & 0xFF;  h[7] = m2s >> 8;
    put32(&h[8], MASTER_MAGIC1);
    put32(&h[12], MASTER_MAGIC2);
}

static bool resp_magic_ok(const uint8_t *r)
{
    return get32(&r[8]) == SLAVE_MAGIC1 && get32(&r[12]) == SLAVE_MAGIC2;
}

static void handle_cb_event(uint32_t evt, const uint8_t *val)
{
    switch (evt) {
    case EVT_CONNECT:
        s_connected = true; s_connect_failed = false; k_sem_give(&s_conn_sem);
        printk("[RW007] event: CONNECTED\n");
        break;
    case EVT_CONNECT_FAIL:
        s_connected = false; s_connect_failed = true; k_sem_give(&s_conn_sem);
        printk("[RW007] event: CONNECT FAILED\n");
        break;
    case EVT_DISCONNECT:
        s_connected = false;
        printk("[RW007] event: DISCONNECTED\n");
        break;
    case EVT_AP_START:
        s_ap_up = true;
        printk("[RW007] event: SOFTAP STARTED\n");
        break;
    case EVT_AP_STOP:
        s_ap_up = false;
        printk("[RW007] event: SOFTAP STOPPED\n");
        break;
    case EVT_AP_ASSOC:
        printk("[RW007] event: station joined our SoftAP\n");
        break;
    case EVT_SCAN_REPORT: {
        /* struct rt_wlan_info: security@0 band@4 rate@8 chan@12 rssi@14 ssid.len@16 ssid@17 */
        uint8_t sl = val[16] > 32 ? 32 : val[16];
        char ssid[33];
        memcpy(ssid, &val[17], sl);
        ssid[sl] = '\0';
        int16_t chan = (int16_t)(val[12] | (val[13] << 8));
        int16_t rssi = (int16_t)(val[14] | (val[15] << 8));
        printk("[RW007] AP  %-32s ch%-2d rssi %4d sec 0x%08x\n", ssid, chan, rssi, get32(val));
        s_scan_count++;

        if (ssid[0] != '\0') {
            bool found = false;
            for (int i = 0; i < s_scan_ap_count; i++) {
                if (strcmp(s_scan_aps[i].ssid, ssid) == 0) {
                    found = true;
                    if (rssi > s_scan_aps[i].rssi) s_scan_aps[i].rssi = rssi;
                    break;
                }
            }
            if (!found && s_scan_ap_count < RW007_MAX_SCAN_APS) {
                size_t len = strlen(ssid);
                if (len > 32) len = 32;
                memcpy(s_scan_aps[s_scan_ap_count].ssid, ssid, len);
                s_scan_aps[s_scan_ap_count].ssid[len] = '\0';
                s_scan_aps[s_scan_ap_count].rssi = rssi;
                s_scan_ap_count++;
            }
        }
        break;
    }
    case EVT_SCAN_DONE:
        k_sem_give(&s_scan_sem);
        break;
    default:
        break;
    }
}

static void handle_rx(const struct spi_data_packet *p)
{
    if (p->data_type == DATA_TYPE_RESP && p->data_len >= 12) {
        uint32_t cmd = get32(&p->buffer[0]);
        if (cmd == s_wait_cmd) {
            uint32_t n = p->data_len > SPI_MAX_DATA_LEN ? SPI_MAX_DATA_LEN : p->data_len;
            memcpy(s_resp, p->buffer, n);
            k_sem_give(&s_resp_sem);
        }
    } else if (p->data_type == DATA_TYPE_CB && p->data_len >= 12) {
        handle_cb_event(get32(&p->buffer[0]), &p->buffer[12]);
    }
    else if ((p->data_type == DATA_TYPE_STA_ETH || p->data_type == DATA_TYPE_AP_ETH) &&
             p->data_len >= 14 && p->data_len <= SPI_MAX_DATA_LEN && s_rx_cb) {
        s_rx_cb(p->buffer, (uint16_t)p->data_len);
    }
}

static int rw007_transfer(uint16_t seq)
{
    uint8_t req[16], rsp[16];
    bool have_tx = s_tx_pending;
    uint16_t m2s = 0;

    if (have_tx && (s_tx.data_len == 0 || s_tx.data_len > SPI_MAX_DATA_LEN)) {
        s_tx_pending = false;
        have_tx = false;
    }
    if (have_tx) {
        m2s = (uint16_t)(s_tx.data_len + PKT_HDR_LEN);
    }

    /* Stage 1: command header */
    build_req(req, MASTER_FLAG_MRDY, PHASE_MASTER_CMD, seq, m2s);
    k_sem_reset(&s_int_sem);
    cs_take();
    spi2_xfer(req, rsp, 16);
    cs_release();

    if (!resp_magic_ok(rsp) || (rsp[1] >> 4) != PHASE_SLAVE_CMD) {
        return XFER_ERR;
    }

    /* The slave pulses INT when it is ready for stage 2 */
    (void)k_sem_take(&s_int_sem, K_MSEC(100));

    /* Stage 2: data header + payload */
    build_req(req, MASTER_FLAG_MRDY, PHASE_MASTER_DATA, seq, m2s);
    cs_take();
    spi2_xfer(req, rsp, 16);

    uint16_t rseq = (uint16_t)(rsp[4] | (rsp[5] << 8));
    if (!resp_magic_ok(rsp) || rseq != seq || (rsp[1] >> 4) != PHASE_SLAVE_DATA) {
        cs_release();
        return XFER_ERR;
    }

    bool srdy = (rsp[1] & 0x0F) & SLAVE_FLAG_SRDY;
    uint16_t s2m = (uint16_t)(rsp[6] | (rsp[7] << 8));
    uint16_t tx_slave_buf = rsp[2] >> 4;
    uint32_t max_len = srdy ? m2s : 0;

    if (s2m > MAX_SPI_PACKET_SIZE) {
        s2m = 0;
    }
    if (s2m > max_len) {
        max_len = s2m;
    }
    if (max_len > 0) {
        uint32_t n = (max_len + 3U) & ~3U;
        spi2_xfer((srdy && have_tx) ? (const uint8_t *)&s_tx : NULL,
                  s2m ? (uint8_t *)&s_rx : NULL, n);
    }
    cs_release();

    if (srdy && have_tx) {
        s_tx_pending = false;
    }
    (void)k_sem_take(&s_int_sem, K_MSEC(100));

    if (s2m > 0) {
        handle_rx(&s_rx);
    }
    return tx_slave_buf > 0 ? XFER_CONTINUE : XFER_OK;
}

static void rw007_thread(void *a, void *b, void *c)
{
    uint16_t seq = 0;
    int errors = 0;

    for (;;) {
        (void)k_sem_take(&s_wake_sem, K_MSEC(1000));
        int r;
        do {
            seq = (seq >= 65533) ? 1 : (seq + 1);
            k_mutex_lock(&s_xfer_lock, K_FOREVER);
            r = rw007_transfer(seq);
            k_mutex_unlock(&s_xfer_lock);
            if (r == XFER_ERR) {
                if (++errors % 20 == 1) {
                    printk("[RW007] SPI transfer error (count %d)\n", errors);
                }
                k_msleep(20);
                break;
            }
        } while (r == XFER_CONTINUE || s_tx_pending);
        k_sem_reset(&s_wake_sem);
    }
}

/* ---- Command layer ------------------------------------------------------ */
/* returns module result code (0 = ok), or negative on transport timeout */
static int rw007_cmd(uint32_t cmd, const void *payload, uint32_t len,
                     uint8_t *out, uint32_t out_cap, uint32_t *out_len, int timeout_ms)
{
    if (len > SPI_MAX_DATA_LEN - 8) {
        return -1;
    }
    k_mutex_lock(&s_cmd_lock, K_FOREVER);

    if (!tx_slot_take(2000)) {
        k_mutex_unlock(&s_cmd_lock);
        return -3;
    }
    s_tx.data_type = DATA_TYPE_CMD;
    put32(&s_tx.buffer[0], cmd);
    put32(&s_tx.buffer[4], len);
    if (len && payload) {
        memcpy(&s_tx.buffer[8], payload, len);
    }
    s_tx.data_len = 8 + len;
    s_wait_cmd = cmd;
    k_sem_reset(&s_resp_sem);
    s_tx_pending = true;
    k_mutex_unlock(&s_xfer_lock);
    k_sem_give(&s_wake_sem);

    int rc;
    if (k_sem_take(&s_resp_sem, K_MSEC(timeout_ms)) != 0) {
        s_tx_pending = false;
        s_wait_cmd = 0xFFFFFFFFU;
        rc = -2;
    } else {
        rc = (int32_t)get32(&s_resp[8]);
        uint32_t rlen = get32(&s_resp[4]);
        if (out && out_cap) {
            uint32_t n = rlen > out_cap ? out_cap : rlen;
            if (n > SPI_MAX_DATA_LEN - 12) n = SPI_MAX_DATA_LEN - 12;
            memcpy(out, &s_resp[12], n);
            if (out_len) *out_len = n;
        }
        s_wait_cmd = 0xFFFFFFFFU;
    }
    k_mutex_unlock(&s_cmd_lock);
    return rc;
}

static void fill_ap_info(uint8_t *b, uint32_t sec, int chan, const char *ssid, const char *pass)
{
    memset(b, 0, AP_INFO_LEN);
    put32(&b[0], sec);
    put32(&b[4], 1);                    /* 2.4 GHz band */
    b[12] = chan & 0xFF; b[13] = (chan >> 8) & 0xFF;
    size_t sl = strlen(ssid);
    if (sl > 32) sl = 32;
    b[16] = (uint8_t)sl;
    memcpy(&b[17], ssid, sl);
    if (pass) {
        strncpy((char *)&b[60], pass, 31);
    }
}

/* ---- Public API --------------------------------------------------------- */
bool rw007_wifi_init(void)
{
    if (!device_is_ready(gpiof) || !device_is_ready(gpiog)) {
        printk("[RW007] GPIO ports not ready\n");
        return false;
    }

    printk("[RW007] SPI2: SCK=PB13 MISO=PC2 MOSI=PC3, CS=PF10 RST=PG15 INT=PG11\n");

    /* Idle levels BEFORE reset: CS high, SCK low (these select the RW007 boot mode) */
    gpio_pin_configure(gpiof, PIN_CS, GPIO_OUTPUT_HIGH);
    gpio_pin_configure(gpiog, PIN_RST, GPIO_OUTPUT_LOW);
    gpio_pin_configure(gpiog, PIN_INT, GPIO_INPUT | GPIO_PULL_DOWN);
    spi2_hw_init();

    k_msleep(100);
    gpio_pin_set_raw(gpiog, PIN_RST, 1);

    uint32_t t0 = k_uptime_get_32();
    while (!gpio_pin_get_raw(gpiog, PIN_INT)) {
        k_msleep(5);
        if (k_uptime_get_32() - t0 > 3000) {
            printk("[RW007] INT/BUSY never went high after reset - module not responding\n");
            return false;
        }
    }
    k_msleep(200);

    gpio_pin_configure(gpiog, PIN_INT, GPIO_INPUT | GPIO_PULL_UP);
    gpio_init_callback(&s_int_cb, int_isr, BIT(PIN_INT));
    gpio_add_callback(gpiog, &s_int_cb);
    gpio_pin_interrupt_configure(gpiog, PIN_INT, GPIO_INT_EDGE_FALLING);

    k_thread_create(&s_thread, s_thread_stack, K_THREAD_STACK_SIZEOF(s_thread_stack),
                    rw007_thread, NULL, NULL, NULL, 5, 0, K_NO_WAIT);
    k_thread_name_set(&s_thread, "rw007");

    int rc = -2;
    for (int i = 0; i < 3 && rc != 0; i++) {
        rc = rw007_cmd(CMD_INIT, NULL, 0, NULL, 0, NULL, 3000);
    }
    printk("[RW007] CMD_INIT -> %d\n", rc);
    if (rc == -2) {
        printk("[RW007] No response from module over SPI. Check wiring/pins.\n");
        return false;
    }

    uint32_t n = 0;
    memset(s_version, 0, sizeof(s_version));
    memset(s_sn, 0, sizeof(s_sn));
    rw007_cmd(CMD_GET_VSR, NULL, 0, (uint8_t *)s_version, sizeof(s_version) - 1, &n, 3000);
    rw007_cmd(CMD_GET_SN, NULL, 0, (uint8_t *)s_sn, sizeof(s_sn) - 1, &n, 3000);
    rw007_cmd(CMD_MAC_GET, NULL, 0, s_mac, sizeof(s_mac), &n, 3000);
    printk("[RW007] version : '%s'\n", s_version);
    printk("[RW007] serial  : '%s'\n", s_sn);
    printk("[RW007] STA MAC : %02x:%02x:%02x:%02x:%02x:%02x\n",
           s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);

    uint32_t mode = MODE_STATION;
    rc = rw007_cmd(CMD_SET_MODE, &mode, sizeof(mode), NULL, 0, NULL, 3000);
    printk("[RW007] SET_MODE(station) -> %d\n", rc);

    s_initialized = (s_version[0] != '\0');
    return s_initialized;
}

int rw007_wifi_scan(void)
{
    if (!s_initialized) return -1;
    s_scan_count = 0;
    s_scan_ap_count = 0;
    k_sem_reset(&s_scan_sem);
    int rc = rw007_cmd(CMD_SCAN, NULL, 0, NULL, 0, NULL, 5000);
    printk("[RW007] CMD_SCAN -> %d\n", rc);
    if (rc != 0) return -1;
    (void)k_sem_take(&s_scan_sem, K_SECONDS(8));
    printk("[RW007] scan complete: %d access points (%d unique)\n", s_scan_count, s_scan_ap_count);
    return s_scan_count;
}

int rw007_wifi_get_scan_aps(rw007_ap_entry_t *out_aps, int max_count)
{
    if (!out_aps || max_count <= 0) return 0;
    int n = (s_scan_ap_count < max_count) ? s_scan_ap_count : max_count;
    memcpy(out_aps, s_scan_aps, n * sizeof(rw007_ap_entry_t));
    return n;
}

bool rw007_wifi_connect(const char *ssid, const char *password)
{
    if (!s_initialized || !ssid) return false;
    printk("[RW007] joining '%s'...\n", ssid);

    uint8_t buf[AP_INFO_LEN];
    fill_ap_info(buf, (password && password[0]) ? SEC_WPA2_AES : SEC_OPEN, 0, ssid, password);

    s_connected = false;
    s_connect_failed = false;
    k_sem_reset(&s_conn_sem);
    int rc = rw007_cmd(CMD_JOIN, buf, AP_INFO_LEN, NULL, 0, NULL, 10000);
    printk("[RW007] CMD_JOIN -> %d\n", rc);
    if (rc != 0) return false;

    (void)k_sem_take(&s_conn_sem, K_SECONDS(15));
    return s_connected;
}

bool rw007_wifi_start_softap(const char *ap_ssid, const char *ap_pass)
{
    if (!s_initialized || !ap_ssid) return false;
    printk("[RW007] starting SoftAP '%s'...\n", ap_ssid);

    uint32_t mode = MODE_AP;
    int rc = rw007_cmd(CMD_SET_MODE, &mode, sizeof(mode), NULL, 0, NULL, 3000);
    printk("[RW007] SET_MODE(ap) -> %d\n", rc);

    uint8_t buf[AP_INFO_LEN];
    bool secured = ap_pass && strlen(ap_pass) >= 8;
    fill_ap_info(buf, secured ? SEC_WPA2_AES : SEC_OPEN, 6, ap_ssid, secured ? ap_pass : NULL);

    rc = rw007_cmd(CMD_SOFTAP, buf, AP_INFO_LEN, NULL, 0, NULL, 5000);
    printk("[RW007] CMD_SOFTAP -> %d\n", rc);
    if (rc != 0) return false;

    s_ap_mode = true;
    k_msleep(500); /* let the AP_START event arrive */

    uint32_t n = 0;
    if (rw007_cmd(CMD_AP_MAC_GET, NULL, 0, s_ap_mac, sizeof(s_ap_mac), &n, 3000) == 0 && n >= 6) {
        printk("[RW007] AP  MAC : %02x:%02x:%02x:%02x:%02x:%02x (STA MAC differs: %s)\n",
               s_ap_mac[0], s_ap_mac[1], s_ap_mac[2], s_ap_mac[3], s_ap_mac[4], s_ap_mac[5],
               memcmp(s_ap_mac, s_mac, 6) ? "yes" : "no");
    } else {
        memcpy(s_ap_mac, s_mac, 6);
        printk("[RW007] AP MAC query failed, using STA MAC\n");
    }
    return true;
}

bool rw007_wifi_stop_softap(void)
{
    if (!s_initialized) return false;
    int rc = rw007_cmd(CMD_AP_STOP, NULL, 0, NULL, 0, NULL, 3000);
    printk("[RW007] CMD_AP_STOP -> %d\n", rc);
    uint32_t mode = MODE_STATION;
    rc = rw007_cmd(CMD_SET_MODE, &mode, sizeof(mode), NULL, 0, NULL, 3000);
    s_ap_up = false;
    s_ap_mode = false;
    return rc == 0;
}

void rw007_wifi_set_rx_callback(rw007_rx_cb_t cb)
{
    s_rx_cb = cb;
}

bool rw007_wifi_send_frame(const uint8_t *frame, uint16_t len)
{
    if (!s_initialized || len < 14 || len > SPI_MAX_DATA_LEN) {
        return false;
    }
    if (!tx_slot_take(500)) {
        return false;
    }
    memcpy(s_tx.buffer, frame, len);
    s_tx.data_type = s_ap_mode ? DATA_TYPE_AP_ETH : DATA_TYPE_STA_ETH;
    s_tx.data_len = len;
    s_tx_pending = true;
    k_mutex_unlock(&s_xfer_lock);
    k_sem_give(&s_wake_sem);
    return true;
}

void rw007_wifi_get_mac(uint8_t mac[6])
{
    memcpy(mac, s_mac, 6);
}

void rw007_wifi_get_ap_mac(uint8_t mac[6])
{
    memcpy(mac, s_ap_mac, 6);
}

bool rw007_wifi_is_connected(void)   { return s_connected; }
bool rw007_wifi_softap_active(void)  { return s_ap_up; }
const char *rw007_wifi_get_version(void) { return s_version; }
const char *rw007_wifi_get_sn(void)      { return s_sn; }
