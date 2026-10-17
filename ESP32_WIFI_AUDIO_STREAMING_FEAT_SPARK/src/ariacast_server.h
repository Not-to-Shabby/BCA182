#ifndef ARIACAST_SERVER_H_
#define ARIACAST_SERVER_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARIACAST_DISCOVERY_PORT 12888
#define ARIACAST_STREAMING_PORT 12889

/**
 * @brief Start AriaCast native UDP discovery and WebSocket audio server.
 */
void ariacast_server_begin(void);

/**
 * @brief Stop AriaCast server and close connections.
 */
void ariacast_server_stop(void);

/**
 * @brief Send playback action to AriaCast app (play, pause, next, previous).
 */
void ariacast_server_button_action(const char *action);

#ifdef __cplusplus
}
#endif

#endif /* ARIACAST_SERVER_H_ */
