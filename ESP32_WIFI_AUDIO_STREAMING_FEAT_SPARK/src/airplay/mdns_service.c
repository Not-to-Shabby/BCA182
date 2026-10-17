#include "mdns_service.h"
#include "raop_txt.h"

#include "mdns.h"      // managed espressif/mdns
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

    raop_txt_item_t items[RAOP_TXT_COUNT];
    size_t n = raop_txt_build(items, RAOP_TXT_COUNT);

    // Copy the pure const items into IDF's (non-const) txt struct.
    mdns_txt_item_t txt[RAOP_TXT_COUNT];
    for (size_t i = 0; i < n; i++) {
        txt[i].key   = (char *) items[i].key;
        txt[i].value = (char *) items[i].value;
    }

    err = mdns_service_add(instance_name, "_raop", "_tcp", port, txt, n);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "service_add _raop._tcp: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "advertising _raop._tcp '%s' on %s.local:%u (%u TXT records)",
             instance_name, hostname, port, (unsigned) n);
    return ESP_OK;
}
