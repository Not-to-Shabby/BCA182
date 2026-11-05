/**
 * @file wifi_portal.h
 * @brief Wi-Fi onboarding portal (P4): SoftAP "BabyMon-Setup" + DHCPv4 server
 *        + captive DNS + HTTP form on http://192.168.4.1.
 *
 * Ported from the LAB_3 Edge AI Activity Tracker setup portal and EXTENDED
 * for the Smart Baby Monitoring System: the form now also captures the MQTT
 * broker preference (host + port) and offers a buzzer-silence control
 * (POST /mute mirrors the remote-mute path of mqtt_task.c apply_cmd()).
 *
 * Contract (LAB_3 rule preserved): wifi_portal_run() NEVER writes to flash.
 * POST /save only captures the submitted values and ends the portal loop;
 * the caller (net_task supervisor) tears down, verifies the credentials with
 * a real STA join, and only then stores them via wifi_storage_save_ex().
 * The portal->supervisor hand-off for the rejoin is simply the return value
 * of wifi_portal_run() -- the portal runs inline on the supervisor thread,
 * so no cross-thread semaphore is required.
 *
 * UI: the LAB_3 ui_dashboard_show_portal()/show_connecting() hooks are NOT
 * ported; instead the portal publishes monitor_state.portal_active
 * (true from SoftAP start to teardown completion) under the monitor-state
 * lock and the UI thread renders the footer AP flag from its snapshot.
 */

#ifndef WIFI_PORTAL_H_
#define WIFI_PORTAL_H_

#include <stdint.h>
#include <stdbool.h>
#include "wifi_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Values captured from a POST /save submission. */
typedef struct {
    char     ssid[WIFI_SSID_MAX_LEN];        /* non-empty on valid          */
    char     pass[WIFI_PASS_MAX_LEN];        /* empty = open AP             */
    char     broker_host[WIFI_BROKER_HOST_MAX_LEN]; /* empty = keep previous */
    uint16_t broker_port;                    /* 0 = not provided            */
    bool     valid;                          /* /save submitted and parsed  */
} wifi_portal_result_t;

/**
 * @brief Runs the captive portal until a /save submission or the timeout.
 *
 * Starts the SoftAP ("BabyMon-Setup", open), the DHCPv4 server
 * (192.168.4.2..), the wildcard captive-DNS responder (UDP 53 -> 192.168.4.1)
 * and the HTTP server (TCP 80). Serves the SSID-dropdown + password + broker
 * form, answers OS captive-probe endpoints with 302 redirects, applies
 * POST /mute directly, and returns on POST /save.
 *
 * @param timeout_sec Window in seconds; 0 waits forever.
 * @param out         Receives the submitted values (valid=true) when the
 *                    phone submitted /save.
 * @return true when a /save submission was captured (out->valid). Nothing is
 *         saved to flash here -- the caller must verify-join first.
 */
bool wifi_portal_run(uint32_t timeout_sec, wifi_portal_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_PORTAL_H_ */
