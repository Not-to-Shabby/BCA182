/**
 * @file net_task.h
 * @brief Network transport task: RW007 Wi-Fi (SPI) -> L2 Ethernet -> DHCP,
 *        with link-loss supervision, exponential backoff and device-fault
 *        states. P4 Smart Baby Monitor.
 *
 * Brings up the RW007 (LAB_3 native SPI driver, raw-register core), joins
 * the configured Wi-Fi (secrets.h, saved to flash by wifi_storage), starts
 * DHCPv4, and keeps the link alive. Publishes state to monitor_state
 * (net_online) and surfaces never-connected/dropped/fault states.
 *
 * RAM contract: this module enables the Zephyr networking + MQTT CONFIG
 * block; the audio ring/record buffers must live in DTCM first (the
 * capture-thread refactor frees ~35 KB of main SRAM for the net pools).
 */
#ifndef NET_TASK_H_
#define NET_TASK_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** No-op: thread is created at build time (K_THREAD_DEFINE) and parks
 *  until the audio path is up, then runs the connection supervisor. */
void net_task_start(void);

/** True when associated + DHCP lease held + link carrier on. */
bool net_task_is_online(void);

/** Dotted-quad string of the current IPv4 (or "0.0.0.0"). */
const char *net_task_get_ip(void);

/** Request the config portal (e.g. KEY0 held 3 s). The supervisor opens
 *  the SoftAP at its next loop iteration (dropping STA if associated). */
void net_task_request_portal(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_TASK_H_ */
