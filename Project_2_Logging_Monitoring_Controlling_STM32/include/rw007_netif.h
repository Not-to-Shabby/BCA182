#ifndef RW007_NETIF_H
#define RW007_NETIF_H

#include <stdbool.h>
#include <stdint.h>

struct net_if;

/** The Zephyr network interface backed by the RW007 (valid after boot). */
struct net_if *rw007_netif_get(void);

/** Reflect the Wi-Fi link state (SoftAP up / station associated) into the stack. */
void rw007_netif_link(bool up);

/** Set the interface MAC (station and SoftAP use different addresses). */
void rw007_netif_set_mac(const uint8_t mac[6]);

/** Print frame counters to the console. */
void rw007_netif_stats(void);

#endif /* RW007_NETIF_H */
