/**
 * @file net_task.c
 * @brief P4 - Network transport task: RW007 Wi-Fi (SPI) -> Zephyr L2
 *        Ethernet -> DHCPv4, with link-loss supervision, exponential
 *        backoff reconnect and the SoftAP captive-portal provisioning
 *        branch.
 *
 * Bring-up sequence (mirrors the verified LAB_3 join_and_get_ip() pattern):
 *   1. Boot gate: park 8 s so the one-shot audio loopback demo finishes
 *      before SPI/Wi-Fi traffic competes for CPU/DMA bandwidth (documented
 *      acceptable simplification -- see the boot-gate comment below).
 *   2. rw007_wifi_init(): raw-register SPI2 bring-up + module reset.
 *   3. rw007_wifi_scan(): console log of the strongest discovered SSIDs.
 *   4. Credential selection: wifi_storage v2 record first (portal-saved
 *      Wi-Fi credentials), falling back to the secrets.h compile-time
 *      default when no record exists.
 *   5. rw007_netif_set_mac(station MAC) + rw007_wifi_connect(selected
 *      credentials), 3-attempt join burst with a short gap.
 *   6. rw007_netif_link(true) + net_dhcpv4_start(), wait up to 20 s for the
 *      NET_EVENT_IPV4_ADDR_ADD management event (DHCP lease).
 *   7. Publish net_online=true + the dotted-quad IP into monitor_state
 *      (under monitor_state_lock()) and printk "[NET] online ip=...".
 *
 * Supervisor loop (1 s poll of rw007_wifi_is_connected() while online): on
 * link loss the interface carrier is dropped, DHCP is stopped, net_online is
 * published as false and the task retries the join burst with exponential
 * backoff (2, 4, 8, 16, 30, 30... s cap).
 *
 * Captive-portal provisioning branch (port of the LAB_3 setup portal, 300 s
 * window because the form now also carries broker + mute controls):
 *   - Boot, no credentials at all (no flash record, secrets.h placeholder):
 *     the portal opens immediately (first-boot UX) and is re-offered after
 *     each backoff pause until credentials are provisioned, so the device
 *     can never end up permanently unreachable.
 *   - Boot, saved credentials but the 3-attempt join burst fails: the
 *     portal opens (LAB_3 while(!online) pattern).
 *   - Runtime, was online at least once: the portal opens after every 5th
 *     failed join burst (e.g. the router password changed) -- between
 *     windows the supervisor resumes backoff retries with the previous
 *     credentials. After a portal exit without save the same backoff
 *     retries continue.
 *   - POST /save hand-off: the portal runs INLINE on this thread, so its
 *     return value is the supervisor's rejoin signal (no semaphore). The
 *     submitted credentials are verified with a real STA join (connect-
 *     verify before saving, the LAB_3 rule); only a verified join stores
 *     all four values via wifi_storage_save_ex(). The MQTT broker change
 *     takes effect at mqtt_task's next reconnect (broker preference is read
 *     from wifi_storage there).
 *   - The portal publishes monitor_state.portal_active (UI footer AP flag);
 *     teardown ordering follows LAB_3: dhcpv4_server_stop -> ipv4 addr
 *     remove -> netif_link(false) -> softap stop -> restore STA MAC.
 *
 * The RW007 driver exposes no standalone hardware-reset entry point that is
 * safe to call twice (rw007_wifi_init() spawns a new driver thread on every
 * call), so after 5 consecutive failed join bursts the task logs a fault
 * line (with netif stats) and simply keeps retrying at the capped interval.
 *
 * RAM contract (see net_task.h): the DTCM migration of the audio ring
 * (audio_capture.c) plus the record-buffer shrink freed ~64 KB of main SRAM
 * to fund the networking CONFIG block in zephyr/prj.conf; this task's own
 * stack also lives in DTCM via the raw-buffer pattern below (bumped to
 * 2560 B for the portal call depth).
 */

#include "net_task.h"

#include "monitor_state.h"
#include "rw007_wifi.h"
#include "rw007_netif.h"
#include "wifi_portal.h"
#include "wifi_storage.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/dhcpv4.h>

/* --------------------------------------------------------------- credentials */
/* Prefer the git-ignored secrets.h (copy of secrets.h.example with real
 * values); fall back to the example placeholders so the build never breaks
 * for grading. The placeholders are detected at runtime with one hint line
 * and route boot into the setup portal instead of a doomed join. */
