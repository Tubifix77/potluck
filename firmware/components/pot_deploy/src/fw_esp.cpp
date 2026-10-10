// M11: EspOtaSink. See pot/fw_esp.hpp.

#include "pot/fw_esp.hpp"

#include "esp_log.h"

namespace pot {

namespace {
constexpr const char* kTag = "pot_fw";
}

bool EspOtaSink::init() {
    part_ = esp_ota_get_next_update_partition(nullptr);
    return part_ != nullptr;
}

uint32_t EspOtaSink::capacity() const { return part_ != nullptr ? static_cast<uint32_t>(part_->size) : 0; }

bool EspOtaSink::begin(uint32_t) {
    abort();
    if (part_ == nullptr) return false;
    // Sequential writes: each sector is erased as the image reaches it (~4 KB at a time), instead of
    // the whole image at once -- an erase of ~0.9 MB holds the flash for seconds, and the node with it.
    const esp_err_t e = esp_ota_begin(part_, OTA_WITH_SEQUENTIAL_WRITES, &h_);
    if (e != ESP_OK) {
        ESP_LOGE(kTag, "esp_ota_begin: %s", esp_err_to_name(e));
        return false;
    }
    open_ = true;
    ended_ = false;
    return true;
}

bool EspOtaSink::write(const uint8_t* data, size_t len) {
    if (!open_) return false;
    const esp_err_t e = esp_ota_write(h_, data, len);
    if (e != ESP_OK) {
        ESP_LOGE(kTag, "esp_ota_write: %s", esp_err_to_name(e));
        return false;
    }
    return true;
}

bool EspOtaSink::finish() {
    if (!open_) return false;
    open_ = false;  // esp_ota_end frees the handle whatever it answers
    const esp_err_t e = esp_ota_end(h_);
    if (e != ESP_OK) {
        ESP_LOGE(kTag, "esp_ota_end: %s", esp_err_to_name(e));
        return false;
    }
    ended_ = true;
    return true;
}

bool EspOtaSink::activate() {
    if (!ended_ || part_ == nullptr) return false;
    const esp_err_t e = esp_ota_set_boot_partition(part_);
    if (e != ESP_OK) {
        ESP_LOGE(kTag, "esp_ota_set_boot_partition: %s", esp_err_to_name(e));
        return false;
    }
    ended_ = false;
    return true;
}

void EspOtaSink::abort() {
    if (open_) esp_ota_abort(h_);
    open_ = false;
    ended_ = false;
}

}  // namespace pot
