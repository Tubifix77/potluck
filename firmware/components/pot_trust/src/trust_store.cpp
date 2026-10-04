#include "pot/trust_store.hpp"

#include <cstring>

#include "bootloader_random.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

namespace pot {

namespace {

constexpr const char* kTag = "pot.trust";
constexpr const char* kNs = "pot_trust";
constexpr const char* kKey = "id";
constexpr uint8_t kBlobVersion = 1;

// One blob, written whole, so a power cut leaves either the old identity or the new one. The private
// seed is stored in the clear: without NVS encryption, physical access to the flash is game over
// (section 9.5 says so), and this bench has neither flash nor NVS encryption enabled.
struct Blob {
    uint8_t version;
    uint8_t flags;  // bit 0 has_key, bit 1 enrolled
    uint8_t reserved[2];
    uint8_t seed[kEdSeedLen];
    uint8_t pub[kEdPubLen];
    uint8_t ca_pub[kEdPubLen];
    uint8_t cert[kNodeCertLen];
};

}  // namespace

bool trust_load(Identity& id) {
    id = Identity{};
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    Blob b{};
    size_t len = sizeof(b);
    const esp_err_t err = nvs_get_blob(h, kKey, &b, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(b) || b.version != kBlobVersion) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(kTag, "identity blob unreadable (%s, %u bytes, v%u)", esp_err_to_name(err),
                     static_cast<unsigned>(len), static_cast<unsigned>(b.version));
        }
        return false;
    }
    // Re-derive the public key from the seed rather than trusting the stored copy: a torn or
    // tampered blob must not leave a node advertising a key it cannot sign with.
    identity_from_seed(id, b.seed);
    if (std::memcmp(id.pub, b.pub, kEdPubLen) != 0) {
        ESP_LOGE(kTag, "stored public key does not match the stored seed; treating as no identity");
        id = Identity{};
        return false;
    }
    if ((b.flags & 2) != 0) {
        std::memcpy(id.ca_pub, b.ca_pub, kEdPubLen);
        std::memcpy(id.cert, b.cert, kNodeCertLen);
        id.enrolled = true;
    }
    std::memset(&b, 0, sizeof(b));
    return true;
}

bool trust_save(const Identity& id) {
    Blob b{};
    b.version = kBlobVersion;
    b.flags = static_cast<uint8_t>((id.has_key ? 1 : 0) | (id.enrolled ? 2 : 0));
    std::memcpy(b.seed, id.seed, kEdSeedLen);
    std::memcpy(b.pub, id.pub, kEdPubLen);
    std::memcpy(b.ca_pub, id.ca_pub, kEdPubLen);
    std::memcpy(b.cert, id.cert, kNodeCertLen);
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, kKey, &b, sizeof(b));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    std::memset(&b, 0, sizeof(b));
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "saving identity failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void trust_random_seed(uint8_t seed[kEdSeedLen], bool radio_running) {
    if (!radio_running) {
        bootloader_random_enable();
    }
    esp_fill_random(seed, kEdSeedLen);
    if (!radio_running) {
        bootloader_random_disable();
    }
}

}  // namespace pot
