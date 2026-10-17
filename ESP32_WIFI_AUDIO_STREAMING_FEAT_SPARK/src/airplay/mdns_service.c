#include "mdns_service.h"
#include "raop_txt.h"

#include <string.h>
#include <stdio.h>
#include "mdns.h"
#include "esp_log.h"

static const char *TAG = "mdns";

esp_err_t mdns_advertise_raop(const char *hostname, const char *instance_name,
                              uint16_t port) {
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "mdns_init: %s", esp_err_to_name(err));
        return err;
    }

    mdns_hostname_set(hostname);
    mdns_instance_name_set(instance_name);

    // 1. Build and advertise _raop._tcp service on port 5000
    raop_txt_item_t items[RAOP_TXT_COUNT];
    size_t n = raop_txt_build(items, RAOP_TXT_COUNT);

    mdns_txt_item_t txt[RAOP_TXT_COUNT];
    for (size_t i = 0; i < n; i++) {
        txt[i].key   = (char *) items[i].key;
        txt[i].value = (char *) items[i].value;
    }

    err = mdns_service_add(instance_name, "_raop", "_tcp", port, txt, n);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "service_add _raop._tcp: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "advertising _raop._tcp '%s' on %s.local:%u (%u TXT records)",
                 instance_name, hostname, port, (unsigned) n);
    }

    // 2. Build and advertise _airplay._tcp service on port 5000 for Apple Music / iTunes
    // Format MAC with colons: "9C:13:9E:F2:3C:9C" from instance_name "9C139EF23C9C@..."
    char dev_id[18] = "00:11:22:33:44:55";
    if (strlen(instance_name) >= 12) {
        snprintf(dev_id, sizeof(dev_id), "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
                 instance_name[0], instance_name[1],
                 instance_name[2], instance_name[3],
                 instance_name[4], instance_name[5],
                 instance_name[6], instance_name[7],
                 instance_name[8], instance_name[9],
                 instance_name[10], instance_name[11]);
    }

    const char *friendly_name = strchr(instance_name, '@');
    if (friendly_name != NULL) {
        friendly_name++; // skip '@'
    } else {
        friendly_name = instance_name;
    }

    mdns_txt_item_t airplay_txt[] = {
        {(char *)"deviceid", dev_id},
        {(char *)"model", (char *)"AirPort4,107"},
        {(char *)"features", (char *)"0x7"},
        {(char *)"flags", (char *)"0x4"},
        {(char *)"srcvers", (char *)"105.1"},
        {(char *)"acl", (char *)"0"},
    };

    err = mdns_service_add(friendly_name, "_airplay", "_tcp", port, airplay_txt,
                           sizeof(airplay_txt) / sizeof(airplay_txt[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "service_add _airplay._tcp: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "advertising _airplay._tcp '%s' on %s.local:%u (deviceid=%s)",
                 friendly_name, hostname, port, dev_id);
    }

    // 3. Build and advertise _audiocast._tcp service on port 12889 for AriaCast
    mdns_txt_item_t audiocast_txt[] = {
        {(char *)"version", (char *)"1.0"},
        {(char *)"samplerate", (char *)"48000"},
        {(char *)"channels", (char *)"2"},
        {(char *)"codecs", (char *)"pcm"},
    };
    err = mdns_service_add(friendly_name, "_audiocast", "_tcp", 12889, audiocast_txt,
                           sizeof(audiocast_txt) / sizeof(audiocast_txt[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "service_add _audiocast._tcp: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "advertising _audiocast._tcp '%s' on %s.local:12889", friendly_name, hostname);
    }

    return ESP_OK;
}
