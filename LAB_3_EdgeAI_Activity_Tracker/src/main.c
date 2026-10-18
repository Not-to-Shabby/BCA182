/**
 * ==============================================================================
 * Laboratory Activity 3: Edge AI Activity Tracker & BLE Telemetry
 * ==============================================================================
 * Microcontroller: STMicroelectronics STM32F407ZGT6 (RT-Thread Spark Board)
 * Sensors: InvenSense ICM-20608-G (I2C2 on PB10/PB11 @ 10 Hz)
 * Display: 1.3" 240x240 ST7789 v3 IPS LCD via FSMC Bank 3 8080 Parallel Bus
 * Wireless: Realtek RW007 BLE Peripheral (Method A GATT Telemetry Service)
 * Model: 10-Tree Random Forest executing on ARM Cortex-M4F Hardware FPU
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#if defined(ZEPHYR_VERSION_CODE) || defined(__arm__)
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#define SLEEP_MS(ms) k_msleep(ms)
#define GET_TIME_MS() k_uptime_get_32()
#else
#define SLEEP_MS(ms) (void)ms
static uint32_t s_sim_time = 0;
#define GET_TIME_MS() (s_sim_time += 100)
#endif

#include "activity_model.h"
#include "feature_extractor.h"
#include "icm20608.h"
#include "rw007_wifi.h"
#include "wifi_storage.h"
#include "wifi_portal.h"
#include "ble_protocol.h"
#include "lcd_st7789.h"
#include "ui_dashboard.h"
#include "uart_telemetry.h"
#include "rw007_netif.h"
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/dhcpv4.h>

static int s_udp_fd = -1;
static K_SEM_DEFINE(s_ip_sem, 0, 1);
static struct net_mgmt_event_callback s_ip_cb;

static void ip_event(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
    if (event == NET_EVENT_IPV4_ADDR_ADD) {
        k_sem_give(&s_ip_sem);
    }
}

/* Associates, then waits for a real DHCP lease. Returns true only with an IP. */
static bool join_and_get_ip(const char *ssid, const char *pass)
{
    struct net_if *iface = rw007_netif_get();

    if (!iface) return false;

    /* Ensure interface link address matches hardware Station MAC */
    uint8_t sta_mac[6];
    rw007_wifi_get_mac(sta_mac);
    rw007_netif_set_mac(sta_mac);

    if (!rw007_wifi_connect(ssid, pass)) {
        return false;
    }
    net_mgmt_init_event_callback(&s_ip_cb, ip_event, NET_EVENT_IPV4_ADDR_ADD);
    net_mgmt_add_event_callback(&s_ip_cb);
    k_sem_reset(&s_ip_sem);

    rw007_netif_link(true);
    net_dhcpv4_start(iface);
    if (k_sem_take(&s_ip_sem, K_SECONDS(20)) != 0) {
        uart1_printf("[WIFI] associated but no DHCP lease\n");
        net_dhcpv4_stop(iface);
        rw007_netif_link(false);
        return false;
    }

    struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;
    if (ipv4) {
        for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
            if (ipv4->unicast[i].ipv4.is_used) {
                const uint8_t *a = ipv4->unicast[i].ipv4.address.in_addr.s4_addr;
                uart1_printf("[WIFI] got IPv4 address via DHCP: %u.%u.%u.%u\n",
                             a[0], a[1], a[2], a[3]);
                break;
            }
        }
    }
    return true;
}

static int open_udp_socket(void)
{
    int one = 1;
    int fd = zsock_socket(NET_AF_INET, NET_SOCK_DGRAM, NET_IPPROTO_UDP);

    if (fd >= 0) {
        zsock_setsockopt(fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_BROADCAST, &one, sizeof(one));
    }
    return fd;
}