#if defined(__has_include)
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "your-wifi-ssid"
#endif

#ifndef WIFI_PASS
#define WIFI_PASS "your-wifi-password"
#endif

/* ------------------------------------------------------------- timing/limits */
#define NET_BOOT_GATE_S        8u    /* park while the boot audio demo runs   */
#define NET_DHCP_TIMEOUT_S     20u   /* lease wait (LAB_3 join pattern)       */
#define NET_POLL_PERIOD_S      1u    /* supervisor link poll                  */
#define NET_RETRY_MIN_S        2u    /* exponential backoff start             */
#define NET_RETRY_MAX_S        30u   /* exponential backoff cap               */
#define NET_INIT_RETRY_S       10u   /* RW007 hardware-init retry delay       */
#define NET_HW_FAULT_ATTEMPTS  5u    /* consecutive join fails -> fault log   */
#define NET_SCAN_TOP           5u    /* strongest SSIDs logged after a scan   */
#define NET_JOIN_ATTEMPTS      3u    /* join burst size (LAB_3 pattern)       */
#define NET_JOIN_RETRY_GAP_S   2u    /* pause between burst attempts          */
#define NET_PORTAL_WINDOW_S    300u  /* captive portal window (bigger form)   */
#define NET_PORTAL_EVERY_BURSTS 5u   /* runtime portal re-offer cadence       */

/* ----------------------------------------------------------------- API state */
/* Single-writer (this thread only) / multi-reader: net_online flips as a
 * whole and s_ip_str is only rewritten while going online, so no lock beyond
 * the monitor_state mutex (which guards the published mirror) is required. */
static volatile bool s_online;
static char s_ip_str[16] = "0.0.0.0";

/* Active credential set (this thread only; guarded by the supervisor loop
 * structure, no cross-thread access). */
static char s_ssid[WIFI_SSID_MAX_LEN];
static char s_pass[WIFI_PASS_MAX_LEN];
static bool s_have_creds;  /* a usable credential pair is loaded          */
static bool s_joined;      /* currently associated + leased               */
static bool s_ever_online; /* joined at least once since boot             */

/* Backoff / fault counters: file-scope statics so the portal helpers can
 * reset them (single writer: this thread). */
static uint32_t s_fail_count;
static uint32_t s_backoff_s = NET_RETRY_MIN_S;

/* ------------------------------------------------------- thread (DTCM stack) */
/* Raw aligned buffer cast to k_thread_stack_t: K_THREAD_STACK_DEFINE carries
 * its own section attribute and CANNOT be wrapped in a custom-section
 * attribute, so the DTCM placement needs this pattern. The +64 guard bytes
 * absorb Zephyr's thread-stack bookkeeping prologue. DTCM is CPU-access-only,
 * which is exactly what a thread stack needs (no DMA touches it). Stack is
 * 2560 B: the captive-portal call chain (poll loop + vsnprintf emit) needs
 * more headroom than the plain supervisor's 2048 B. */
#define NET_TASK_STACK_SIZE 2560u
#define NET_TASK_PRIORITY   7

static uint8_t __attribute__((section(".dtcm_noinit"), aligned(32)))
    s_net_stack[NET_TASK_STACK_SIZE + 64u];
static struct k_thread s_net_thread;
static bool s_thread_started; /* idempotency guard (main() is the only caller) */

/* DHCP lease notification: management-event callback -> semaphore. */
static K_SEM_DEFINE(s_ip_sem, 0, 1);
static struct net_mgmt_event_callback s_ip_cb;

static void ip_event(struct net_mgmt_event_callback *cb, uint64_t event,
                     struct net_if *iface)
{
    ARG_UNUSED(cb);
    ARG_UNUSED(iface);
    if (event == NET_EVENT_IPV4_ADDR_ADD) {
        k_sem_give(&s_ip_sem);
    }
}

/* ------------------------------------------------------------ state publish  */
static void publish_net_state(bool online)
{
    s_online = online;
    if (!online) {
        strcpy(s_ip_str, "0.0.0.0");
    }
    monitor_state_lock();
    monitor_state_get()->net_online = online;
    monitor_state_unlock();
}

