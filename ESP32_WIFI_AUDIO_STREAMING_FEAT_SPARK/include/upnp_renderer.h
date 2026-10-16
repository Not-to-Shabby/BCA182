/**
 * @file upnp_renderer.h
 * @brief UPnP/DLNA MediaRenderer (SSDP discovery, AVTransport/RenderingControl SOAP, HTTP WAV pull).
 */

#ifndef UPNP_RENDERER_H_
#define UPNP_RENDERER_H_

#include <stdint.h>

/* Implemented by main.cpp */
void send_spark_packet(uint8_t type, const uint8_t *payload, uint16_t len);
void spark_set_stream_state(uint8_t state);
void spark_set_volume(uint8_t percent);

/* Implemented by upnp_renderer.cpp */
void upnp_begin(void);
void upnp_loop(void);
void upnp_button(uint8_t cmd);

#endif /* UPNP_RENDERER_H_ */
