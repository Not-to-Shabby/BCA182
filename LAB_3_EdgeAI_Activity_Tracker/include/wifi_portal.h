/**
 * Wi-Fi onboarding portal: SoftAP "Spark-Setup" + DHCP server + HTTP form.
 */

#ifndef WIFI_PORTAL_H
#define WIFI_PORTAL_H

#include <stdint.h>
#include <stdbool.h>
#include "wifi_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Runs the portal until the phone submits credentials or the timeout expires.
 *
 * @param timeout_sec 0 waits forever.
 * @param out_ssid    Receives the submitted SSID (WIFI_SSID_MAX_LEN bytes).
 * @param out_pass    Receives the submitted password (WIFI_PASS_MAX_LEN bytes).
 * @return true if credentials were submitted. Nothing is saved here; the caller
 *         should join first and store only credentials that actually work.
 */
bool wifi_portal_run(uint32_t timeout_sec, char *out_ssid, char *out_pass);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_PORTAL_H */
