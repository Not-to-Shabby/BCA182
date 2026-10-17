// mDNS advertisement of the AirPlay-1 (RAOP) service so the speaker appears in
// the iOS/macOS AirPlay menu. Thin wrapper over IDF's managed `espressif/mdns`.
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// RTSP control port advertised for _raop._tcp. Nothing listens here in Phase 1
// (the RTSP state machine is Phase 2); the advert alone makes us discoverable.
#define RAOP_RTSP_PORT 5000

// Init mDNS, set `hostname` (e.g. "conduit"), and advertise _raop._tcp on `port`
// with the RAOP TXT records and service instance name `instance_name`
// (e.g. "E83DC1F2AC6C@Conduit"). Precondition: esp_netif/Wi-Fi are up.
esp_err_t mdns_advertise_raop(const char *hostname, const char *instance_name,
                              uint16_t port);

#ifdef __cplusplus
}
#endif
