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

    // 2. Build and advertise _audiocast._tcp service on port 12889 for AriaCast
    const char *friendly_name = strchr(instance_name, '@');
    if (friendly_name != NULL) {
        friendly_name++; // skip '@'
    } else {
        friendly_name = instance_name;
    }

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
