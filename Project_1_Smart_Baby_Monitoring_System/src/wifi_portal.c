/**
 * ==============================================================================
 * Wi-Fi onboarding portal (SoftAP + DHCPv4 server + captive DNS + HTTP form)
 * ==============================================================================
 * Ported from the LAB_3 Edge AI Activity Tracker wifi_portal.c and EXTENDED
 * for the P4 Smart Baby Monitoring System.
 *
 * Flow: the phone joins the open "BabyMon-Setup" SoftAP, receives a
 * 192.168.4.x lease from the Zephyr DHCPv4 server, resolves any hostname to
 * 192.168.4.1 through the wildcard DNS responder, and the OS captive-portal
 * probes are answered with 302 redirects so the form pops up automatically.
 * The form captures Wi-Fi credentials + MQTT broker preference (POST /save)
 * and offers a buzzer-silence control (POST /mute).
 *
 * Extensions vs LAB_3:
 *   - /save additionally parses "broker" + "bport" into wifi_portal_result_t;
 *     storage still happens ONLY after the caller's verified STA join.
 *   - /mute applies a 30 min / 1 h / 2 h / 8 h buzzer silence directly,
 *     mirroring the mqtt_task.c apply_cmd() pattern (baby_logic_acknowledge_for
 *     + mute_until_ms mirror + buzzer_on re-evaluation under the state lock).
 *   - LAB_3's ui_dashboard_show_portal()/show_connecting() hooks are replaced
 *     by publishing monitor_state.portal_active (UI renders the footer flag).
 *   - Streaming responses through a small chunk buffer instead of LAB_3's
 *     1.6 KB page + 1.8 KB response statics (RAM headroom on this board is
 *     ~6.5 KB), and 2 connection slots instead of 4.
 *
 * Teardown ordering (LAB_3, preserved): close client conns -> close listeners
 * -> grace sleep (phone finishes reading the confirmation page) ->
 * dhcpv4_server_stop -> ipv4 addr remove -> netif_link(false) -> softap stop
 * -> restore STA MAC.
 * ==============================================================================
 */

#include "wifi_portal.h"
#include "rw007_wifi.h"
#include "rw007_netif.h"
#include "monitor_state.h"
#include "baby_logic.h"

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/dhcpv4_server.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

/* Broker placeholder source: prefer the saved v2 record, else the secrets.h
 * default (same __has_include pattern as net_task.c / mqtt_task.c). */
#if defined(__has_include)
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#endif

#ifndef MQTT_BROKER_HOST
#define MQTT_BROKER_HOST "test.mosquitto.org"
#endif

#ifndef MQTT_BROKER_PORT
#define MQTT_BROKER_PORT 1883u
#endif

#define SETUP_AP_SSID   "BabyMon-Setup"
#define SETUP_PORTAL_IP "192.168.4.1"
#define REQ_MAX         1024
#define MAX_CONN        2
#define CONN_IDLE_MS    8000

/* ---------------------------------------------------------- portal state */
static wifi_portal_result_t s_result;
static volatile bool s_got_save;

/* ------------------------------------------------------------ HTML helpers */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Extracts and URL-decodes one field from an application/x-www-form-urlencoded body. */
static bool form_field(const char *body, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = body;

    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t o = 0;
            while (*p && *p != '&' && o + 1 < cap) {
                if (*p == '+') {
                    out[o++] = ' ';
                    p++;
                } else if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
                    out[o++] = (char)(hexval(p[1]) * 16 + hexval(p[2]));
                    p += 3;
                } else {
                    out[o++] = *p++;
                }
            }
            out[o] = '\0';
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

