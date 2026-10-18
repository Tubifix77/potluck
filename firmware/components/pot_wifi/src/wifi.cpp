// M13: Potluck's own Wi-Fi station. See pot/wifi.hpp.

#include "pot/wifi.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "sdkconfig.h"

#if CONFIG_POT_WIFI
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#endif

namespace pot::wifi {

#if CONFIG_POT_WIFI

namespace {

const char* kTag = "pot.wifi";
const char* kNvsNs = "pot_wifi";
constexpr size_t kSsidMax = 32;
constexpr size_t kPassMin = 8;
constexpr size_t kPassMax = 64;
constexpr uint32_t kRetryFirstMs = 1000;
constexpr uint32_t kRetryMaxMs = 60000;
constexpr uint32_t kRepickGapMs = 30000;
constexpr uint16_t kMaxRecords = 16;

bool g_active = false;
// Kept for the association only: the driver holds its own copy once set_config has run.
char g_ssid[kSsidMax + 1] = {};
char g_pass[kPassMax + 1] = {};
std::atomic<bool> g_assoc{false};
std::atomic<bool> g_ip{false};
std::atomic<uint8_t> g_channel{0};       // the current association's, 0 if none
std::atomic<uint8_t> g_take{0};          // the latest association's, until taken
std::atomic<uint8_t> g_prefer{0};
std::atomic<uint8_t> g_force{0};         // join_channel's: the next pick takes this channel whatever the margin
std::atomic<uint16_t> g_seen{0};         // channels the network was heard on (bit c), from every scan
std::atomic<int8_t> g_rssi{0};
std::atomic<uint32_t> g_associations{0};
std::atomic<uint32_t> g_scans{0};
std::atomic<uint8_t> g_last_reason{0};
std::atomic<bool> g_repick{false};       // a disconnect we asked for: pick again at once
std::atomic<uint32_t> g_retry_ms{kRetryFirstMs};
std::atomic<int64_t> g_retry_at_us{0};
std::atomic<int64_t> g_last_repick_us{0};
esp_timer_handle_t g_retry_timer = nullptr;

bool load_creds() {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sl = sizeof(g_ssid), pl = sizeof(g_pass);
    const bool ok = nvs_get_str(h, "ssid", g_ssid, &sl) == ESP_OK && nvs_get_str(h, "pass", g_pass, &pl) == ESP_OK &&
                    g_ssid[0] != '\0';
    nvs_close(h);
    if (!ok) {
        std::memset(g_ssid, 0, sizeof(g_ssid));
        std::memset(g_pass, 0, sizeof(g_pass));
    }
    return ok;
}

void scan_now() {
    wifi_scan_config_t sc{};
    sc.ssid = reinterpret_cast<uint8_t*>(g_ssid);  // only this network's access points
    sc.show_hidden = false;
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    g_retry_at_us.store(0);
    if (esp_wifi_scan_start(&sc, false) == ESP_OK) {
        g_scans.fetch_add(1);
    } else {
        ESP_LOGW(kTag, "scan did not start; retrying");
        // Fall through to the retry below on the next disconnect or timer.
        const uint32_t ms = g_retry_ms.load();
        g_retry_at_us.store(esp_timer_get_time() + static_cast<int64_t>(ms) * 1000);
        esp_timer_start_once(g_retry_timer, static_cast<uint64_t>(ms) * 1000);
    }
}

void retry_later() {
    const uint32_t ms = g_retry_ms.load();
    g_retry_at_us.store(esp_timer_get_time() + static_cast<int64_t>(ms) * 1000);
    esp_timer_stop(g_retry_timer);
    esp_timer_start_once(g_retry_timer, static_cast<uint64_t>(ms) * 1000);
    g_retry_ms.store(ms * 2 > kRetryMaxMs ? kRetryMaxMs : ms * 2);
}

void on_retry(void*) {
    if (!g_assoc.load()) scan_now();  // an association that came in meanwhile stands
}

// The access point to join: on the preferred channel if one there is within the margin of the strongest,
// else the strongest. Returns false if the network was not heard.
bool pick(wifi_ap_record_t& out) {
    EXT_RAM_BSS_ATTR static wifi_ap_record_t recs[kMaxRecords];  // only the event task: PSRAM where there is any
    uint16_t n = kMaxRecords;
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK || n == 0) return false;
    const uint8_t force = g_force.exchange(0);
    const uint8_t prefer = force != 0 ? force : g_prefer.load();
    int best = -1, best_pref = -1;
    for (uint16_t i = 0; i < n; ++i) {
        if (std::strncmp(reinterpret_cast<const char*>(recs[i].ssid), g_ssid, kSsidMax) != 0) continue;
        if (recs[i].primary >= 1 && recs[i].primary <= 14) g_seen.fetch_or(static_cast<uint16_t>(1u << recs[i].primary));
        if (best < 0 || recs[i].rssi > recs[best].rssi) best = i;
        if (prefer != 0 && recs[i].primary == prefer && (best_pref < 0 || recs[i].rssi > recs[best_pref].rssi)) {
            best_pref = i;
        }
    }
    if (best < 0) return false;
    const bool take_pref = best_pref >= 0 && (force != 0 ||
                                              recs[best_pref].rssi >= recs[best].rssi - CONFIG_POT_WIFI_PREFER_MARGIN_DB);
    out = recs[take_pref ? best_pref : best];
    ESP_LOGI(kTag, "joining an access point on channel %u (%d dBm)%s", static_cast<unsigned>(out.primary),
             static_cast<int>(out.rssi), take_pref ? ", the cell's" : (prefer != 0 ? "; none within the margin on the cell's" : ""));
    return true;
}

void on_wifi(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        scan_now();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        if (g_assoc.load()) return;  // a scan that finished after an association: nothing to join
        wifi_ap_record_t ap{};
        if (!pick(ap)) {
            ESP_LOGW(kTag, "network not heard; retrying in %u ms", static_cast<unsigned>(g_retry_ms.load()));
            retry_later();
            return;
        }
        wifi_config_t cfg{};
        std::memcpy(cfg.sta.ssid, g_ssid, std::strlen(g_ssid));
        std::memcpy(cfg.sta.password, g_pass, std::strlen(g_pass));
        cfg.sta.bssid_set = true;
        std::memcpy(cfg.sta.bssid, ap.bssid, sizeof(cfg.sta.bssid));
        cfg.sta.channel = ap.primary;
        cfg.sta.scan_method = WIFI_FAST_SCAN;
        if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK || esp_wifi_connect() != ESP_OK) {
            ESP_LOGW(kTag, "could not start the association; retrying");
            retry_later();
        }
        std::memset(&cfg, 0, sizeof(cfg));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        const auto* ev = static_cast<const wifi_event_sta_connected_t*>(data);
        g_assoc.store(true);
        g_channel.store(ev->channel);
        g_take.store(ev->channel);
        g_associations.fetch_add(1);
        wifi_ap_record_t info{};
        if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) g_rssi.store(info.rssi);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto* ev = static_cast<const wifi_event_sta_disconnected_t*>(data);
        const bool was = g_assoc.exchange(false);
        g_ip.store(false);
        g_channel.store(0);
        g_last_reason.store(ev->reason);
        if (g_repick.exchange(false)) {
            scan_now();
            return;
        }
        if (was) ESP_LOGW(kTag, "disconnected (reason %u)", static_cast<unsigned>(ev->reason));
        retry_later();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        g_ip.store(true);
        g_retry_ms.store(kRetryFirstMs);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        g_ip.store(false);  // associated, no address: not connected for a role that connects out
    }
}