static bool connect_with_retries(const char *ssid, const char *pass, int max_rounds)
{
    if (!ssid || ssid[0] == '\0') return false;

    for (int round = 1; round <= max_rounds; round++) {
        uart1_printf("[WIFI] Connection attempt %d of %d to '%s'...\n", round, max_rounds, ssid);
        ui_dashboard_show_connecting(ssid, round, max_rounds, "Associating with AP...");

        if (join_and_get_ip(ssid, pass)) {
            ui_dashboard_show_connecting(ssid, round, max_rounds, "Connected! IP acquired.");
            SLEEP_MS(500);
            ui_dashboard_show_connecting(ssid, 0, 0, NULL);
            return true;
        }

        if (round < max_rounds) {
            uart1_printf("[WIFI] Attempt %d failed. Retrying in 2 seconds...\n", round);
            ui_dashboard_show_connecting(ssid, round, max_rounds, "Failed. Retrying in 2s...");
            SLEEP_MS(2000);
        } else {
            uart1_printf("[WIFI] All %d connection attempts failed.\n", max_rounds);
            ui_dashboard_show_connecting(ssid, round, max_rounds, "3 attempts failed.");
            SLEEP_MS(1000);
        }
    }

    ui_dashboard_show_connecting(ssid, 0, 0, NULL);
    return false;
}

static void board_buttons_init(void)
{
    /* Enable GPIOC peripheral clock on AHB1 */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;

    /* PC0 (KEY0) and PC1 (KEY1) configured as Inputs with Pull-Up (active LOW) */
    GPIOC->MODER &= ~((3U << (0 * 2)) | (3U << (1 * 2)));
    GPIOC->PUPDR = (GPIOC->PUPDR & ~((3U << (0 * 2)) | (3U << (1 * 2)))) |
                   ((1U << (0 * 2)) | (1U << (1 * 2)));
}

static inline bool key0_is_pressed(void)
{
    return !(GPIOC->IDR & (1U << 0));
}

static inline bool key1_is_pressed(void)
{
    return !(GPIOC->IDR & (1U << 1));
}