/* Take the whole path down: carrier off + DHCP stop (LAB_3 pattern) and the
 * shared-state mirror. */
static void net_take_offline(void)
{
    struct net_if *iface = rw007_netif_get();
    if (iface != NULL) {
        rw007_netif_link(false);
        net_dhcpv4_stop(iface); /* no-op when DHCP is not running */
    }
    publish_net_state(false);
}

/* ------------------------------------------------------------- scan summary  */
/* The driver already printks every discovered AP; this sorts the deduplicated
 * result by RSSI (strongest first) and logs the top few as a one-glance
 * summary. Static buffer: 16 entries would cost 576 B of thread stack. */
static rw007_ap_entry_t s_scan_aps[RW007_MAX_SCAN_APS];

static void log_scan_top(void)
{
    const int n = rw007_wifi_get_scan_aps(s_scan_aps, RW007_MAX_SCAN_APS);
    const int top = (n < (int)NET_SCAN_TOP) ? n : (int)NET_SCAN_TOP;

    /* Selection sort, strongest RSSI first (only the first 'top' slots are
     * actually needed, but n <= 16 keeps the full pass trivial). */
    for (int i = 0; i < top; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++) {
            if (s_scan_aps[j].rssi > s_scan_aps[best].rssi) {
                best = j;
            }
        }
        if (best != i) {
            const rw007_ap_entry_t tmp = s_scan_aps[i];
            s_scan_aps[i] = s_scan_aps[best];
            s_scan_aps[best] = tmp;
        }
        printk("[NET] scan top %d: '%s' rssi %d\n", i + 1,
               s_scan_aps[i].ssid, s_scan_aps[i].rssi);
    }
}

/* --------------------------------------------------------- credential source */
/* Best available credentials: the wifi_storage v2 record (portal-saved)
 * first, else the secrets.h compile-time default (skipped when it is still
 * the placeholder). */
static void load_best_creds(void)
{
    s_have_creds = false;

    if (wifi_storage_read(s_ssid, s_pass)) {
        s_have_creds = true;
        printk("[NET] using saved credentials for '%s'\n", s_ssid);
        return;
    }
    if (strcmp(WIFI_SSID, "your-wifi-ssid") != 0) {
        strncpy(s_ssid, WIFI_SSID, sizeof(s_ssid) - 1u);
        s_ssid[sizeof(s_ssid) - 1u] = '\0';
        strncpy(s_pass, WIFI_PASS, sizeof(s_pass) - 1u);
        s_pass[sizeof(s_pass) - 1u] = '\0';
        s_have_creds = true;
        printk("[NET] using secrets.h credentials for '%s'\n", s_ssid);
    }
}

/* ------------------------------------------------------- associate + lease   */
/* Mirror of the verified LAB_3 main.c join_and_get_ip(): station MAC into the
 * netif, Wi-Fi association, carrier on + DHCP start, bounded lease wait.
 * Returns true only with a real IPv4 address in s_ip_str. */
static bool join_and_get_ip(void)
{
    struct net_if *iface = rw007_netif_get();
    if (iface == NULL) {
        return false;
    }

    /* Ensure the interface link address matches the hardware Station MAC. */
    uint8_t sta_mac[6];
    rw007_wifi_get_mac(sta_mac);
    rw007_netif_set_mac(sta_mac);

    if (!rw007_wifi_connect(s_ssid, s_pass)) {
        return false;
    }

    k_sem_reset(&s_ip_sem);
    rw007_netif_link(true);
    net_dhcpv4_start(iface);
    if (k_sem_take(&s_ip_sem, K_SECONDS(NET_DHCP_TIMEOUT_S)) != 0) {
        printk("[NET] associated but no DHCP lease\n");
        net_dhcpv4_stop(iface);
        rw007_netif_link(false);
        return false;
    }

    struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;
    if (ipv4 != NULL) {
        for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
            if (ipv4->unicast[i].ipv4.is_used) {
                const uint8_t *a =
                    ipv4->unicast[i].ipv4.address.in_addr.s4_addr;
                snprintf(s_ip_str, sizeof(s_ip_str), "%u.%u.%u.%u",
                         (unsigned)a[0], (unsigned)a[1],
                         (unsigned)a[2], (unsigned)a[3]);
                break;
            }
        }
    }
    return true;
}

