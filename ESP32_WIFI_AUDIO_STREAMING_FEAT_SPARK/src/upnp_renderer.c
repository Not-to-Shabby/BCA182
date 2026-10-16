/**
 * @file upnp_renderer.c
 * @brief SSDP Advertiser & UPnP Device Descriptor for DLNA Casting.
 */

#include "audio_receiver.h"
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <string.h>
#include <stdio.h>

#define SSDP_MULTICAST_ADDR    "239.255.255.250"
#define SSDP_PORT               1900

static const char *SSDP_NOTIFY_TEMPLATE =
    "NOTIFY * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:1900\r\n"
    "CACHE-CONTROL: max-age=1800\r\n"
    "LOCATION: http://192.168.4.1:8080/description.xml\r\n"
    "NT: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
    "NTS: ssdp:alive\r\n"
    "SERVER: Zephyr/3.x UPnP/1.1 RT-Spark-Audio/1.0\r\n"
    "USN: uuid:12345678-1234-1234-1234-123456789abc::urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
    "\r\n";

static K_THREAD_STACK_DEFINE(s_ssdp_stack, 2048);
static struct k_thread s_ssdp_thread_data;

static void ssdp_beacon_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(SSDP_PORT);
    inet_pton(AF_INET, SSDP_MULTICAST_ADDR, &dest.sin_addr);

    while (1) {
        sendto(sock, SSDP_NOTIFY_TEMPLATE, strlen(SSDP_NOTIFY_TEMPLATE), 0,
               (struct sockaddr *)&dest, sizeof(dest));
        k_sleep(K_SECONDS(10));
    }
}

int upnp_renderer_init(void)
{
    k_thread_create(&s_ssdp_thread_data, s_ssdp_stack,
                    K_THREAD_STACK_SIZEOF(s_ssdp_stack),
                    ssdp_beacon_thread,
                    NULL, NULL, NULL,
                    7, 0, K_NO_WAIT);
    printk("[UPnP] SSDP MediaRenderer discovery service started.\n");
    return 0;
}
