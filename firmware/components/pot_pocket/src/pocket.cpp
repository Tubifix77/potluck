// M13.1: the pocket page, the board half. See pot/pocket.hpp.

#include "pot/pocket.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "sdkconfig.h"

#if CONFIG_POT_POCKET
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#endif

namespace pot::pocket {

#if CONFIG_POT_POCKET

namespace {

const char* kTag = "pot.pocket";
const char* kNvsNs = "pot_pocket";

EXT_RAM_BSS_ATTR Snapshot g_snap;       // the link task's latest, under g_mu
EXT_RAM_BSS_ATTR Snapshot g_render;     // the server's copy
EXT_RAM_BSS_ATTR char g_page[kPageMax];
SemaphoreHandle_t g_mu = nullptr;
StaticSemaphore_t g_mu_buf;
httpd_handle_t g_server = nullptr;
bool g_active = false;
int g_reason = 1;
std::atomic<uint32_t> g_pages{0};
std::atomic<uint32_t> g_refused{0};

size_t take_copy() {
    if (g_mu == nullptr) return 0;
    xSemaphoreTake(g_mu, portMAX_DELAY);
    std::memcpy(&g_render, &g_snap, sizeof(g_snap));
    xSemaphoreGive(g_mu);
    return 1;
}

esp_err_t serve(httpd_req_t* req, bool json) {
    take_copy();
    const size_t n = json ? render_json(g_render, g_page, sizeof(g_page)) : render_html(g_render, g_page, sizeof(g_page));
    if (n == 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "page too large");
        return ESP_OK;
    }
    httpd_resp_set_type(req, json ? "application/json" : "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    g_pages.fetch_add(1);
    return httpd_resp_send(req, g_page, static_cast<ssize_t>(n));
}

esp_err_t get_html(httpd_req_t* req) { return serve(req, false); }
esp_err_t get_json(httpd_req_t* req) { return serve(req, true); }

// Every other method on the two paths: read-only, said plainly. (The server answers 404 for any other path itself.)
esp_err_t refuse(httpd_req_t* req) {
    g_refused.fetch_add(1);
    httpd_resp_set_hdr(req, "Allow", "GET");
    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "read-only: this page changes nothing");
    return ESP_OK;
}

bool load(char* ssid, size_t sl, char* pass, size_t pl) {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) return false;
    const bool ok = nvs_get_str(h, "ssid", ssid, &sl) == ESP_OK && nvs_get_str(h, "pass", pass, &pl) == ESP_OK &&
                    ssid[0] != '\0' && std::strlen(pass) >= 8;
    nvs_close(h);
    return ok;
}

}  // namespace

bool start(bool other_ap) {
    if (other_ap) {
        g_reason = 2;
        return false;
    }
    char ssid[33] = {}, pass[65] = {};
    if (!load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        g_reason = 1;
        ESP_LOGI(kTag, "no access point saved; the module is idle (POT! pocket ap ...)");
        return false;
    }
    g_mu = xSemaphoreCreateMutexStatic(&g_mu_buf);
    wifi_mode_t mode = WIFI_MODE_NULL;
    uint8_t primary = 1;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err == ESP_OK && esp_netif_create_default_wifi_ap() == nullptr) err = ESP_FAIL;
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_APSTA);  // the station stays: ESP-NOW rides on it
    if (err == ESP_OK) err = esp_wifi_get_channel(&primary, &second);
    wifi_config_t ap{};
    std::memcpy(ap.ap.ssid, ssid, std::strlen(ssid));
    ap.ap.ssid_len = static_cast<uint8_t>(std::strlen(ssid));
    std::memcpy(ap.ap.password, pass, std::strlen(pass));
    ap.ap.channel = primary;  // the cell's channel: one radio, one channel
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;  // a phone or two; soft-AP and ESP-NOW share the encryption key slots
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    std::memset(&ap, 0, sizeof(ap));
    std::memset(pass, 0, sizeof(pass));
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "access point not started (%s)", esp_err_to_name(err));
        g_reason = 3;
        return false;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 10;  // 2 paths x (GET + 4 refused methods)
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 4096;
    if (httpd_start(&g_server, &cfg) != ESP_OK) {
        ESP_LOGE(kTag, "HTTP server not started");
        g_reason = 3;
        return false;
    }
    const httpd_method_t others[] = {HTTP_POST, HTTP_PUT, HTTP_DELETE, HTTP_PATCH};
    for (const char* path : {"/", "/status.json"}) {
        httpd_uri_t u{};
        u.uri = path;
        u.method = HTTP_GET;
        u.handler = (std::strcmp(path, "/") == 0) ? &get_html : &get_json;
        bool ok = httpd_register_uri_handler(g_server, &u) == ESP_OK;
        for (httpd_method_t m : others) {
            httpd_uri_t r{};
            r.uri = path;
            r.method = m;
            r.handler = &refuse;
            ok = httpd_register_uri_handler(g_server, &r) == ESP_OK && ok;
        }
        if (!ok) ESP_LOGW(kTag, "a handler for %s was not registered (the server still refuses other methods)", path);
    }
    g_active = true;
    g_reason = 0;
    ESP_LOGI(kTag, "pocket page up on its own access point, channel %u", static_cast<unsigned>(primary));
    return true;
}

