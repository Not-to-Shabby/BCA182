/**
 * @file net_task.c
 * @brief Communication thread: RW007 Wi-Fi bring-up, DHCP, MQTT session.
 *
 * Responsibilities (assignment "Communication task"):
 *  - initialize the onboard Wi-Fi (verified RW007 SPI driver) and maintain the
 *    broker connection, reconnecting with backoff without blocking sensing
 *    (sensing pushes into a queue; this thread is the only MQTT user);
 *  - publish aggregated telemetry every APP_MQTT_PERIOD_MS and status
 *    heartbeats, subscribe to the command topic, publish acknowledgments;
 *  - hand validated commands to control_task via g_cmd_q.
 */

#include <zephyr/kernel.h>
/* net_if.h must precede dhcpv4.h on Zephyr 4.4: the dhcpv4 prototypes need
 * the complete struct net_if or GCC flags incompatible-pointer-type warnings. */
#include <zephyr/net/net_if.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/poll.h>
#include <zephyr/sys/printk.h>

#include <string.h>

#include "app_config.h"
#include "app_context.h"
#include "rw007_netif.h"
#include "rw007_wifi.h"
#include "telemetry.h"
#include "wifi_storage.h"

#define RXBUF_SIZE 512
#define TXBUF_SIZE 512
#define CMD_PAYLOAD_MAX 255

static uint8_t s_rxbuf[RXBUF_SIZE];
static uint8_t s_txbuf[TXBUF_SIZE];
static uint8_t s_cmdbuf[CMD_PAYLOAD_MAX];

static struct mqtt_client s_client;
static struct sockaddr_in s_broker;
static bool s_mqtt_connected;

static bool s_wifi_up;
static bool s_lease_ok;

/* Latest link state for telemetry/status builders */
static volatile bool s_wifi_ok;
static volatile bool s_mqtt_ok;

static struct net_mgmt_event_callback s_dhcp_cb;
static volatile bool s_addr_added;

/* ------------------------------------------------------------------------ */
/* DHCP                                                                      */
/* ------------------------------------------------------------------------ */

static void on_ipv4_addr(struct net_mgmt_event_callback *cb, uint64_t mgmt_evt,
                         struct net_if *iface)
{
    ARG_UNUSED(cb);
    ARG_UNUSED(iface);
    if (mgmt_evt == NET_EVENT_IPV4_ADDR_ADD) {
        s_addr_added = true;
    }
}

static bool wait_dhcp_lease(struct net_if *iface, int32_t timeout_ms)
{
    s_addr_added = false;
    net_dhcpv4_start(iface);

    int64_t deadline = k_uptime_get() + timeout_ms;
    while (k_uptime_get() < deadline) {
        if (s_addr_added) {
            struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;
            if (ipv4 != NULL && ipv4->unicast[0].ipv4.is_used) {
                const uint8_t *a = ipv4->unicast[0].ipv4.address.in_addr.s4_addr;
                printk("[NET] got IPv4 via DHCP: %u.%u.%u.%u\n", a[0], a[1], a[2],
                       a[3]);
                return true;
            }
        }
        k_msleep(100);
    }
    net_dhcpv4_stop(iface);
    printk("[NET] associated but no DHCP lease within %d ms\n", (int)timeout_ms);
    return false;
}

/* ------------------------------------------------------------------------ */
/* Wi-Fi                                                                     */
/* ------------------------------------------------------------------------ */

