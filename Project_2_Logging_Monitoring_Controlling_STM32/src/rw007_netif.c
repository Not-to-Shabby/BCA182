/**
 * ==============================================================================
 * RW007 as a Zephyr Ethernet network interface
 * ==============================================================================
 * The RW007 exchanges raw Ethernet frames with the host. This file registers it
 * with Zephyr's L2 Ethernet layer so the stock IPv4/UDP/TCP/DHCP/socket stack
 * runs on top of it.
 */

#include "rw007_wifi.h"
#include "rw007_netif.h"
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_core.h>
#include <string.h>

static struct net_if *s_iface;
static uint8_t s_tx_frame[1520];

static uint32_t s_rx_frames, s_rx_drop, s_tx_frames, s_tx_fail;

static void dump_frame(const char *dir, const uint8_t *f, uint16_t len)
{
    /* Ignore noisy background multicast (mDNS 01:00:5e:00:00:fb and IGMP) */
    if (f[0] == 0x01 && f[1] == 0x00 && f[2] == 0x5E) {
        return;
    }

    if (len >= 14 && f[12] == 0x08 && f[13] == 0x00 && len >= 34) {
        uint8_t proto = f[23];
        if (proto == 6 && len >= 48) { /* TCP */
            uint16_t sport = (f[34] << 8) | f[35];
            uint16_t dport = (f[36] << 8) | f[37];
            uint8_t flags = f[47];
            printk("[NET] %s TCP %u->%u len=%u [%s%s%s%s%s]\n",
                   dir, sport, dport, len,
                   (flags & 0x02) ? "SYN " : "",
                   (flags & 0x10) ? "ACK " : "",
                   (flags & 0x08) ? "PSH " : "",
                   (flags & 0x01) ? "FIN " : "",
                   (flags & 0x04) ? "RST " : "");
            return;
        } else if (proto == 17 && len >= 42) { /* UDP */
            uint16_t sport = (f[34] << 8) | f[35];
            uint16_t dport = (f[36] << 8) | f[37];
            if (sport == 5353 || dport == 5353) return; /* ignore mDNS */
            printk("[NET] %s UDP %u->%u len=%u\n", dir, sport, dport, len);
            return;
        }
    } else if (len >= 14 && f[12] == 0x08 && f[13] == 0x06 && len >= 42) {
        uint16_t op = (f[20] << 8) | f[21];
        printk("[NET] %s ARP %s %u.%u.%u.%u -> %u.%u.%u.%u\n",
               dir, (op == 1) ? "REQ" : "REP",
               f[28], f[29], f[30], f[31],
               f[38], f[39], f[40], f[41]);
        return;
    }

    printk("[NET] %s len=%u dst=%02x:%02x:%02x:%02x:%02x:%02x type=%02x%02x\n",
           dir, len, f[0], f[1], f[2], f[3], f[4], f[5], f[12], f[13]);
}

static void rw007_netif_rx(const uint8_t *frame, uint16_t len)
{
    if (!s_iface) {
        return;
    }
    s_rx_frames++;
    dump_frame("RX", frame, len);
    struct net_pkt *pkt = net_pkt_rx_alloc_with_buffer(s_iface, len, NET_AF_UNSPEC, 0, K_NO_WAIT);
    if (!pkt) {
        s_rx_drop++;
        printk("[NET] RX drop: no pkt buffer (drops=%u)\n", s_rx_drop);
        return;
    }
    if (net_pkt_write(pkt, frame, len) < 0 || net_recv_data(s_iface, pkt) < 0) {
        s_rx_drop++;
        net_pkt_unref(pkt);
    }
}

static int rw007_netif_send(const struct device *dev, struct net_pkt *pkt)
{
    size_t len = net_pkt_get_len(pkt);

    if (len > sizeof(s_tx_frame)) {
        return -EMSGSIZE;
    }
    if (net_pkt_read(pkt, s_tx_frame, len) < 0) {
        return -EIO;
    }
    bool ok = rw007_wifi_send_frame(s_tx_frame, (uint16_t)len);

    s_tx_frames++;
    dump_frame(ok ? "TX" : "TX-FAIL", s_tx_frame, (uint16_t)len);
    if (!ok) {
        s_tx_fail++;
    }
    return ok ? 0 : -EBUSY;
}

void rw007_netif_set_mac(const uint8_t mac[6])
{
    if (!s_iface) {
        return;
    }
    net_if_down(s_iface);
    net_if_set_link_addr(s_iface, (uint8_t *)mac, 6, NET_LINK_ETHERNET);
    printk("[NET] iface MAC set to %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void rw007_netif_stats(void)
{
    printk("[NET] stats rx=%u rx_drop=%u tx=%u tx_fail=%u\n", s_rx_frames, s_rx_drop, s_tx_frames, s_tx_fail);
}

static void rw007_netif_iface_init(struct net_if *iface)
{
    uint8_t mac[6];

    s_iface = iface;
    rw007_wifi_get_mac(mac);
    net_if_set_link_addr(iface, mac, sizeof(mac), NET_LINK_ETHERNET);
    ethernet_init(iface);
    net_if_flag_set(iface, NET_IF_NO_AUTO_START);
    net_eth_carrier_off(iface);
    rw007_wifi_set_rx_callback(rw007_netif_rx);
}

void rw007_netif_link(bool up)
{
    if (!s_iface) {
        return;
    }
    if (up) {
        net_if_up(s_iface);
        net_eth_carrier_on(s_iface);
    } else {
        net_eth_carrier_off(s_iface);
    }
}

static enum ethernet_hw_caps rw007_netif_caps(const struct device *dev)
{
    return 0;
}

static const struct ethernet_api rw007_eth_api = {
    .iface_api.init   = rw007_netif_iface_init,
    .get_capabilities = rw007_netif_caps,
    .send             = rw007_netif_send,
};

static int rw007_netif_dev_init(const struct device *dev)
{
    return 0;
}

ETH_NET_DEVICE_INIT(rw007_eth, "RW007", rw007_netif_dev_init, NULL, NULL, NULL,
                    CONFIG_ETH_INIT_PRIORITY, &rw007_eth_api, NET_ETH_MTU);

struct net_if *rw007_netif_get(void)
{
    return net_if_lookup_by_dev(DEVICE_GET(rw007_eth));
}