bool active() { return g_active; }
int idle_reason() { return g_reason; }

void publish(const Snapshot& s) {
    if (!g_active || g_mu == nullptr) return;
    if (xSemaphoreTake(g_mu, 0) != pdTRUE) return;  // the server is copying: the next second's will do
    std::memcpy(&g_snap, &s, sizeof(s));
    xSemaphoreGive(g_mu);
}

void print_status() {
    if (!g_active) return;
    wifi_sta_list_t sta{};
    const int phones = esp_wifi_ap_get_sta_list(&sta) == ESP_OK ? sta.num : -1;
    std::printf("{\"t\":\"pocket\",\"active\":1,\"phones\":%d,\"pages\":%lu,\"refused\":%lu,\"int_free\":%u,"
                "\"int_largest\":%u}\n",
                phones, static_cast<unsigned long>(g_pages.load()), static_cast<unsigned long>(g_refused.load()),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

bool handle_console(const char* line, size_t len) {
    static const char kPrefix[] = "POT! pocket ";
    constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (len < kPrefixLen || std::memcmp(line, kPrefix, kPrefixLen) != 0) return false;
    char buf[160];
    if (len - kPrefixLen >= sizeof(buf)) {
        std::printf("{\"t\":\"pocket_cfg\",\"result\":\"too_long\"}\n");
        return true;
    }
    std::memcpy(buf, line + kPrefixLen, len - kPrefixLen);
    buf[len - kPrefixLen] = '\0';
    for (size_t n = std::strlen(buf); n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n'); --n) buf[n - 1] = '\0';
    const char* result = "usage";
    nvs_handle_t h;
    if (std::strcmp(buf, "forget") == 0) {
        result = "erase_failed";
        if (nvs_open(kNvsNs, NVS_READWRITE, &h) == ESP_OK) {
            if (nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK) result = "forgotten";
            nvs_close(h);
        }
    } else if (std::strncmp(buf, "ap ", 3) == 0) {
        char* name = buf + 3;
        char* sp = std::strchr(name, ' ');
        if (sp != nullptr) {
            *sp = '\0';
            const char* pass = sp + 1;
            const size_t nl = std::strlen(name), pl = std::strlen(pass);
            if (nl < 1 || nl > 32 || pl < 8 || pl > 63) {
                result = "refused_length";  // name 1-32 bytes; WPA2 password 8-63 characters
            } else {
                result = "save_failed";
                if (nvs_open(kNvsNs, NVS_READWRITE, &h) == ESP_OK) {
                    if (nvs_set_str(h, "ssid", name) == ESP_OK && nvs_set_str(h, "pass", pass) == ESP_OK &&
                        nvs_commit(h) == ESP_OK) {
                        result = "ap_saved";
                    }
                    nvs_close(h);
                }
            }
        }
    }
    std::memset(buf, 0, sizeof(buf));
    std::printf("{\"t\":\"pocket_cfg\",\"result\":\"%s\"}\n", result);  // the result only, never the arguments
    return true;
}

#else  // !CONFIG_POT_POCKET

bool start(bool) { return false; }
bool active() { return false; }
int idle_reason() { return 1; }
void publish(const Snapshot&) {}
void print_status() {}
bool handle_console(const char*, size_t) { return false; }

#endif

}  // namespace pot::pocket
