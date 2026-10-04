// SlotStore on the ESP32: two raw data partitions (subtype 0x40, labels "modA" / "modB") for the
// images, and NVS for the A/B state. See deploy.hpp for why the slots are raw partitions rather
// than §7.4's LittleFS.

#include "pot/deploy_esp.hpp"

#include <cstring>

#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace pot {

namespace {

constexpr const char* kTag = "pot.deploy";
constexpr const char* kNvsNs = "pot_deploy";
constexpr uint32_t kSlotMagic = 0x544C5350u;  // "PSLT"
constexpr size_t kSlotHeaderLen = 12;         // magic, len, crc

const esp_partition_t* slot_partition(uint8_t slot) {
    const char* label = (slot == kSlotA) ? "modA" : (slot == kSlotB) ? "modB" : nullptr;
    if (label == nullptr) {
        return nullptr;
    }
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                     static_cast<esp_partition_subtype_t>(0x40), label);
}

void put32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}
uint32_t get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

bool EspSlotStore::init() {
    // NVS is normally initialised by the radio bring-up already; a second init is harmless, and
    // a build without the radio still needs it here.
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK && err != ESP_ERR_NVS_NO_FREE_PAGES && err != ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(kTag, "nvs_flash_init: %s", esp_err_to_name(err));
    }
    ok_ = slot_partition(kSlotA) != nullptr && slot_partition(kSlotB) != nullptr;
    if (!ok_) {
        ESP_LOGE(kTag, "module slot partitions modA/modB not found: is partitions.csv flashed?");
    }
    return ok_;
}

bool EspSlotStore::load_state(DeployState& out) {
    out = DeployState{};
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) {
        return true;  // never written: nothing deployed, which is a valid state
    }
    nvs_get_u8(h, "active", &out.active);
    nvs_get_u8(h, "previous", &out.previous);
    nvs_get_u8(h, "pending", &out.pending);
    nvs_get_u8(h, "trials", &out.trial_boots);
    nvs_get_u32(h, "counter", &out.counter);
    nvs_close(h);
    return true;
}

bool EspSlotStore::save_state(const DeployState& s) {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_u8(h, "active", s.active) == ESP_OK && nvs_set_u8(h, "previous", s.previous) == ESP_OK &&
              nvs_set_u8(h, "pending", s.pending) == ESP_OK && nvs_set_u8(h, "trials", s.trial_boots) == ESP_OK &&
              nvs_set_u32(h, "counter", s.counter) == ESP_OK;
    ok = ok && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool EspSlotStore::write_slot(uint8_t slot, const uint8_t* data, size_t len) {
    const esp_partition_t* part = slot_partition(slot);
    if (part == nullptr || kSlotHeaderLen + len > part->size) {
        return false;
    }
    const size_t total = kSlotHeaderLen + len;
    const size_t unit = part->erase_size;  // the partition's own erase granularity
    const size_t erase = (total + unit - 1) / unit * unit;
    if (esp_partition_erase_range(part, 0, erase) != ESP_OK) {
        return false;
    }
    // Data first, header last: a write torn before the header lands leaves a slot that reads as
    // empty rather than one whose header vouches for half an image.
    if (esp_partition_write(part, kSlotHeaderLen, data, len) != ESP_OK) {
        return false;
    }
    uint8_t hdr[kSlotHeaderLen];
    put32(hdr, kSlotMagic);
    put32(hdr + 4, static_cast<uint32_t>(len));
    put32(hdr + 8, crc32(data, len));
    return esp_partition_write(part, 0, hdr, sizeof(hdr)) == ESP_OK;
}

bool EspSlotStore::read_slot(uint8_t slot, uint8_t* buf, size_t cap, size_t& len) {
    len = 0;
    const esp_partition_t* part = slot_partition(slot);
    if (part == nullptr) {
        return false;
    }
    uint8_t hdr[kSlotHeaderLen];
    if (esp_partition_read(part, 0, hdr, sizeof(hdr)) != ESP_OK || get32(hdr) != kSlotMagic) {
        return false;
    }
    const uint32_t n = get32(hdr + 4);
    if (n == 0 || n > cap || kSlotHeaderLen + n > part->size) {
        return false;
    }
    if (esp_partition_read(part, kSlotHeaderLen, buf, n) != ESP_OK) {
        return false;
    }
    if (crc32(buf, n) != get32(hdr + 8)) {
        ESP_LOGE(kTag, "slot %u fails its CRC", static_cast<unsigned>(slot));
        return false;
    }
    len = n;
    return true;
}

}  // namespace pot
