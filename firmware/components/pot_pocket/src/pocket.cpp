// M13.1: the pocket page, the board half. See pot/pocket.hpp.

#include "pot/pocket.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "sdkconfig.h"

#include "pot/pocket_auth.hpp"

#if CONFIG_POT_POCKET
#include <utility>

#include "esp_attr.h"
#include "esp_random.h"
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
// M26: the login and its sessions (internal RAM, a few hundred bytes -- the kill line is 4 KB), the writes waiting for
// the link task and the last result. All under g_mu: the server's task, the link task and the console share them.
Auth g_auth;
constexpr size_t kPendingMax = 2;
PendingWrite g_pending[kPendingMax];
size_t g_pending_n = 0;
char g_note[80] = {};

void fill_random(uint8_t* out, size_t n) { esp_fill_random(out, n); }
uint32_t now_ms() { return static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS); }

struct Lock {
    Lock() {
        if (g_mu != nullptr) xSemaphoreTake(g_mu, portMAX_DELAY);
    }
    ~Lock() {
        if (g_mu != nullptr) xSemaphoreGive(g_mu);
    }
};

void set_note(const char* text) {
    Lock l;
    std::snprintf(g_note, sizeof(g_note), "%s", text);
}

// The session the request's cookie names (s=<32 hex>), or nullptr. Call under g_mu.
const Session* request_session(httpd_req_t* req) {
    char cookie[160] = {};
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) return nullptr;
    const char* at = cookie;
    while ((at = std::strstr(at, "s=")) != nullptr) {
        if (at == cookie || at[-1] == ' ' || at[-1] == ';') break;
        at += 2;
    }
    if (at == nullptr) return nullptr;
    at += 2;
    size_t n = 0;
    while (at[n] != '\0' && at[n] != ';' && at[n] != ' ') ++n;
    return g_auth.session(at, n, now_ms());
}

// A form posted from this page, not from another site: Origin (or, without it, Referer) names this board's address.
bool same_origin(httpd_req_t* req) {
    char host[64] = {}, origin[96] = {};
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return false;
    char want[80];
    std::snprintf(want, sizeof(want), "http://%s", host);
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) == ESP_OK) return std::strcmp(origin, want) == 0;
    if (httpd_req_get_hdr_value_str(req, "Referer", origin, sizeof(origin)) == ESP_OK) {
        const size_t wl = std::strlen(want);
        return std::strncmp(origin, want, wl) == 0 && (origin[wl] == '/' || origin[wl] == '\0');
    }
    return true;  // neither (an old browser): the SameSite=Strict cookie and the form token still stand
}

// The form body, at most cap - 1 bytes; false if longer or not received.
bool read_body(httpd_req_t* req, char* buf, size_t cap) {
    if (req->content_len == 0 || req->content_len >= cap) return false;
    size_t got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    buf[got] = '\0';
    return true;
}

esp_err_t back_home(httpd_req_t* req) {
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t forbid(httpd_req_t* req, const char* why) {
    g_refused.fetch_add(1);
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, why);
    return ESP_OK;
}

size_t take_copy() {
    if (g_mu == nullptr) return 0;
    xSemaphoreTake(g_mu, portMAX_DELAY);
    std::memcpy(&g_render, &g_snap, sizeof(g_snap));
    xSemaphoreGive(g_mu);
    return 1;
}