/* Bounded join burst with a short gap between attempts (LAB_3
 * connect_with_retries pattern). */
static bool join_burst(uint32_t attempts)
{
    for (uint32_t attempt = 1u; attempt <= attempts; attempt++) {
        printk("[NET] join attempt %u/%u for '%s'\n",
               (unsigned)attempt, (unsigned)attempts, s_ssid);
        if (join_and_get_ip()) {
            return true;
        }
        if (attempt < attempts) {
            k_sleep(K_SECONDS(NET_JOIN_RETRY_GAP_S));
        }
    }
    return false;
}

/* ----------------------------------------------------- portal save hand-off  */
/* Verify-then-save (LAB_3 rule): the portal only captured the submission;
 * nothing is stored until the submitted credentials pass a real STA join.
 * A blank broker field preserves the previously saved broker preference. */
static void try_verify_and_save(const wifi_portal_result_t *res)
{
    printk("[NET] verifying submitted credentials for '%s'\n", res->ssid);

    strncpy(s_ssid, res->ssid, sizeof(s_ssid) - 1u);
    s_ssid[sizeof(s_ssid) - 1u] = '\0';
    strncpy(s_pass, res->pass, sizeof(s_pass) - 1u);
    s_pass[sizeof(s_pass) - 1u] = '\0';
    s_have_creds = true;

    if (!join_burst(NET_JOIN_ATTEMPTS)) {
        printk("[NET] submitted credentials did not join -- not saved\n");
        s_joined = false;
        s_have_creds = false;
        load_best_creds(); /* resume with the previous credential set */
        return;
    }

    /* Verified: record the join and persist the full v2 record. */
    s_joined = true;
    s_ever_online = true;
    s_fail_count = 0;
    s_backoff_s = NET_RETRY_MIN_S;

    const char *broker = res->broker_host;
    uint16_t broker_port = res->broker_port;
    char prev_host[WIFI_BROKER_HOST_MAX_LEN];
    uint16_t prev_port;

    if ((broker == NULL || broker[0] == '\0') &&
        wifi_storage_get_broker(prev_host, sizeof(prev_host), &prev_port)) {
        broker = prev_host; /* blank field = keep current broker */
        broker_port = prev_port;
    }

    if (wifi_storage_save_ex(res->ssid, res->pass,
                             (broker != NULL && broker[0] != '\0') ? broker : NULL,
                             broker_port)) {
        printk("[NET] credentials%s%s verified & saved to flash\n",
               (broker != NULL && broker[0] != '\0') ? " + broker " : "",
               (broker != NULL && broker[0] != '\0') ? broker : "");
    } else {
        printk("[NET] WARNING: flash save failed (credentials active for "
               "this boot only)\n");
    }
}

