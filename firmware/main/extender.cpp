// M6.1 (CR-6): the extender seam. See extender.hpp.

#include "extender.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "sdkconfig.h"

#if CONFIG_POT_EXTENDER
#if !__has_include("pme/hotspot.h")
#error "CONFIG_POT_EXTENDER needs poor-mans-extender's pme_hotspot: build with tools\\build_firmware.ps1 -Variant extender -PmeHotspot <its firmware\\components dir>"
#endif
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "pme/creds.h"
#include "pme/hotspot.h"
#include "pot/espnow_port.hpp"
#endif

namespace pot::extender {

#if CONFIG_POT_EXTENDER

namespace {

const char* kTag = "pot.extender";
bool g_active = false;
std::atomic<uint8_t> g_channel{0};

// pme_hotspot calls this on the event task. The node is not ours to touch from here (it lives under
// the link task's lock, and an announcement is signed on the crypto worker), so the channel is only
// left for the link task to pick up.
void on_channel(uint8_t channel, void*) { g_channel.store(channel); }

// Split a console line into at most `max` arguments, in place: double quotes group, backslash escapes
// the next character. The same rules as ESP-IDF's esp_console_split_argv, which the extender's own
// `pme>` prompt uses, so a quoted name typed there types the same here.
size_t split(char* s, char* argv[], size_t max) {
    size_t argc = 0;
    char* out = s;
    char* p = s;
    while (*p != '\0' && argc < max) {
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '\0') break;
        argv[argc++] = out;
        bool quoted = false;
        while (*p != '\0') {
            if (*p == '\\' && p[1] != '\0') {
                *out++ = p[1];
                p += 2;
            } else if (*p == '"') {
                quoted = !quoted;
                ++p;
            } else if (!quoted && (*p == ' ' || *p == '\t')) {
                ++p;
                break;
            } else {
                *out++ = *p++;
            }
        }
        *out++ = '\0';
    }
    return argc;
}

bool fits(const char* ssid, const char* pass) {
    const size_t s = std::strlen(ssid), p = std::strlen(pass);
    return s >= 1 && s <= PME_SSID_MAX && p >= PME_PASS_MIN && p <= PME_PASS_MAX;
}

}  // namespace

bool start() {
    // ESP-IDF's Wi-Fi driver logs "wifi:connected with <network name>, ..." at INFO on every association
    // (seen on board B, M0-LOG session 30). pme_hotspot no longer names networks; neither may the
    // console under it, since a soak captures the console whole. Warnings and errors still print.
    esp_log_level_set("wifi", ESP_LOG_WARN);
    pme_hotspot_config_t cfg{};
    if (pme_creds_load(&cfg) != ESP_OK) {
        ESP_LOGW(kTag, "credentials unreadable; running as an ordinary member");
        return false;
    }
    if (cfg.ap_ssid[0] == '\0') {
        std::strncpy(cfg.ap_ssid, CONFIG_POT_EXTENDER_AP_SSID_DEFAULT, PME_SSID_MAX);
    }
    cfg.ap_max_conn = CONFIG_POT_EXTENDER_AP_MAX_CONN;
    pme_hotspot_set_channel_cb(&on_channel, nullptr);
    const esp_err_t err = pme_hotspot_start(&cfg);
    std::memset(&cfg, 0, sizeof(cfg));  // the only copy at rest is pme_creds' NVS
    if (err != ESP_OK) {
        ESP_LOGW(kTag,
                 "hotspot not started (%s): running as an ordinary member. Give it credentials with "
                 "`POT! pme sta ...` and `POT! pme ap ...`, then reset.",
                 esp_err_to_name(err));
        return false;
    }
    g_active = true;
    ESP_LOGI(kTag, "hotspot up; ESP-NOW attaches to its radio and follows the router's channel");
    return true;
}

bool active() { return g_active; }

uint8_t take_channel() { return g_channel.exchange(0); }

bool associated() {
    if (!g_active) return false;
    pme_hotspot_status_t st{};
    return pme_hotspot_get_status(&st) == ESP_OK && st.sta_connected;
}

void print_status() {
    pme_hotspot_status_t st{};
    if (g_active) pme_hotspot_get_status(&st);
    // Field for field the baseline's line (poor-mans-extender firmware/main/main.c). Never a credential.
    std::printf("{\"t\":\"hs\",\"up_s\":%llu,\"running\":%d,\"sta\":%d,\"has_ip\":%d,\"ch\":%u,"
                "\"rssi\":%d,\"clients\":%u,\"reconnects\":%lu,\"int_free\":%u,\"psram_free\":%u}\n",
                static_cast<unsigned long long>(esp_timer_get_time() / 1000000), g_active ? 1 : 0,
                st.sta_connected ? 1 : 0, st.sta_has_ip ? 1 : 0, static_cast<unsigned>(st.channel),
                static_cast<int>(st.rssi), static_cast<unsigned>(st.clients),
                static_cast<unsigned long>(st.reconnects),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

bool handle_console(const char* line, size_t len) {
    static const char kPrefix[] = "POT! pme ";
    constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (len < kPrefixLen || std::memcmp(line, kPrefix, kPrefixLen) != 0) return false;
    char buf[400];
    if (len >= sizeof(buf)) {
        std::printf("{\"t\":\"pme\",\"result\":\"too_long\"}\n");
        return true;
    }
    std::memcpy(buf, line + kPrefixLen, len - kPrefixLen);
    buf[len - kPrefixLen] = '\0';
    for (size_t n = std::strlen(buf); n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n'); --n) buf[n - 1] = '\0';
    char* argv[4] = {};
    const size_t argc = split(buf, argv, 4);
    const char* result = "usage";
    if (argc == 1 && std::strcmp(argv[0], "forget") == 0) {
        result = pme_creds_erase() == ESP_OK ? "forgotten" : "erase_failed";
    } else if (argc == 3 && (std::strcmp(argv[0], "sta") == 0 || std::strcmp(argv[0], "ap") == 0)) {
        if (!fits(argv[1], argv[2])) {
            result = "refused_length";  // name 1-32 bytes, password 8-64 (WPA2): pme_hotspot's own limits
        } else {
            const bool sta = argv[0][0] == 's';
            const esp_err_t e = sta ? pme_creds_save_sta(argv[1], argv[2]) : pme_creds_save_ap(argv[1], argv[2]);
            result = e == ESP_OK ? (sta ? "sta_saved" : "ap_saved") : "save_failed";
        }
    }
    std::memset(buf, 0, sizeof(buf));
    // The result only: never the arguments. "saved" takes effect at the next boot.
    std::printf("{\"t\":\"pme\",\"result\":\"%s\"}\n", result);
    return true;
}

#else  // !CONFIG_POT_EXTENDER

bool start() { return false; }
bool active() { return false; }
uint8_t take_channel() { return 0; }
bool associated() { return false; }
void print_status() {}
bool handle_console(const char*, size_t) { return false; }

#endif

}  // namespace pot::extender