// Split a console line into at most `max` arguments, in place: double quotes group, backslash escapes the next
// character -- the extender's rules (firmware/main/extender.cpp), so a name quoted there is quoted the same here.
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

}  // namespace

bool start(uint8_t cell_channel) {
    // ESP-IDF's driver logs "connected with <network name>" at INFO on every association; a soak captures the
    // console whole, so it may not (the extender's rule, M0-LOG session 30). Warnings and errors still print.
    esp_log_level_set("wifi", ESP_LOG_WARN);
    if (!load_creds()) {
        ESP_LOGI(kTag, "no network saved; the module is idle (POT! wifi sta ..., or tools/wifi_set.py)");
        return false;
    }
    g_prefer.store(cell_channel);
    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) {
        err = esp_event_loop_create_default();
        if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;
    }
    if (err == ESP_OK && esp_netif_create_default_wifi_sta() == nullptr) err = ESP_FAIL;
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    if (err == ESP_OK) err = esp_wifi_init(&wcfg);
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);  // the credentials' one copy is ours, in NVS
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    const esp_timer_create_args_t targs = {&on_retry, nullptr, ESP_TIMER_TASK, "pot_wifi", false};
    if (err == ESP_OK) err = esp_timer_create(&targs, &g_retry_timer);
    if (err == ESP_OK) err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi, nullptr);
    if (err == ESP_OK) err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi, nullptr);
    if (err == ESP_OK) err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &on_wifi, nullptr);
    if (err == ESP_OK) err = esp_wifi_start();
    // No power save: ESP-NOW measures delay, and modem sleep adds a wake latency (espnow_port.cpp's rule).
    if (err == ESP_OK) err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "station not started (%s); running as an ordinary member", esp_err_to_name(err));
        return false;
    }
    g_active = true;
    ESP_LOGI(kTag, "station up; ESP-NOW attaches to its radio and follows the router's channel");
    return true;
}

bool active() { return g_active; }
bool associated() { return g_active && g_assoc.load(); }
bool has_ip() { return g_active && g_ip.load(); }
uint8_t take_channel() { return g_take.exchange(0); }
void prefer_channel(uint8_t channel) { g_prefer.store(channel); }

uint16_t seen_channels() { return g_seen.load(); }