static bool wifi_bringup(void)
{
    if (!rw007_wifi_init()) {
        printk("[NET] RW007 init failed\n");
        return false;
    }

    uint8_t mac[6];
    rw007_wifi_get_mac(mac);
    rw007_netif_set_mac(mac);

    char ssid[WIFI_SSID_MAX_LEN];
    char pass[WIFI_PASS_MAX_LEN];
    if (!wifi_storage_read(ssid, pass)) {
        printk("[NET] no stored Wi-Fi credentials; set them via the onboarding "
               "portal (phase 3) and retry\n");
        return false;
    }

    printk("[NET] joining \"%s\" ...\n", ssid);
    if (!rw007_wifi_connect(ssid, pass)) {
        printk("[NET] association failed\n");
        return false;
    }

    rw007_netif_link(true);
    struct net_if *iface = rw007_netif_get();
    if (iface == NULL) {
        printk("[NET] no netif after association\n");
        rw007_netif_link(false);
        return false;
    }

    if (!wait_dhcp_lease(iface, 30000)) {
        rw007_netif_link(false);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* MQTT                                                                      */
/* ------------------------------------------------------------------------ */

static void publish_json(const char *topic, const char *json, size_t len)
{
    struct mqtt_publish_param p;
    memset(&p, 0, sizeof(p));
    p.message.topic.topic.utf8 = (const uint8_t *)topic;
    p.message.topic.topic.size = strlen(topic);
    p.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
    p.message.payload.data = (uint8_t *)json;
    p.message.payload.len = (uint32_t)len;
    int rc = mqtt_publish(&s_client, &p);
    if (rc != 0) {
        printk("[MQTT] publish to %s failed: %d\n", topic, rc);
    }
}

static void publish_status(bool online)
{
    char json[TELEMETRY_JSON_MAX];
    int rc = telemetry_build_status(online, app_state_raw()->snap.buzzer_on,
                                    app_state_raw()->snap.led_mode, s_wifi_ok,
                                    s_mqtt_ok, (uint32_t)(k_uptime_get() / 1000),
                                    APP_DEVICE_ID, json, sizeof(json));
    if (rc == TELEMETRY_OK) {
        publish_json(APP_TOPIC_STATUS, json, strlen(json));
    }
}

static void mqtt_evt(struct mqtt_client *client, const struct mqtt_evt *evt)
{
    ARG_UNUSED(client);
    switch (evt->type) {
    case MQTT_EVT_CONNACK:
        if (evt->result == 0) {
            printk("[MQTT] connected to broker %s:%u\n", APP_BROKER_HOST,
                   (unsigned)APP_BROKER_PORT);
            s_mqtt_connected = true;
            s_mqtt_ok = true;

            static struct mqtt_topic cmd_topic;
            cmd_topic.topic.utf8 = (const uint8_t *)APP_TOPIC_CMD;
            cmd_topic.topic.size = strlen(APP_TOPIC_CMD);
            cmd_topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
            static struct mqtt_subscription_list sub;
            sub.list = &cmd_topic;
            sub.list_count = 1;
            sub.message_id = 1u;
            int rc = mqtt_subscribe(&s_client, &sub);
            printk("[MQTT] subscribed to %s: %d\n", APP_TOPIC_CMD, rc);

            publish_status(true);
        } else {
            printk("[MQTT] broker refused connection: %d\n", evt->result);
        }
        break;

    case MQTT_EVT_DISCONNECT:
        printk("[MQTT] disconnected\n");
        s_mqtt_connected = false;
        s_mqtt_ok = false;
        break;

    case MQTT_EVT_PUBLISH: {
        const struct mqtt_publish_param *pub = &evt->param.publish;
        uint32_t len = pub->message.payload.len;
        if (len > sizeof(s_cmdbuf)) {
            printk("[MQTT] oversized command (%u bytes) rejected\n", (unsigned)len);
            app_count_reject();
            break;
        }
        if (mqtt_readall_publish_payload(client, s_cmdbuf, len) != 0) {
            app_count_reject();
            break;
        }

        mqtt_command_t cmd;
        int prc = telemetry_parse_command((const char *)s_cmdbuf, len, &cmd);
        if (prc == TELEMETRY_OK) {
            app_count_accept();
            app_cmd_put(&cmd); /* control_task validates priority rules again */
        } else {
            /* Invalid or unauthorized commands are rejected, never guessed. */
            printk("[MQTT] rejected malformed command (%d): %.*s\n", prc, (int)len,
                   (const char *)s_cmdbuf);
            app_count_reject();
            mqtt_command_t bad;
            memset(&bad, 0, sizeof(bad));
            bad.id = 0;
            bad.type = APP_CMD_NONE;
            app_ack_put(&bad, false, "malformed_command", "");
        }
        break;
    }

    default:
        break;
    }
}

static bool mqtt_try_connect(void)
{
    memset(&s_client, 0, sizeof(s_client));

    memset(&s_broker, 0, sizeof(s_broker));
    s_broker.sin_family = AF_INET;
    s_broker.sin_port = htons(APP_BROKER_PORT);
    if (net_addr_pton(AF_INET, APP_BROKER_HOST, &s_broker.sin_addr) != 0) {
        printk("[MQTT] bad broker address %s\n", APP_BROKER_HOST);
        return false;
    }

    s_client.broker = (struct sockaddr *)&s_broker;
    s_client.evt_cb = mqtt_evt;
    s_client.client_id.utf8 = (const uint8_t *)APP_DEVICE_ID;
    s_client.client_id.size = strlen(APP_DEVICE_ID);
    s_client.rx_buf = s_rxbuf;
    s_client.rx_buf_size = sizeof(s_rxbuf);
    s_client.tx_buf = s_txbuf;
    s_client.tx_buf_size = sizeof(s_txbuf);

    int rc = mqtt_connect(&s_client);
    if (rc != 0) {
        printk("[MQTT] connect attempt failed: %d\n", rc);
        return false;
    }

    /* CONNACK arrives asynchronously via mqtt_input(); wait briefly. */
    int64_t deadline = k_uptime_get() + 5000;
    while (!s_mqtt_connected && k_uptime_get() < deadline) {
        struct pollfd fd = {
            .fd = s_client.transport.tcp.sock,
            .events = POLLIN,
        };
        int prc = zsock_poll(&fd, 1, 100);
        if (prc > 0) {
            mqtt_input(&s_client);
        }
    }
    return s_mqtt_connected;
}

/* ------------------------------------------------------------------------ */
/* Thread                                                                    */
/* ------------------------------------------------------------------------ */

void net_task_run(void)
{
    printk("[NET] communication thread started (broker %s:%u)\n", APP_BROKER_HOST,
           (unsigned)APP_BROKER_PORT);

    net_mgmt_init_event_callback(&s_dhcp_cb, on_ipv4_addr, NET_EVENT_IPV4_ADDR_ADD);
    net_mgmt_add_event_callback(&s_dhcp_cb);

    int64_t next_telemetry = k_uptime_get();
    int64_t next_status = k_uptime_get() + APP_STATUS_PERIOD_MS;
    int32_t connect_backoff_ms = 1000;

    telemetry_snapshot_t latest;
    memset(&latest, 0, sizeof(latest));
    bool have_latest = false;

    for (;;) {
        k_msleep(100);

        /* --- Wi-Fi / lease maintenance (non-blocking for sensing) ------- */
        if (!s_wifi_up) {
            s_wifi_ok = false;
            if (wifi_bringup()) {
                s_wifi_up = true;
                s_lease_ok = true;
                s_wifi_ok = true;
                connect_backoff_ms = 1000;
            } else {
                /* retry later; sensing keeps running the whole time */
                k_msleep(5000);
                continue;
            }
        }
        if (!rw007_wifi_is_connected()) {
            printk("[NET] Wi-Fi lost\n");
            s_wifi_up = false;
            s_lease_ok = false;
            s_wifi_ok = false;
            s_mqtt_ok = false;
            continue;
        }

        const struct net_if *iface = rw007_netif_get();

        /* --- MQTT session ------------------------------------------------ */
        if (!s_mqtt_connected) {
            if (s_lease_ok && iface != NULL && mqtt_try_connect()) {
                connect_backoff_ms = 1000;
                next_telemetry = k_uptime_get();
            } else {
                k_msleep(connect_backoff_ms);
                connect_backoff_ms = connect_backoff_ms * 2;
                if (connect_backoff_ms > 60000) {
                    connect_backoff_ms = 60000;
                }
                continue;
            }
        }

        /* --- Service the socket ------------------------------------------ */
        struct pollfd fd = {
            .fd = s_client.transport.tcp.sock,
            .events = POLLIN,
        };
        int prc = zsock_poll(&fd, 1, 50);
        if (prc < 0) {
            printk("[NET] poll error; dropping MQTT session\n");
            mqtt_disconnect(&s_client, NULL);
            s_mqtt_connected = false;
            s_mqtt_ok = false;
            continue;
        }
        if (prc > 0) {
            int irc = mqtt_input(&s_client);
            if (irc != 0) {
                printk("[NET] mqtt_input error %d\n", irc);
                mqtt_disconnect(&s_client, NULL);
                s_mqtt_connected = false;
                s_mqtt_ok = false;
                continue;
            }
        }
        if (mqtt_live(&s_client) != 0) {
            printk("[NET] keepalive failed; reconnecting\n");
            mqtt_disconnect(&s_client, NULL);
            s_mqtt_connected = false;
            s_mqtt_ok = false;
            continue;
        }

        /* --- Publish acknowledgments immediately -------------------------- */
        app_ack_t ack;
        while (app_ack_get(&ack, K_NO_WAIT)) {
            char json[TELEMETRY_JSON_MAX];
            int rc = telemetry_build_ack(&ack.cmd, ack.accepted, ack.reason,
                                         ack.actual, ack.ts_ms, json, sizeof(json));
            if (rc == TELEMETRY_OK) {
                publish_json(APP_TOPIC_ACK, json, strlen(json));
            }
        }

        /* --- Publish aggregated telemetry every APP_MQTT_PERIOD_MS ------- */
        telemetry_snapshot_t snap;
        while (app_telemetry_get(&snap, K_NO_WAIT)) {
            latest = snap;
            have_latest = true; /* collapse the 200 ms stream into the latest */
        }
        if (have_latest && k_uptime_get() >= next_telemetry) {
            latest.wifi_ok = s_wifi_ok;
            latest.mqtt_ok = s_mqtt_ok;
            char json[TELEMETRY_JSON_MAX];
            int rc = telemetry_build_telemetry(&latest, APP_DEVICE_ID, json,
                                               sizeof(json));
            if (rc == TELEMETRY_OK) {
                publish_json(APP_TOPIC_TELEMETRY, json, strlen(json));
            }
            next_telemetry = k_uptime_get() + APP_MQTT_PERIOD_MS;
        }

        /* --- Status heartbeat --------------------------------------------- */
        if (k_uptime_get() >= next_status) {
            publish_status(true);
            next_status = k_uptime_get() + APP_STATUS_PERIOD_MS;
        }
    }
}

K_THREAD_DEFINE(net_thread, 4096, net_task_run, NULL, NULL, NULL, 4, 0, 0);
