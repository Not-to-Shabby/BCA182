// AirPlay-1 (RAOP) receiver: RTSP control server. Public surface is deliberately
// tiny — main starts it after Wi-Fi GOT_IP (once esp_netif + the advertised
// _raop._tcp service exist). Single session at a time (spec §8: a second sender
// gets 453 Busy). The RTP audio path is Phase 3.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Session lifecycle events for the status LED (spec §7). The RTSP task owns
// RECORD/TEARDOWN; main wires this callback to system_led_set_state so the LED
// reflects streaming vs idle without raop depending on the system component.
typedef enum { RAOP_EV_STREAMING, RAOP_EV_IDLE } raop_event_t;
typedef void (*raop_event_cb_t)(raop_event_t ev);

// Register a callback fired on RECORD (STREAMING) and on TEARDOWN / idle-reclaim
// (IDLE). NULL clears it. Call before raop_server_start(). Invoked from the RTSP
// task; keep the callback short and non-blocking.
void raop_set_event_cb(raop_event_cb_t cb);

// Start the RTSP server task (listens on RAOP_RTSP_PORT = 5000). Safe to call
// once. Runs raop_crypto_init() internally on first start; if the crypto self-
// test fails the server is NOT started (we cannot negotiate without the key).
void raop_server_start(void);

// Stop the server task and tear down any live session (used on Wi-Fi LOST_IP later).
void raop_server_stop(void);

#ifdef __cplusplus
}
#endif