bool join_channel(uint8_t channel) {
    if (!g_active || channel < 1 || channel > 14) return false;
    g_force.store(channel);
    ESP_LOGI(kTag, "joining the network on channel %u", static_cast<unsigned>(channel));
    if (g_assoc.load()) {
        g_repick.store(true);
        esp_wifi_disconnect();  // the disconnect event scans at once
    } else {
        esp_timer_stop(g_retry_timer);
        scan_now();
    }
    return true;
}

bool repick() {
    if (!g_active) return false;
    const int64_t now = esp_timer_get_time();
    const int64_t last = g_last_repick_us.load();
    if (last != 0 && now - last < static_cast<int64_t>(kRepickGapMs) * 1000) return false;
    g_last_repick_us.store(now);
    ESP_LOGI(kTag, "picking again, preferring channel %u", static_cast<unsigned>(g_prefer.load()));
    if (g_assoc.load()) {
        g_repick.store(true);
        esp_wifi_disconnect();  // the disconnect event scans at once
    } else {
        esp_timer_stop(g_retry_timer);
        scan_now();
    }
    return true;
}

Status status() {
    Status s;
    s.active = g_active;
    s.associated = associated();
    s.has_ip = has_ip();
    s.channel = g_channel.load();
    s.rssi = g_rssi.load();
    s.associations = g_associations.load();
    s.scans = g_scans.load();
    const int64_t at = g_retry_at_us.load();
    const int64_t now = esp_timer_get_time();
    s.retry_in_ms = (at != 0 && at > now) ? static_cast<uint32_t>((at - now) / 1000) : 0;
    s.last_reason = g_last_reason.load();
    return s;
}

void print_status() {
    if (!g_active) return;
    if (g_assoc.load()) {
        wifi_ap_record_t info{};
        if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) g_rssi.store(info.rssi);
    }
    const Status s = status();
    std::printf("{\"t\":\"wifi\",\"assoc\":%d,\"ip\":%d,\"ch\":%u,\"rssi\":%d,\"associations\":%lu,\"scans\":%lu,"
                "\"retry_in_ms\":%lu,\"last_reason\":%u,\"int_free\":%u,\"int_largest\":%u}\n",
                s.associated ? 1 : 0, s.has_ip ? 1 : 0, static_cast<unsigned>(s.channel), static_cast<int>(s.rssi),
                static_cast<unsigned long>(s.associations), static_cast<unsigned long>(s.scans),
                static_cast<unsigned long>(s.retry_in_ms), static_cast<unsigned>(s.last_reason),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

bool handle_console(const char* line, size_t len) {
    static const char kPrefix[] = "POT! wifi ";
    constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (len < kPrefixLen || std::memcmp(line, kPrefix, kPrefixLen) != 0) return false;
    char buf[256];
    const char* result = "usage";
    if (len >= sizeof(buf) + kPrefixLen) {
        std::printf("{\"t\":\"wifi_cfg\",\"result\":\"too_long\"}\n");
        return true;
    }
    std::memcpy(buf, line + kPrefixLen, len - kPrefixLen);
    buf[len - kPrefixLen] = '\0';
    for (size_t n = std::strlen(buf); n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n'); --n) buf[n - 1] = '\0';
    char* argv[4] = {};
    const size_t argc = split(buf, argv, 4);
    nvs_handle_t h;
    if (argc == 1 && std::strcmp(argv[0], "forget") == 0) {
        result = "erase_failed";
        if (nvs_open(kNvsNs, NVS_READWRITE, &h) == ESP_OK) {
            if (nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK) result = "forgotten";
            nvs_close(h);
        }
    } else if (argc == 3 && std::strcmp(argv[0], "sta") == 0) {
        const size_t sl = std::strlen(argv[1]), pl = std::strlen(argv[2]);
        if (sl < 1 || sl > kSsidMax || pl < kPassMin || pl > kPassMax) {
            result = "refused_length";  // name 1-32 bytes, password 8-64 (WPA2)
        } else {
            result = "save_failed";
            if (nvs_open(kNvsNs, NVS_READWRITE, &h) == ESP_OK) {
                if (nvs_set_str(h, "ssid", argv[1]) == ESP_OK && nvs_set_str(h, "pass", argv[2]) == ESP_OK &&
                    nvs_commit(h) == ESP_OK) {
                    result = "sta_saved";
                }
                nvs_close(h);
            }
        }
    }
    std::memset(buf, 0, sizeof(buf));
    // The result only: never the arguments. "saved" takes effect at the next boot.
    std::printf("{\"t\":\"wifi_cfg\",\"result\":\"%s\"}\n", result);
    return true;
}

#else  // !CONFIG_POT_WIFI

bool start(uint8_t) { return false; }
bool active() { return false; }
bool associated() { return false; }
bool has_ip() { return false; }
uint8_t take_channel() { return 0; }
void prefer_channel(uint8_t) {}
bool repick() { return false; }
uint16_t seen_channels() { return 0; }
bool join_channel(uint8_t) { return false; }
Status status() { return Status{}; }
void print_status() {}
bool handle_console(const char*, size_t) { return false; }

#endif

}  // namespace pot::wifi