/* ------------------------------------------------------------------- thread  */
static void net_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    /* Boot gate: net_task_start() is called from main() after the audio path
     * is already live (audio_capture_start() precedes it), so no hand-off
     * flag is needed; the fixed park keeps the boot loopback demo
     * (record + playback + codec powerdown, ends ~8-9 s after boot) clear of
     * SPI/Wi-Fi traffic. Documented acceptable simplification. */
    k_sleep(K_SECONDS(NET_BOOT_GATE_S));

    /* One-time DHCP lease notification wiring (NOT per attempt, or the
     * callback list would grow on every reconnect). */
    net_mgmt_init_event_callback(&s_ip_cb, ip_event, NET_EVENT_IPV4_ADDR_ADD);
    net_mgmt_add_event_callback(&s_ip_cb);

    if (strcmp(WIFI_SSID, "your-wifi-ssid") == 0) {
        printk("[NET] secrets.h placeholders in use -- join the SoftAP "
               "'BabyMon-Setup' to provision credentials\n");
    }

    bool hw_up = false;
    wifi_portal_result_t res;

    load_best_creds();

    for (;;) {
        if (!hw_up) {
            hw_up = rw007_wifi_init();
            if (!hw_up) {
                printk("[NET] RW007 init failed, retry in %u s\n",
                       (unsigned)NET_INIT_RETRY_S);
                k_sleep(K_SECONDS(NET_INIT_RETRY_S));
                continue;
            }
            printk("[NET] RW007 ready: '%s' sn '%s'\n",
                   rw007_wifi_get_version(), rw007_wifi_get_sn());
            (void)rw007_wifi_scan();
            log_scan_top();
        }

        /* No credentials anywhere: first-boot UX -- portal immediately, and
         * re-offered after every backoff pause until provisioning succeeds
         * (the device would otherwise be unreachable forever). */
        if (!s_have_creds) {
            net_take_offline();
            printk("[NET] no credentials -- opening config portal "
                   "(%u s window)\n", (unsigned)NET_PORTAL_WINDOW_S);
            if (wifi_portal_run(NET_PORTAL_WINDOW_S, &res) && res.valid) {
                try_verify_and_save(&res);
                if (s_joined) {
                    continue; /* verified + saved: supervise at loop bottom */
                }
            }
            k_sleep(K_SECONDS(s_backoff_s));
            s_backoff_s = (s_backoff_s < NET_RETRY_MAX_S) ? (s_backoff_s * 2u)
                                                          : NET_RETRY_MAX_S;
            continue;
        }

        if (!s_joined) {
            if (join_burst(NET_JOIN_ATTEMPTS)) {
                s_joined = true;
                s_ever_online = true;
                s_fail_count = 0;
                s_backoff_s = NET_RETRY_MIN_S;
            } else {
                /* Join burst failed: exponential backoff reconnect, with the
                 * config portal opened (a) on boot when the saved
                 * credentials never joined, and (b) every 5th failed burst
                 * at runtime (e.g. changed router password). Between portal
                 * windows the supervisor retries the previous credentials. */
                net_take_offline();
                s_fail_count++;
                if (s_fail_count == NET_HW_FAULT_ATTEMPTS ||
                    (s_fail_count % NET_HW_FAULT_ATTEMPTS) == 0) {
                    /* The driver exposes no standalone reset/reinit entry
                     * point that is safe to call twice (rw007_wifi_init()
                     * spawns another driver thread on every call), so the
                     * supervisor just keeps retrying the join path at the
                     * capped interval. */
                    printk("[NET] fault: %u consecutive failed join bursts "
                           "-- no driver reset path, continuing retries\n",
                           (unsigned)s_fail_count);
                    rw007_netif_stats();
                }

                if (!s_ever_online ||
                    (s_fail_count % NET_PORTAL_EVERY_BURSTS) == 0u) {
                    printk("[NET] opening config portal (%u s window)\n",
                           (unsigned)NET_PORTAL_WINDOW_S);
                    if (wifi_portal_run(NET_PORTAL_WINDOW_S, &res) &&
                        res.valid) {
                        try_verify_and_save(&res);
                        if (s_joined) {
                            continue; /* supervise at loop bottom */
                        }
                    }
                    /* Portal exit without save (or failed verify): fall
                     * through to the backoff pause with the previous
                     * credentials. */
                }

                printk("[NET] join failed, retry in %u s\n",
                       (unsigned)s_backoff_s);
                k_sleep(K_SECONDS(s_backoff_s));
                s_backoff_s = (s_backoff_s < NET_RETRY_MAX_S)
                                  ? (s_backoff_s * 2u)
                                  : NET_RETRY_MAX_S;
                continue;
            }
        }

        /* Joined: supervise the link with a 1 s poll. */
        publish_net_state(true);
        printk("[NET] online ip=%s\n", s_ip_str);

        while (rw007_wifi_is_connected()) {
            k_sleep(K_SECONDS(NET_POLL_PERIOD_S));
        }

        /* Link lost: take the path down and start the retry cycle. */
        printk("[NET] dropped, retry in %u s\n", (unsigned)s_backoff_s);
        net_take_offline();
        s_joined = false;
        k_sleep(K_SECONDS(s_backoff_s));
        /* backoff stays at MIN (it was reset when the join succeeded), so
         * the first post-drop retry happens quickly. */
    }
}

/* ---------------------------------------------------------------- public API */
void net_task_start(void)
{
    if (s_thread_started) {
        return; /* idempotent */
    }
    s_thread_started = true;
    k_thread_create(&s_net_thread, (k_thread_stack_t *)s_net_stack,
                    NET_TASK_STACK_SIZE, net_thread_fn,
                    NULL, NULL, NULL, NET_TASK_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&s_net_thread, "net_thread");
}

bool net_task_is_online(void)
{
    return s_online;
}

const char *net_task_get_ip(void)
{
    return s_ip_str;
}
