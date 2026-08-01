// Boot epoch — see boot_epoch.hpp.

#include "pot/boot_epoch.hpp"

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace pot {
namespace {

const char* kTag = "pot.epoch";
const char* kNamespace = "pot";
const char* kKey = "boot_epoch";  // §3: NVS keys are capped at 15 characters; this is 10

uint32_t g_current = 0;

}  // namespace

uint32_t boot_epoch_current() { return g_current; }

uint32_t boot_epoch_next() {
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        g_current = esp_random() | 0x80000000u;  // never 0; see the header
        ESP_LOGW(kTag, "nvs_open failed (%s); using random epoch %u", esp_err_to_name(err),
                 static_cast<unsigned>(g_current));
        return g_current;
    }

    uint32_t epoch = 0;
    err = nvs_get_u32(h, kKey, &epoch);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(kTag, "nvs_get_u32 failed: %s", esp_err_to_name(err));
    }

    ++epoch;
    if (epoch == 0) {
        // Wrapped after 4.29 billion boots, which will not happen, but 0 means "no information"
        // to every reader of the field so it must never be emitted.
        epoch = 1;
    }

    err = nvs_set_u32(h, kKey, epoch);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        // The counter did not persist, so the next boot will reuse this value. Say so loudly:
        // silently repeating an epoch would make a reboot look like a recovered link.
        ESP_LOGE(kTag, "failed to persist boot epoch (%s) - epoch %u may repeat",
                 esp_err_to_name(err), static_cast<unsigned>(epoch));
    }
    nvs_close(h);

    g_current = epoch;
    ESP_LOGI(kTag, "boot epoch %u", static_cast<unsigned>(epoch));
    return epoch;
}

}  // namespace pot