esp_err_t serve(httpd_req_t* req, bool json) {
    take_copy();
    PageView v;
    {
        Lock l;
        v.writes_on = g_auth.enabled();
        const Session* s = v.writes_on ? request_session(req) : nullptr;
        if (s != nullptr) {
            v.logged_in = true;
            to_hex(s->csrf, kTokenBytes, v.csrf);
        }
        std::snprintf(v.note, sizeof(v.note), "%s", g_note);
    }
    const size_t n =
        json ? render_json(g_render, g_page, sizeof(g_page)) : render_html(g_render, v, g_page, sizeof(g_page));
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

// M26: POST /login {password}. A right one sets the session cookie; any answer goes back to the page, which says how.
esp_err_t post_login(httpd_req_t* req) {
    char body[128], pw[kLoginMax + 1];
    if (!same_origin(req)) {
        {
            Lock l;
            ++g_auth.counters().bad_origin;
        }
        return forbid(req, "not from this page");
    }
    if (!read_body(req, body, sizeof(body)) || !form_field(body, std::strlen(body), "password", pw, sizeof(pw))) {
        return forbid(req, "no password");
    }
    const Session* s = nullptr;
    LoginResult r;
    char cookie[96] = {};
    {
        Lock l;
        r = g_auth.login(pw, std::strlen(pw), now_ms(), &fill_random, &s);
        if (r == LoginResult::Ok) {
            char id[kTokenHex + 1];
            to_hex(s->id, kTokenBytes, id);
            std::snprintf(cookie, sizeof(cookie), "s=%s; HttpOnly; SameSite=Strict; Path=/; Max-Age=900", id);
        }
    }
    std::memset(pw, 0, sizeof(pw));
    std::memset(body, 0, sizeof(body));
    if (r == LoginResult::Ok) {
        set_note("logged in");
        httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    } else {
        set_note(r == LoginResult::Locked ? "too many wrong passwords: try again in a minute"
                 : r == LoginResult::Off  ? "writes are off on this board"
                                          : "wrong password");
    }
    return back_home(req);
}

// M26: POST /write {name, value, csrf}: a logged-in phone's change, checked here and queued for the link task.
esp_err_t post_write(httpd_req_t* req) {
    char body[192], name[41], value[24], csrf[kTokenHex + 1];
    if (!same_origin(req)) {
        {
            Lock l;
            ++g_auth.counters().bad_origin;
        }
        return forbid(req, "not from this page");
    }
    if (!read_body(req, body, sizeof(body)) || !form_field(body, std::strlen(body), "name", name, sizeof(name)) ||
        !form_field(body, std::strlen(body), "value", value, sizeof(value)) ||
        !form_field(body, std::strlen(body), "csrf", csrf, sizeof(csrf))) {
        return forbid(req, "name, value and csrf");
    }
    take_copy();
    bool writable = false;
    for (size_t i = 0; i < g_render.n_items; ++i) {
        if (std::strcmp(g_render.items[i].name, name) == 0) writable = g_render.items[i].writable;
    }
    Lock l;
    const Session* s = request_session(req);
    if (s == nullptr) {
        ++g_auth.counters().bad_session;
        return forbid(req, "log in first");
    }
    if (!g_auth.csrf_ok(*s, csrf, std::strlen(csrf))) return forbid(req, "not from this page");
    if (!writable) return forbid(req, "not writable from outside");
    if (!g_auth.take_write(now_ms())) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_set_hdr(req, "Retry-After", "1");
        return httpd_resp_send(req, "slow down", HTTPD_RESP_USE_STRLEN);
    }
    if (g_pending_n >= kPendingMax) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "busy", HTTPD_RESP_USE_STRLEN);
    }
    PendingWrite& w = g_pending[g_pending_n++];
    std::snprintf(w.name, sizeof(w.name), "%s", name);
    std::snprintf(w.value, sizeof(w.value), "%s", value);
    std::snprintf(g_note, sizeof(g_note), "%s: sent", name);
    return back_home(req);
}

esp_err_t post_logout(httpd_req_t* req) {
    char body[96], csrf[kTokenHex + 1];
    if (!same_origin(req) || !read_body(req, body, sizeof(body)) ||
        !form_field(body, std::strlen(body), "csrf", csrf, sizeof(csrf))) {
        return forbid(req, "not from this page");
    }
    {
        Lock l;
        const Session* s = request_session(req);
        if (s != nullptr && g_auth.csrf_ok(*s, csrf, std::strlen(csrf))) g_auth.logout(s);
    }
    set_note("logged out");
    return back_home(req);
}

bool load_login() {
    nvs_handle_t h;
    uint8_t blob[kSaltBytes + kHashBytes];
    size_t n = sizeof(blob);
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) return false;
    const bool ok = nvs_get_blob(h, "login", blob, &n) == ESP_OK && n == sizeof(blob);
    nvs_close(h);
    if (ok) g_auth.set_login(blob, blob + kSaltBytes);
    return ok;
}

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
    load_login();  // M26: writes on only with a saved login
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;  // 2 paths x (GET + 4 refused methods), and M26's three POSTs
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
    const std::pair<const char*, esp_err_t (*)(httpd_req_t*)> posts[] = {
        {"/login", &post_login}, {"/write", &post_write}, {"/logout", &post_logout}};
    for (const auto& pr : posts) {
        httpd_uri_t u{};
        u.uri = pr.first;
        u.method = HTTP_POST;
        u.handler = pr.second;
        if (httpd_register_uri_handler(g_server, &u) != ESP_OK) ESP_LOGW(kTag, "%s not registered", pr.first);
    }
    g_active = true;
    g_reason = 0;
    ESP_LOGI(kTag, "pocket page up on its own access point, channel %u", static_cast<unsigned>(primary));
    return true;
}

