#include "raop_metadata.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "raop-meta";
static dmap_meta_t s_cur;   // zeroed at load

// Copy src into dst when the tag was present AND the value actually changed.
// Returns true if it changed (so the caller logs once per real change).
static bool upd(char *dst, bool has, const char *src) {
    if (!has) return false;
    if (strcmp(dst, src) == 0) return false;
    strncpy(dst, src, DMAP_STR_MAX - 1);
    dst[DMAP_STR_MAX - 1] = '\0';
    return true;
}

extern void airplay_notify_metadata(const char *title, const char *artist, const char *album);

void raop_metadata_update(const dmap_meta_t *m) {
    bool ch = false;
    ch |= upd(s_cur.title,  m->has_title,  m->title);
    ch |= upd(s_cur.artist, m->has_artist, m->artist);
    ch |= upd(s_cur.album,  m->has_album,  m->album);
    if (ch) {
        ESP_LOGI(TAG, "now playing: \"%s\" - %s - %s",
                 s_cur.title, s_cur.artist, s_cur.album);
        airplay_notify_metadata(s_cur.title, s_cur.artist, s_cur.album);
    }
}

void raop_metadata_clear(void) {
    memset(&s_cur, 0, sizeof(s_cur));
}