int main(void)
{
    /* 0. Initialize Direct Hardware USART1 on PA9/PA10 for ST-LINK VCP @ 115200 baud */
    uart1_telemetry_init();
    board_buttons_init();

    uart1_printf("\n===============================================================\n");
    uart1_printf("  BCA180/BCA152 - Laboratory Activity 3: Activity Recognition  \n");
    uart1_printf("  Target: RT-Thread Spark Board (STM32F407ZGT6)                \n");
    uart1_printf("  Display: 240x240 ST7789 IPS LCD (FSMC Parallel Bus)          \n");
    uart1_printf("  Telemetry: Direct Hardware USART1 (PA9/PA10 @ 115200 baud)   \n");
    uart1_printf("  Architecture: On-Device TinyML (Cortex-M4F FPU) + RW007 Wi-Fi \n");
    uart1_printf("===============================================================\n\n");

    /* 1. Initialize LCD & Render Dashboard Frame */
    uart1_printf("[INIT] Initializing 240x240 ST7789 IPS LCD on FSMC Bank 3...\n");
    ui_dashboard_init();
    uart1_printf("[INIT] LCD frame and backlight (PF9) active.\n");

    /* 2. Initialize 6-Axis Motion Sensor */
    uart1_printf("[INIT] Initializing InvenSense ICM-20608 6-Axis IMU...\n");
    bool imu_ok = icm20608_init();
    if (!imu_ok) {
        uart1_printf("[WARN] ICM-20608 not responding (WHO_AM_I mismatch). Using simulation fallback.\n");
    } else {
        uart1_printf("[INIT] ICM-20608 verified (WHO_AM_I = 0xAF). Configured @ 10 Hz, +-4g, +-1000 dps.\n");
    }

    /* 3. Initialize Feature Extractor Window & Pedometer */
    feature_window_t window;
    feature_extractor_init(&window);
    uart1_printf("[INIT] Feature window initialized (15 samples, 21 metrics, FPU-accelerated).\n");

    /* 4. Initialize Realtek RW007 SPI Wi-Fi Module under Zephyr RTOS */
    uart1_printf("[INIT] Initializing RW007 SPI Wi-Fi Module under Zephyr RTOS...\n");
    if (rw007_wifi_init()) {
        uart1_printf("[RW007] module online, firmware '%s'\n", rw007_wifi_get_version());
        rw007_wifi_scan();
    } else {
        uart1_printf("[RW007] module did NOT respond - Wi-Fi unavailable\n");
    }

    /* 5. Join a network: saved credentials first, otherwise the setup portal */
    bool online = false;
    char saved_ssid[WIFI_SSID_MAX_LEN] = {0};
    char saved_pass[WIFI_PASS_MAX_LEN] = {0};

    if (rw007_wifi_get_version()[0] != '\0') {
        if (wifi_storage_read(saved_ssid, saved_pass)) {
            uart1_printf("[WIFI] Found saved network '%s'. Attempting 3-round connection...\n", saved_ssid);
            online = connect_with_retries(saved_ssid, saved_pass, 3);
        } else {
            uart1_printf("[WIFI] No saved network in Flash.\n");
        }

        while (!online) {
            uart1_printf("[WIFI] Starting setup portal...\n");
            if (wifi_portal_run(180, saved_ssid, saved_pass)) {
                online = connect_with_retries(saved_ssid, saved_pass, 3);
                if (online) {
                    wifi_storage_save(saved_ssid, saved_pass);
                    uart1_printf("[WIFI] Credentials verified & saved to Flash.\n");
                } else {
                    uart1_printf("[WIFI] Failed 3 attempts to '%s'. Re-opening portal.\n", saved_ssid);
                }
            } else {
                /* Portal timed out without credentials: retry saved Wi-Fi if available */
                if (wifi_storage_read(saved_ssid, saved_pass)) {
                    online = connect_with_retries(saved_ssid, saved_pass, 2);
                }
            }
        }
        ui_dashboard_init();
    }
    s_udp_fd = online ? open_udp_socket() : -1;

    uart1_printf("\n[SYSTEM] Entering continuous real-time acquisition and classification loop...\n\n");

    activity_type_t current_activity = ACTIVITY_WALKING;
    activity_type_t last_activity = ACTIVITY_UNKNOWN;
    uint8_t confidence = 85;
    uint32_t loop_count = 0;

    while (1) {
        uint32_t now_ms = GET_TIME_MS();
        imu_sample_t sample;

        /* Acquire reading from physical sensor or fallback */
        bool got_sample = false;
        if (imu_ok) {
            got_sample = icm20608_read_sample(&sample);
        }

        if (!got_sample) {
            /* If physical sensor read fails, default to realistic STATIONARY resting baseline
             * (1.0g gravity along Z-axis, microscopic MEMS thermal noise < 0.01g) */
            float noise = ((float)(loop_count % 7) - 3.0f) * 0.001f;
            sample.ax = 0.02f + noise;
            sample.ay = -0.01f - noise;
            sample.az = 0.99f + noise;
            sample.gx = 0.001f;
            sample.gy = -0.001f;
            sample.gz = 0.001f;
        }

        /* Push into temporal sliding window */
        bool window_ready = feature_extractor_push(&window, &sample, now_ms);

        /* Compute features and run TinyML inference every 7 samples (~0.7s step) */
        if (window_ready && (loop_count % 7 == 0)) {
            float features[ACTIVITY_NUM_FEATURES];
            feature_extractor_compute(&window, features);

            /* Run 10-Tree Random Forest Model on Cortex-M4F Hardware FPU (< 5 microseconds) */
            current_activity = tinyml_predict_activity(features, &confidence);

            float mag_mean = features[18];

            /* 1. Transmit Wi-Fi UDP Telemetry to Port 8888 */
            ble_activity_packet_t pkt;
            ble_protocol_pack(&pkt, (uint8_t)current_activity, confidence,
                              window.current_cadence_spm, window.total_steps, mag_mean);

            if (s_udp_fd >= 0) {
                struct net_sockaddr_in dst = { 0 };

                dst.sin_family = NET_AF_INET;
                dst.sin_port = net_htons(RW007_UDP_PORT);
                dst.sin_addr.s_addr = 0xFFFFFFFFU; /* limited broadcast */
                if (zsock_sendto(s_udp_fd, &pkt, sizeof(pkt), 0,
                                 (struct net_sockaddr *)&dst, sizeof(dst)) < 0) {
                    uart1_printf("[UDP] send failed\n");
                }
            }

            const char *act_str = "STATIONARY";
            if (current_activity == ACTIVITY_RUNNING) act_str = "RUNNING";
            else if (current_activity == ACTIVITY_WALKING) act_str = "WALKING";

            uart1_printf("[WIFI TX] Act: %s (%u%%) | Cadence: %u SPM | Steps: %lu | Mag: %.2fg | CKS: 0x%02X\n",
                         act_str, confidence, window.current_cadence_spm,
                         (unsigned long)window.total_steps, (double)mag_mean, pkt.checksum);

            /* 2. Differential Live Update on ST7789 IPS Display */
            ui_dashboard_update(
                current_activity,
                confidence,
                window.current_cadence_spm,
                window.total_steps,
                mag_mean,
                sample.ax,
                sample.ay,
                sample.az
            );

            /* 3. Terminal Notification on State Transition */
            if (current_activity != last_activity) {
                const char *act_name = "STATIONARY";
                if (current_activity == ACTIVITY_RUNNING) act_name = "RUNNING";
                else if (current_activity == ACTIVITY_WALKING) act_name = "WALKING";

                uart1_printf("[TRANSITION] Movement changed -> %s (Confidence: %u%%)\n",
                             act_name, confidence);
                last_activity = current_activity;
            }
        } else if (loop_count % 3 == 0) {
            /* High-rate live accelerometer readout update on LCD (every 300 ms) */
            float inst_mag = sqrtf(sample.ax * sample.ax + sample.ay * sample.ay + sample.az * sample.az);
            ui_dashboard_update(
                current_activity,
                confidence,
                window.current_cadence_spm,
                window.total_steps,
                inst_mag,
                sample.ax,
                sample.ay,
                sample.az
            );
        }

        loop_count++;

        /* Check if user pressed KEY0 to request Wi-Fi Reconfiguration */
        if (key0_is_pressed()) {
            SLEEP_MS(50);
            if (key0_is_pressed()) {
                uart1_printf("[KEY0] Wi-Fi reconfigure requested. Showing confirmation on LCD...\n");
                while (key0_is_pressed()) {
                    SLEEP_MS(10);
                }

                int countdown = 8;
                bool confirmed = false;
                uint32_t t_last = GET_TIME_MS();

                while (countdown > 0) {
                    ui_dashboard_show_reset_confirm(saved_ssid, countdown);

                    if (key0_is_pressed()) {
                        SLEEP_MS(50);
                        if (key0_is_pressed()) {
                            confirmed = true;
                            while (key0_is_pressed()) SLEEP_MS(10);
                            break;
                        }
                    }

                    if (key1_is_pressed()) {
                        SLEEP_MS(50);
                        if (key1_is_pressed()) {
                            uart1_printf("[KEY1] Reconfiguration cancelled by user.\n");
                            while (key1_is_pressed()) SLEEP_MS(10);
                            break;
                        }
                    }

                    SLEEP_MS(50);
                    if (GET_TIME_MS() - t_last >= 1000) {
                        countdown--;
                        t_last = GET_TIME_MS();
                    }
                }
                ui_dashboard_show_reset_confirm(saved_ssid, 0);

                if (confirmed) {
                    uart1_printf("[WIFI] Reset confirmed by user! Launching Setup Portal...\n");
                    if (s_udp_fd >= 0) {
                        zsock_close(s_udp_fd);
                        s_udp_fd = -1;
                    }
                    wifi_storage_clear();
                    memset(saved_ssid, 0, sizeof(saved_ssid));
                    memset(saved_pass, 0, sizeof(saved_pass));

                    online = false;
                    while (!online) {
                        uart1_printf("[WIFI] Starting setup portal...\n");
                        if (wifi_portal_run(180, saved_ssid, saved_pass)) {
                            online = connect_with_retries(saved_ssid, saved_pass, 3);
                            if (online) {
                                wifi_storage_save(saved_ssid, saved_pass);
                                uart1_printf("[WIFI] New credentials saved to Flash.\n");
                                break;
                            } else {
                                uart1_printf("[WIFI] Failed 3 attempts to '%s'. Re-opening portal.\n", saved_ssid);
                            }
                        } else {
                            break;
                        }
                    }
                    s_udp_fd = online ? open_udp_socket() : -1;
                }

                /* Restore main dashboard display */
                ui_dashboard_init();
            }
        }

        SLEEP_MS(100); /* 100 ms loop delay = 10 Hz sampling rate */
    }

    return 0;
}