/* Escape &, <, >, ", ' so SSIDs / broker hosts cannot break the markup. */
static void html_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 7 < cap; p++) {
        const char *rep = NULL;
        if (*p == '&') rep = "&amp;";
        else if (*p == '<') rep = "&lt;";
        else if (*p == '>') rep = "&gt;";
        else if (*p == '"') rep = "&quot;";
        else if (*p == '\'') rep = "&#39;";
        if (rep) {
            size_t rl = strlen(rep);
            memcpy(out + o, rep, rl);
            o += rl;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------- socket IO  */
/* Sends data with an overall deadline. */
static void send_all(int fd, const char *data, size_t len)
{
    int64_t give_up = k_uptime_get() + 4000;

    while (len > 0 && k_uptime_get() < give_up) {
        ssize_t n = zsock_send(fd, data, len, 0);

        if (n > 0) {
            data += n;
            len -= (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                             errno == ENOMEM || errno == ENOBUFS)) {
            k_msleep(10);
        } else {
            return; /* hard send error: caller closes anyway */
        }
    }
}

/* Streaming emit: one fmt chunk per call, written straight after a fixed
 * 200 OK header (body is delimited by Connection: close + FIN). The portal
 * is single-threaded, so the shared chunk buffer and header latch need no
 * lock. http_begin() arms the latch at the start of each streamed response. */
static bool s_http_header_pending;

static void http_begin(void)
{
    s_http_header_pending = true;
}

static void http_emit(int fd, const char *fmt, ...)
{
    static char chunk[640]; /* every emitted chunk stays below this cap */

    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(chunk, sizeof(chunk), fmt, ap);
    va_end(ap);
    if (len < 0) {
        return;
    }
    if (s_http_header_pending) {
        static const char HDR[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Connection: close\r\n\r\n";
        send_all(fd, HDR, sizeof(HDR) - 1);
        s_http_header_pending = false;
    }
    send_all(fd, chunk, (size_t)((len < (int)sizeof(chunk)) ? len : (int)sizeof(chunk) - 1));
}

/* Small fixed pages (save confirmation, mute confirmation) can carry an
 * exact Content-Length. */
static void send_static_page(int fd, const char *body)
{
    char hdr[128];

    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: %u\r\n"
                     "Connection: close\r\n\r\n",
                     (unsigned)strlen(body));
    send_all(fd, hdr, (size_t)n);
    send_all(fd, body, strlen(body));
}

static bool request_complete(const char *req, size_t got)
{
    const char *hdr_end = strstr(req, "\r\n\r\n");

    if (!hdr_end) {
        return false;
    }
    if (strncmp(req, "POST", 4) != 0) {
        return true;
    }
    const char *cl = strstr(req, "Content-Length:");
    size_t want = cl ? (size_t)atoi(cl + 15) : 0;

    return got - (size_t)(hdr_end + 4 - req) >= want;
}

/* ---------------------------------------------------------- portal page   */
static const char PAGE_STYLE[] =
    "<style>"
    "body{font-family:sans-serif;background:#0b0f19;color:#fff;text-align:center;padding:20px;margin:0}"
    ".c{background:#161f30;border:1px solid #334155;border-radius:12px;padding:20px;max-width:320px;margin:auto}"
    "label{color:#94a3b8;font-size:13px;display:block;text-align:left;margin-bottom:2px}"
    "select,input,button{width:100%;padding:12px;margin:8px 0;border-radius:8px;font-size:16px;box-sizing:border-box}"
    "select,input{border:1px solid #334155;background:#0f172a;color:#fff}"
    "button{background:#10b981;border:0;color:#fff;font-weight:bold;cursor:pointer}"
    "button.mute{background:#f59e0b}hr{border:0;border-top:1px solid #334155;margin:18px 0}"
    "</style>";

/* Current broker preference for the form placeholders: saved v2 value, else
 * the secrets.h compile-time default. */
static void current_broker(char *host, size_t cap, uint16_t *port)
{
    if (wifi_storage_get_broker(host, cap, port)) {
        return;
    }
    strncpy(host, MQTT_BROKER_HOST, cap - 1);
    host[cap - 1] = '\0';
    *port = (uint16_t)MQTT_BROKER_PORT;
}

static void send_portal_page(int fd)
{
    static rw007_ap_entry_t aps[RW007_MAX_SCAN_APS];
    const int n_aps = rw007_wifi_get_scan_aps(aps, RW007_MAX_SCAN_APS);

    char broker_ph[WIFI_BROKER_HOST_MAX_LEN];
    char esc[WIFI_BROKER_HOST_MAX_LEN + 40];
    char ssid_esc[sizeof(aps[0].ssid) + 40];
    uint16_t broker_port;

    current_broker(broker_ph, sizeof(broker_ph), &broker_port);
    html_escape(broker_ph, esc, sizeof(esc));
    http_begin();
    http_emit(fd,
        "<!DOCTYPE html><html><head>"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Baby Monitor Setup</title>");
    http_emit(fd, PAGE_STYLE);
    http_emit(fd,
        "</head><body><div class=\"c\">"
        "<h2 style=\"color:#10b981;margin-top:0\">Baby Monitor Setup</h2>"
        "<form method=\"POST\" action=\"/save\">"
        "<label>Wi-Fi network:</label>"
        "<select onchange=\"if(this.value)document.getElementById('ssid').value=this.value;\">"
        "<option value=\"\">-- Select Network (%d found) --</option>",
        n_aps);

    for (int i = 0; i < n_aps; i++) {
        html_escape(aps[i].ssid, ssid_esc, sizeof(ssid_esc));
        http_emit(fd, "<option value=\"%s\">%s (%d dBm)</option>",
                  ssid_esc, ssid_esc, aps[i].rssi);
    }

    http_emit(fd,
        "</select>"
        "<input id=\"ssid\" name=\"ssid\" placeholder=\"Or enter SSID manually\" required>"
        "<input name=\"pass\" type=\"password\" placeholder=\"Wi-Fi Password\">"
        "<label>MQTT broker host (blank = keep current):</label>"
        "<input name=\"broker\" placeholder=\"%s\">"
        "<label>MQTT broker port:</label>"
        "<input name=\"bport\" type=\"number\" value=\"%u\">"
        "<button type=\"submit\">Save &amp; Connect</button>"
        "</form><hr>",
        esc, (unsigned)broker_port);

    /* Buzzer-silence section (POST /mute, mirrors the remote-mute path). */
    http_emit(fd,
        "<h2 style=\"color:#f59e0b\">Buzzer</h2>"
        "<form method=\"POST\" action=\"/mute\">"
        "<label>Silence buzzer for:</label>"
        "<select name=\"min\">"
        "<option value=\"30\">30 minutes</option>"
        "<option value=\"60\">1 hour</option>"
        "<option value=\"120\">2 hours</option>"
        "<option value=\"480\">8 hours</option>"
        "</select>"
        "<button type=\"submit\" class=\"mute\">Silence</button>"
        "</form></div></body></html>");
}

/* ------------------------------------------------------------- handlers   */
static const char PAGE_MUTE[] =
    "<!DOCTYPE html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<style>body{background:#0B0F19;color:#F59E0B;font-family:sans-serif;text-align:center;padding:40px}"
    "a{color:#10B981}</style></head><body><h2>Buzzer silenced</h2>"
    "<p style=color:#fff>The alarm buzzer is muted for the selected duration.</p>"
    "<p><a href=\"http://" SETUP_PORTAL_IP "/\">Back to setup page</a></p></body></html>";

/* Exact mirror of mqtt_task.c apply_cmd()'s mute path (minus the MQTT-side
 * clamp -- the dropdown only offers fixed 30 min..8 h values). */
static void portal_apply_mute(uint32_t mute_ms)
{
    const uint32_t now = k_uptime_get_32();

    monitor_state_lock();
    monitor_state_t *st = monitor_state_get();
    baby_logic_acknowledge_for(now, mute_ms);
    st->mute_until_ms = now + mute_ms;
    st->buzzer_on = baby_logic_buzzer_on(st->state, now);
    monitor_state_unlock();
}

static void handle_mute(int fd, const char *body)
{
    char val[8];
    uint32_t minutes = 30;

    if (form_field(body, "min", val, sizeof(val))) {
        long v = atol(val);
        if (v >= 1 && v <= 480) {
            minutes = (uint32_t)v;
        }
    }

    portal_apply_mute(minutes * 60u * 1000u);
    printk("[PORTAL] buzzer muted for %u min via portal\n", (unsigned)minutes);
    send_static_page(fd, PAGE_MUTE);
}

static void handle_save(int fd, const char *body)
{
    memset(&s_result, 0, sizeof(s_result));

    if (form_field(body, "ssid", s_result.ssid, sizeof(s_result.ssid)) &&
        s_result.ssid[0] != '\0') {
        if (!form_field(body, "pass", s_result.pass, sizeof(s_result.pass))) {
            s_result.pass[0] = '\0';
        }
        if (!form_field(body, "broker", s_result.broker_host,
                        sizeof(s_result.broker_host))) {
            s_result.broker_host[0] = '\0';
        }
        s_result.broker_port = 0;
        char port_val[8];
        if (form_field(body, "bport", port_val, sizeof(port_val))) {
            long p = atol(port_val);
            if (p > 0 && p <= 65535) {
                s_result.broker_port = (uint16_t)p;
            }
        }
        /* Empty host keeps the previously saved broker preference: the form
         * placeholder shows the current value, so blank = "keep current". */
        s_result.valid = true;
        s_got_save = true;
        printk("[PORTAL] save: ssid '%s' broker '%s' port %u\n",
               s_result.ssid,
               s_result.broker_host[0] ? s_result.broker_host : "(keep)",
               (unsigned)s_result.broker_port);
        send_static_page(fd,
            "<!DOCTYPE html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\">"
            "<style>body{background:#0B0F19;color:#10B981;font-family:sans-serif;text-align:center;padding:40px}</style>"
            "</head><body><h2>Saved</h2><p style=color:#fff>The board is joining your network now.</p>"
            "<p>Reconnect your phone to your normal Wi-Fi.</p></body></html>");
        return;
    }
    /* Malformed /save: fall through to the form. */
    send_portal_page(fd);
}

static void handle_request(int fd, char *req)
{
    char *eol = strstr(req, "\r\n");

    if (eol) {
        *eol = '\0';
        printk("[HTTP] %s\n", req);
        *eol = '\r';
    }

    /* Automatic Captive Portal Detection Endpoints (Android / iOS / Windows) */
    if (strncmp(req, "GET /generate_204", 17) == 0 ||
        strncmp(req, "GET /gen_204", 12) == 0 ||
        strncmp(req, "GET /hotspot-detect.html", 24) == 0 ||
        strncmp(req, "GET /ncsi.txt", 13) == 0 ||
        strncmp(req, "GET /connecttest.txt", 20) == 0 ||
        strncmp(req, "GET /canonical.html", 19) == 0) {
        static const char REDIR[] =
            "HTTP/1.1 302 Found\r\n"
            "Location: http://" SETUP_PORTAL_IP "/\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        send_all(fd, REDIR, sizeof(REDIR) - 1);
        return;
    }

    const char *body = strstr(req, "\r\n\r\n");

    if (strncmp(req, "POST /save", 10) == 0 && body) {
        handle_save(fd, body + 4);
        return;
    }
    if (strncmp(req, "POST /mute", 10) == 0 && body) {
        handle_mute(fd, body + 4);
        return;
    }
    /* Any other request, including the OS captive-portal probes, gets the form. */
    send_portal_page(fd);
}

/* ----------------------------------------------------------- conn slots   */
struct conn {
    int fd;
    size_t len;
    int64_t deadline;
    char buf[REQ_MAX];
};

static struct conn s_conn[MAX_CONN];

static void conn_close(struct conn *c)
{
    if (c->fd >= 0) {
        zsock_close(c->fd);
        c->fd = -1;
    }
}

static void conn_accept(int lfd)
{
    int cfd = zsock_accept(lfd, NULL, NULL);
    struct conn *slot = NULL;

    if (cfd < 0) {
        return;
    }
    int nodelay = 1;
    zsock_setsockopt(cfd, NET_IPPROTO_TCP, ZSOCK_TCP_NODELAY, &nodelay, sizeof(nodelay));
    for (int i = 0; i < MAX_CONN; i++) {
        if (s_conn[i].fd < 0) {
            slot = &s_conn[i];
            break;
        }
    }
    if (!slot) { /* full: drop the connection that has been idle the longest */
        slot = &s_conn[0];
        for (int i = 1; i < MAX_CONN; i++) {
            if (s_conn[i].deadline < slot->deadline) {
                slot = &s_conn[i];
            }
        }
        conn_close(slot);
    }
    slot->fd = cfd;
    slot->len = 0;
    slot->buf[0] = '\0';
    slot->deadline = k_uptime_get() + CONN_IDLE_MS;
}

static void conn_service(struct conn *c)
{
    ssize_t n = zsock_recv(c->fd, c->buf + c->len, REQ_MAX - 1 - c->len, ZSOCK_MSG_DONTWAIT);

    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        conn_close(c);
        return;
    }
    if (n > 0) {
        c->len += (size_t)n;
        c->buf[c->len] = '\0';
        if (request_complete(c->buf, c->len) || c->len >= REQ_MAX - 1) {
            handle_request(c->fd, c->buf);
            c->len = 0;
            zsock_shutdown(c->fd, ZSOCK_SHUT_WR);
            /* Give client time to receive data and ACK before closing */
            c->deadline = k_uptime_get() + 2500;
        }
    }
}

/* --------------------------------------------------------- DNS responder  */
static int open_dns_listener(void)
{
    struct net_sockaddr_in a = { 0 };
    int one = 1;
    int fd = zsock_socket(NET_AF_INET, NET_SOCK_DGRAM, NET_IPPROTO_UDP);

    if (fd < 0) return -1;
    zsock_setsockopt(fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_REUSEADDR, &one, sizeof(one));
    a.sin_family = NET_AF_INET;
    a.sin_port = net_htons(53);
    if (zsock_bind(fd, (struct net_sockaddr *)&a, sizeof(a)) < 0) {
        zsock_close(fd);
        return -1;
    }
    return fd;
}

static void serve_dns_query(int fd)
{
    struct net_sockaddr_in client;
    net_socklen_t addrlen = sizeof(client);
    uint8_t buf[256];

    ssize_t len = zsock_recvfrom(fd, buf, sizeof(buf) - 16, ZSOCK_MSG_DONTWAIT,
                                 (struct net_sockaddr *)&client, &addrlen);
    if (len >= 12) {
        /* Standard DNS answer resolving any domain query to 192.168.4.1 */
        buf[2] = 0x81; /* Response flag */
        buf[3] = 0x80; /* Recursion available, no error */
        buf[6] = 0x00;
        buf[7] = 0x01; /* 1 Answer RR */
        buf[8] = 0x00; buf[9] = 0x00;
        buf[10] = 0x00; buf[11] = 0x00;

        static const uint8_t ans[16] = {
            0xC0, 0x0C,             /* Pointer to QNAME at byte 12 */
            0x00, 0x01,             /* Type A */
            0x00, 0x01,             /* Class IN */
            0x00, 0x00, 0x00, 0x3C, /* TTL 60s */
            0x00, 0x04,             /* Data len 4 */
            192, 168, 4, 1          /* IP: 192.168.4.1 */
        };
        memcpy(buf + len, ans, sizeof(ans));
        zsock_sendto(fd, buf, len + sizeof(ans), 0,
                     (struct net_sockaddr *)&client, addrlen);
    }
}

static int open_listener(void)
{
    struct net_sockaddr_in a = { 0 };
    int one = 1;
    int fd = zsock_socket(NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TCP);

    if (fd < 0) return -1;
    zsock_setsockopt(fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_REUSEADDR, &one, sizeof(one));
    a.sin_family = NET_AF_INET;
    a.sin_port = net_htons(80);
    if (zsock_bind(fd, (struct net_sockaddr *)&a, sizeof(a)) < 0 || zsock_listen(fd, 2) < 0) {
        zsock_close(fd);
        return -1;
    }
    return fd;
}

/* ------------------------------------------------------------- UI mirror  */
/* LAB_3 rendered the portal on the LCD from inside the portal; here the UI
 * thread owns the display, so the portal only publishes the flag. */
static void set_portal_active(bool active)
{
    monitor_state_lock();
    monitor_state_get()->portal_active = active;
    monitor_state_unlock();
}

/* -------------------------------------------------------------- main loop */
bool wifi_portal_run(uint32_t timeout_sec, wifi_portal_result_t *out)
{
    struct net_if *iface = rw007_netif_get();
    struct net_in_addr ap_ip, mask, pool;

    printk("\n[PORTAL] SoftAP '%s'  ->  http://%s\n", SETUP_AP_SSID, SETUP_PORTAL_IP);

    if (!iface) {
        printk("[PORTAL] no network interface\n");
        return false;
    }
    if (!rw007_wifi_start_softap(SETUP_AP_SSID, NULL)) {
        printk("[PORTAL] SoftAP failed to start\n");
        return false;
    }
    set_portal_active(true);

    net_addr_pton(NET_AF_INET, "192.168.4.1", &ap_ip);
    net_addr_pton(NET_AF_INET, "255.255.255.0", &mask);
    net_addr_pton(NET_AF_INET, "192.168.4.2", &pool);

    {
        uint8_t ap_mac[6];

        rw007_wifi_get_ap_mac(ap_mac);
        rw007_netif_set_mac(ap_mac);
    }
    rw007_netif_link(true);
    if (!net_if_ipv4_addr_add(iface, &ap_ip, NET_ADDR_MANUAL, 0)) {
        printk("[PORTAL] could not set " SETUP_PORTAL_IP "\n");
    }
    net_if_ipv4_set_netmask_by_addr(iface, &ap_ip, &mask);
    net_if_ipv4_set_gw(iface, &ap_ip);

    int rc = net_dhcpv4_server_start(iface, &pool);
    printk("[PORTAL] DHCP server start -> %d (leases from 192.168.4.2)\n", rc);

    int lfd = open_listener();
    if (lfd < 0) {
        printk("[PORTAL] cannot open HTTP listener\n");
        net_dhcpv4_server_stop(iface);
        rw007_wifi_stop_softap();
        set_portal_active(false);
        return false;
    }
    printk("[PORTAL] HTTP server listening on port 80\n");

    int dns_fd = open_dns_listener();
    if (dns_fd >= 0) {
        printk("[PORTAL] Captive DNS responder active on UDP port 53\n");
    }

    s_got_save = false;
    memset(&s_result, 0, sizeof(s_result));
    for (int i = 0; i < MAX_CONN; i++) {
        s_conn[i].fd = -1;
    }
    int64_t end = timeout_sec ? k_uptime_get() + (int64_t)timeout_sec * 1000 : INT64_MAX;
    int64_t next_stats = k_uptime_get() + 5000;

    while (!s_got_save && k_uptime_get() < end) {
        struct zsock_pollfd pfd[2 + MAX_CONN];
        int nfd = 0;

        pfd[0].fd = lfd;
        pfd[0].events = ZSOCK_POLLIN;
        pfd[0].revents = 0;
        nfd = 1;

        int dns_idx = -1;
        if (dns_fd >= 0) {
            dns_idx = nfd;
            pfd[nfd].fd = dns_fd;
            pfd[nfd].events = ZSOCK_POLLIN;
            pfd[nfd].revents = 0;
            nfd++;
        }

        for (int i = 0; i < MAX_CONN; i++) {
            if (s_conn[i].fd >= 0) {
                pfd[nfd].fd = s_conn[i].fd;
                pfd[nfd].events = ZSOCK_POLLIN;
                pfd[nfd].revents = 0;
                nfd++;
            }
        }

        if (k_uptime_get() >= next_stats) {
            rw007_netif_stats();
            next_stats += 5000;
        }

        if (zsock_poll(pfd, nfd, 200) > 0) {
            if (pfd[0].revents & ZSOCK_POLLIN) {
                conn_accept(lfd);
            }
            if (dns_idx >= 0 && (pfd[dns_idx].revents & ZSOCK_POLLIN)) {
                serve_dns_query(dns_fd);
            }
            for (int i = 0; i < MAX_CONN && !s_got_save; i++) {
                if (s_conn[i].fd < 0) {
                    continue;
                }
                for (int k = (dns_idx >= 0 ? 2 : 1); k < nfd; k++) {
                    if (pfd[k].fd == s_conn[i].fd && pfd[k].revents) {
                        conn_service(&s_conn[i]);
                        break;
                    }
                }
            }
        }
        for (int i = 0; i < MAX_CONN; i++) {
            if (s_conn[i].fd >= 0 && k_uptime_get() > s_conn[i].deadline) {
                conn_close(&s_conn[i]);
            }
        }
    }

    for (int i = 0; i < MAX_CONN; i++) {
        conn_close(&s_conn[i]);
    }
    zsock_close(lfd);
    if (dns_fd >= 0) {
        zsock_close(dns_fd);
    }
    k_msleep(1500); /* let the phone finish reading the confirmation page */
    net_dhcpv4_server_stop(iface);
    net_if_ipv4_addr_rm(iface, &ap_ip);
    rw007_netif_link(false);
    rw007_wifi_stop_softap();
    set_portal_active(false);

    /* Restore Station MAC address for client mode */
    {
        uint8_t sta_mac[6];
        rw007_wifi_get_mac(sta_mac);
        rw007_netif_set_mac(sta_mac);
    }
    k_msleep(300);

    if (!s_got_save) {
        printk("[PORTAL] closed without a submission\n");
        return false;
    }
    if (out) {
        *out = s_result;
    }
    return true;
}