bool active() { return g_active; }
int idle_reason() { return g_reason; }

bool take_write(PendingWrite* out) {
    if (!g_active || g_mu == nullptr) return false;
    Lock l;
    if (g_pending_n == 0) return false;
    *out = g_pending[0];
    for (size_t i = 1; i < g_pending_n; ++i) g_pending[i - 1] = g_pending[i];
    --g_pending_n;
    return true;
}

void write_result(const char* name, const char* result) {
    Lock l;
    std::snprintf(g_note, sizeof(g_note), "%s: %s", name, result);
}

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
    AuthCounters c;
    bool on;
    {
        Lock l;
        c = g_auth.counters();
        on = g_auth.enabled();
    }
    std::printf("{\"t\":\"pocket\",\"active\":1,\"phones\":%d,\"pages\":%lu,\"refused\":%lu,\"writes_on\":%d,"
                "\"logins\":%lu,\"bad_passwords\":%lu,\"locked\":%lu,\"bad_session\":%lu,\"bad_csrf\":%lu,"
                "\"bad_origin\":%lu,\"rate_limited\":%lu,\"writes\":%lu,\"auth_bytes\":%u,\"int_free\":%u,"
                "\"int_largest\":%u}\n",
                phones, static_cast<unsigned long>(g_pages.load()), static_cast<unsigned long>(g_refused.load()),
                on ? 1 : 0, static_cast<unsigned long>(c.logins), static_cast<unsigned long>(c.bad_passwords),
                static_cast<unsigned long>(c.locked), static_cast<unsigned long>(c.bad_session),
                static_cast<unsigned long>(c.bad_csrf), static_cast<unsigned long>(c.bad_origin),
                static_cast<unsigned long>(c.rate_limited), static_cast<unsigned long>(c.writes),
                static_cast<unsigned>(sizeof(g_auth) + sizeof(g_pending) + sizeof(g_note)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

bool figures(uint32_t* phones, uint32_t* pages, uint32_t* refused) {
    if (!g_active) return false;
    wifi_sta_list_t sta{};
    *phones = esp_wifi_ap_get_sta_list(&sta) == ESP_OK ? static_cast<uint32_t>(sta.num) : 0;
    *pages = g_pages.load();
    *refused = g_refused.load();
    return true;
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
    } else if (std::strcmp(buf, "login off") == 0) {
        result = "erase_failed";
        if (nvs_open(kNvsNs, NVS_READWRITE, &h) == ESP_OK) {
            const esp_err_t e = nvs_erase_key(h, "login");
            if ((e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(h) == ESP_OK) result = "writes_off";
            nvs_close(h);
        }
        if (std::strcmp(result, "writes_off") == 0) {
            Lock l;
            g_auth.clear_login();
        }
    } else if (std::strncmp(buf, "login ", 6) == 0) {
        const char* pw = buf + 6;
        const size_t pl = std::strlen(pw);
        if (pl < 1 || pl > kLoginMax) {
            result = "refused_length";
        } else {
            uint8_t blob[kSaltBytes + kHashBytes];
            esp_fill_random(blob, kSaltBytes);
            hash_login(blob, pw, pl, blob + kSaltBytes);
            result = "save_failed";
            if (nvs_open(kNvsNs, NVS_READWRITE, &h) == ESP_OK) {
                if (nvs_set_blob(h, "login", blob, sizeof(blob)) == ESP_OK && nvs_commit(h) == ESP_OK) result = "writes_on";
                nvs_close(h);
            }
            if (std::strcmp(result, "writes_on") == 0) {
                Lock l;
                g_auth.set_login(blob, blob + kSaltBytes);
            }
            std::memset(blob, 0, sizeof(blob));
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
bool figures(uint32_t*, uint32_t*, uint32_t*) { return false; }
bool handle_console(const char*, size_t) { return false; }
bool take_write(PendingWrite*) { return false; }
void write_result(const char*, const char*) {}

#endif

}  // namespace pot::pocket
