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

char g_host[20] = "192.168.4.1";  // the access point's own address, set at start: the only Host this page answers

// A request header into buf: 1 read, 0 absent, -1 longer than buf (refused -- never taken as absent: security review
// 2026-10-26, a long Origin was read as none and the check let it through).
int header(httpd_req_t* req, const char* name, char* buf, size_t cap) {
    const size_t n = httpd_req_get_hdr_value_len(req, name);
    if (n == 0) return 0;
    if (n >= cap) return -1;
    return httpd_req_get_hdr_value_str(req, name, buf, cap) == ESP_OK ? 1 : -1;
}

// Host names this board itself (with or without :80): a page from elsewhere rebinding a name to it is refused.
bool host_ok(httpd_req_t* req) {
    char host[32] = {};
    if (header(req, "Host", host, sizeof(host)) != 1) return false;
    char with_port[28];
    std::snprintf(with_port, sizeof(with_port), "%s:80", g_host);
    return std::strcmp(host, g_host) == 0 || std::strcmp(host, with_port) == 0;
}

// The session the request's cookie names (s=<32 hex>), or nullptr. Call under g_mu.
const Session* request_session(httpd_req_t* req) {
    char cookie[160] = {};
    if (header(req, "Cookie", cookie, sizeof(cookie)) != 1) return nullptr;
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

// A form posted from this page, not from another site: Host is this board, and Origin (or, without it, Referer) names it.
bool same_origin(httpd_req_t* req) {
    if (!host_ok(req)) return false;
    char origin[96] = {};
    char want[32];
    std::snprintf(want, sizeof(want), "http://%s", g_host);
    const int o = header(req, "Origin", origin, sizeof(origin));
    if (o != 0) return o == 1 && std::strcmp(origin, want) == 0;
    const int r = header(req, "Referer", origin, sizeof(origin));
    if (r != 0) {
        const size_t wl = std::strlen(want);
        return r == 1 && std::strncmp(origin, want, wl) == 0 && (origin[wl] == '/' || origin[wl] == '\0');
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

void no_framing(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "frame-ancestors 'none'");
}

esp_err_t back_home(httpd_req_t* req, const char* where = "/") {
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", where);
    no_framing(req);
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
    if (!host_ok(req)) return forbid(req, "this page answers at its own address only");
    take_copy();
    PageView v;
    char cookie[96] = {};
    {
        Lock l;
        v.writes_on = g_auth.enabled();
        const Session* s = v.writes_on ? request_session(req) : nullptr;
        if (s != nullptr) {
            v.logged_in = true;
            to_hex(s->csrf, kTokenBytes, v.csrf);
            char id[kTokenHex + 1];
            to_hex(s->id, kTokenBytes, id);
            std::snprintf(cookie, sizeof(cookie), "s=%s; HttpOnly; SameSite=Strict; Path=/; Max-Age=900", id);
            std::snprintf(v.note, sizeof(v.note), "%s", g_note);  // the last write's result: a logged-in phone's only
        }
    }
    char query[16] = {}, r[4] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "r", r, sizeof(r)) == ESP_OK) {
        const char* said = std::strcmp(r, "in") == 0    ? "logged in"
                           : std::strcmp(r, "out") == 0 ? "logged out"
                           : std::strcmp(r, "w") == 0   ? "wrong password"
                           : std::strcmp(r, "l") == 0   ? "too many wrong passwords: try again later"
                           : std::strcmp(r, "o") == 0   ? "writes are off on this board"
                                                        : "";
        if (said[0] != '\0') std::snprintf(v.note, sizeof(v.note), "%s", said);  // fixed words, never the query's
    }
    if (cookie[0] != '\0') httpd_resp_set_hdr(req, "Set-Cookie", cookie);  // in use: the 15 minutes start again
    no_framing(req);
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
    char body[256], pw[kLoginMax + 1];
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
        httpd_resp_set_hdr(req, "Set-Cookie", cookie);
        return back_home(req, "/?r=in");
    }
    return back_home(req, r == LoginResult::Locked ? "/?r=l" : r == LoginResult::Off ? "/?r=o" : "/?r=w");
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
    enum class Verdict { Queued, NoSession, BadCsrf, NotWritable, Slow, Busy } verdict = Verdict::Queued;
    {
        Lock l;  // deciding only: every reply is sent after the lock is given back
        const Session* s = request_session(req);
        if (s == nullptr) {
            ++g_auth.counters().bad_session;
            verdict = Verdict::NoSession;
        } else if (!g_auth.csrf_ok(*s, csrf, std::strlen(csrf))) {
            verdict = Verdict::BadCsrf;
        } else if (!writable) {
            verdict = Verdict::NotWritable;
        } else if (!g_auth.take_write(now_ms())) {
            verdict = Verdict::Slow;
        } else if (g_pending_n >= kPendingMax) {
            verdict = Verdict::Busy;
        } else {
            PendingWrite& w = g_pending[g_pending_n++];
            std::snprintf(w.name, sizeof(w.name), "%s", name);
            std::snprintf(w.value, sizeof(w.value), "%s", value);
            std::snprintf(g_note, sizeof(g_note), "%s: sent", name);
        }
    }
    switch (verdict) {
        case Verdict::NoSession: return forbid(req, "log in first");
        case Verdict::BadCsrf: return forbid(req, "not from this page");
        case Verdict::NotWritable: return forbid(req, "not writable from outside");
        case Verdict::Slow:
            httpd_resp_set_status(req, "429 Too Many Requests");
            httpd_resp_set_hdr(req, "Retry-After", "1");
            return httpd_resp_send(req, "slow down", HTTPD_RESP_USE_STRLEN);
        case Verdict::Busy:
            httpd_resp_set_status(req, "503 Service Unavailable");
            return httpd_resp_send(req, "busy", HTTPD_RESP_USE_STRLEN);
        case Verdict::Queued: break;
    }
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
    return back_home(req, "/?r=out");
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
    esp_netif_t* ap_if = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip{};
    if (ap_if != nullptr && esp_netif_get_ip_info(ap_if, &ip) == ESP_OK && ip.ip.addr != 0) {
        std::snprintf(g_host, sizeof(g_host), IPSTR, IP2STR(&ip.ip));
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;  // 2 paths x (GET + 4 refused methods), and M26's three POSTs
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 5120;  // M26: the login's and the write's buffers (review, 2026-10-26: 4096 was close)
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
    if (xSemaphoreTake(g_mu, 0) != pdTRUE) return false;  // the page holds it: the next pass -- the link task never waits
    const bool any = g_pending_n > 0;
    if (any) {
        *out = g_pending[0];
        for (size_t i = 1; i < g_pending_n; ++i) g_pending[i - 1] = g_pending[i];
        --g_pending_n;
    }
    xSemaphoreGive(g_mu);
    return any;
}

void write_result(const char* name, const char* result) {
    if (g_mu == nullptr || xSemaphoreTake(g_mu, 0) != pdTRUE) return;  // never wait for the page: the note can be missed
    std::snprintf(g_note, sizeof(g_note), "%s: %s", name, result);
    xSemaphoreGive(g_mu);
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
        if (pl < kLoginMin || pl > kLoginMax) {
            result = "refused_length";  // 12-63 characters
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
