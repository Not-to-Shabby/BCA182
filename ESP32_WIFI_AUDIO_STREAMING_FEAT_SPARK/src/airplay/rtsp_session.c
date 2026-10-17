#include "rtsp_session.h"

#include <string.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_log.h"

static const char *TAG = "raop_session";

void raop_session_close_udp(raop_session_t *s) {
    int *fds[3] = {&s->audio_fd, &s->control_fd, &s->timing_fd};
    for (int i = 0; i < 3; i++) {
        if (*fds[i] >= 0) {
            close(*fds[i]);
            *fds[i] = -1;
        }
    }
    s->audio_port = s->control_port = s->timing_port = 0;
}

void raop_session_reset(raop_session_t *s) {
    raop_session_close_udp(s);
    memset(s, 0, sizeof(*s));
    s->state = RAOP_IDLE;
    s->audio_fd = s->control_fd = s->timing_fd = -1;
}

// Bind one UDP socket to an ephemeral port; return the fd and its port (host order).
static int bind_one(uint16_t *out_port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;  // ephemeral

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in local;
    socklen_t llen = sizeof(local);
    if (getsockname(fd, (struct sockaddr *)&local, &llen) != 0) {
        close(fd);
        return -1;
    }
    *out_port = ntohs(local.sin_port);
    return fd;
}

int raop_session_bind_udp(raop_session_t *s) {
    s->audio_fd   = bind_one(&s->audio_port);
    s->control_fd = bind_one(&s->control_port);
    s->timing_fd  = bind_one(&s->timing_port);
    if (s->audio_fd < 0 || s->control_fd < 0 || s->timing_fd < 0) {
        ESP_LOGE(TAG, "UDP bind failed (audio=%d control=%d timing=%d)",
                 s->audio_fd, s->control_fd, s->timing_fd);
        raop_session_close_udp(s);
        return -1;
    }
    ESP_LOGI(TAG, "UDP bound: audio=%u control=%u timing=%u",
             s->audio_port, s->control_port, s->timing_port);
    return 0;
}
