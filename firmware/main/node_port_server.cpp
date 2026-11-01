#include "node_port_server.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "esp_attr.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "pot/frame.hpp"
#include "pot/node_port.hpp"
#include "pot/serial_framing.hpp"
#include "uplink.hpp"

namespace pot::nodeport {
namespace {

Hooks g_hooks{};
bool g_started = false;
std::atomic<int> g_client{-1};      // the logged-in node's socket
std::atomic<uint16_t> g_node{0};    // its id
std::atomic<uint32_t> g_addr{0};
// The link task cannot send to the node: the server task closes the connection, so the node redials (another board,
// or the hub). Found on the owner's cell 2026-11-01: B's sends to the tablet stopped at 02:42:37 -- each one dropped
// silently -- while the socket stayed open and the tablet's own frames kept arriving, for 13 minutes.
std::atomic<bool> g_kick{false};
uint32_t g_send_fail_since = 0;  // the link task's only (send_to runs under the node mutex)
constexpr uint32_t kSendStuckMs = 3000;
void* g_notify = nullptr;
Counters g_c{};

// The rate a node's frames may come in at, as the hub's (hub_bridge.py RATE_PER_S, RATE_BURST).
constexpr float kRatePerS = 100.0f, kRateBurst = 200.0f;
float g_tokens = kRateBurst;
uint32_t g_tokens_at = 0;

// Frames for the link task: each message is the node's id (2 bytes) and the frame.
constexpr size_t kQueueBytes = 8 * (2 + kEspNowV2LinkMtu + 4);
EXT_RAM_BSS_ATTR uint8_t g_q_storage[kQueueBytes];
StaticMessageBuffer_t g_q_ctl;
MessageBufferHandle_t g_q = nullptr;

EXT_RAM_BSS_ATTR SerialReassembler g_rx;
EXT_RAM_BSS_ATTR uint8_t g_rx_buf[1024];
EXT_RAM_BSS_ATTR uint8_t g_msg[2 + kEspNowV2LinkMtu];
EXT_RAM_BSS_ATTR uint8_t g_wire[kSerialFrameMax + 1];  // send side: under the node mutex, like every hal_send
EXT_RAM_BSS_ATTR char g_line[kLoginLineMax + 1];
EXT_RAM_BSS_ATTR StackType_t g_stack[6144 / sizeof(StackType_t)];
StaticTask_t g_tcb;

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void close_client(int fd) {
    int expected = fd;
    if (g_client.compare_exchange_strong(expected, -1)) g_node.store(0);
    if (fd >= 0) ::close(fd);
}

void on_frame(void*, const uint8_t* frame, size_t len) {
    Frame f;
    const uint16_t node = g_node.load();
    if (parse(frame, len, f) != FrameError::Ok || f.hdr.src != node) {
        ++g_c.dropped_src;  // every frame must be the logged-in node's own
        return;
    }
    const uint32_t t = now_ms();
    g_tokens += static_cast<float>(t - g_tokens_at) * (kRatePerS / 1000.0f);
    if (g_tokens > kRateBurst) g_tokens = kRateBurst;
    g_tokens_at = t;
    if (g_tokens < 1.0f) {
        ++g_c.dropped_rate;
        return;
    }
    g_tokens -= 1.0f;
    g_msg[0] = static_cast<uint8_t>(node >> 8);
    g_msg[1] = static_cast<uint8_t>(node & 0xFF);
    std::memcpy(g_msg + 2, frame, len);
    if (xMessageBufferSend(g_q, g_msg, 2 + len, 0) == 0) {
        ++g_c.dropped_queue;
        return;
    }
    ++g_c.frames_in;
    if (g_notify != nullptr) xTaskNotifyGive(static_cast<TaskHandle_t>(g_notify));
}

struct Check {
    const char* line;
    size_t len;
    const uint8_t* nonce;
    NodePortLogin out;
};
void check_job(void* a) {
    Check& c = *static_cast<Check*>(a);
    c.out = node_port_check(c.line, c.len, c.nonce, g_hooks.ca_pub);
}

bool send_line(int fd, const char* s, size_t n) { return ::send(fd, s, n, 0) == static_cast<ssize_t>(n); }

// One login on a fresh connection, within kLoginTimeoutMs. The node's id, or 0.
uint16_t login(int fd) {
    timeval tv{static_cast<time_t>(kLoginTimeoutMs / 1000), 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t nonce[kNodePortNonceLen];
    esp_fill_random(nonce, sizeof(nonce));
    char hello[96];
    const size_t hn = node_port_challenge(nonce, hello, sizeof(hello));
    if (hn == 0 || !send_line(fd, hello, hn)) return 0;
    size_t n = 0;
    const uint32_t until = now_ms() + kLoginTimeoutMs;
    while (n < kLoginLineMax && static_cast<int32_t>(until - now_ms()) > 0) {
        const ssize_t r = ::recv(fd, g_line + n, 1, 0);  // byte by byte: frames must not follow before "ok"
        if (r <= 0) return 0;
        if (g_line[n] == '\n') break;
        ++n;
    }
    if (n == 0 || n >= kLoginLineMax || g_line[n] != '\n') return 0;
    Check c{g_line, n, nonce, {}};
    g_hooks.run_heavy(nullptr, &check_job, &c);
    if (c.out.node_id == 0) {
        ++g_c.refused;
        char why[160];
        const int wn = std::snprintf(why, sizeof(why), "refused %s\n", c.out.why);
        if (wn > 0) send_line(fd, why, static_cast<size_t>(wn) < sizeof(why) ? static_cast<size_t>(wn) : sizeof(why) - 1);
        return 0;
    }
    char ok[64];
    const size_t on = node_port_ok(c.out.node_id, g_hooks.board_id, g_hooks.board_mac, ok, sizeof(ok));
    if (on == 0 || !send_line(fd, ok, on)) return 0;
    timeval none{0, 200000};  // from here the task polls: a short receive timeout
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));
    return c.out.node_id;
}

uint32_t station_address() {
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip{};
    if (sta == nullptr || esp_netif_get_ip_info(sta, &ip) != ESP_OK || ip.ip.addr == 0) return 0;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&ip.ip.addr);  // network order in memory
    return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
           (static_cast<uint32_t>(b[2]) << 8) | b[3];
}

void task(void*) {
    int lfd = -1;
    uint32_t last_rx = 0;
    for (;;) {
        if (!uplink::has_ip()) {
            close_client(g_client.load());
            if (lfd >= 0) ::close(lfd);
            lfd = -1;
            g_addr.store(0);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (lfd < 0) {
            lfd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (lfd < 0) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            int yes = 1;
            setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(kNodePortDefault);
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            if (::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(lfd, 2) != 0) {
                ::close(lfd);
                lfd = -1;
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            g_addr.store(station_address());
        }
        const int cfd = g_client.load();
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(lfd, &rd);
        int maxfd = lfd;
        if (cfd >= 0) {
            FD_SET(cfd, &rd);
            if (cfd > maxfd) maxfd = cfd;
        }
        timeval tv{1, 0};
        const int ready = ::select(maxfd + 1, &rd, nullptr, nullptr, &tv);
        if (ready > 0 && FD_ISSET(lfd, &rd)) {
            const int nfd = ::accept(lfd, nullptr, nullptr);
            if (nfd >= 0) {
                const uint16_t id = login(nfd);
                if (id == 0) {
                    ::close(nfd);
                } else {
                    const int old = g_client.exchange(-1);
                    if (old >= 0) {
                        ::close(old);
                        ++g_c.replaced;
                    }
                    g_rx.reset();
                    g_tokens = kRateBurst;
                    g_tokens_at = now_ms();
                    g_node.store(id);
                    g_client.store(nfd);
                    last_rx = now_ms();
                    ++g_c.logins;
                }
            }
        }
        const int fd = g_client.load();
        if (fd >= 0 && ready > 0 && FD_ISSET(fd, &rd)) {
            const ssize_t r = ::recv(fd, g_rx_buf, sizeof(g_rx_buf), 0);
            if (r > 0) {
                last_rx = now_ms();
                g_rx.feed(g_rx_buf, static_cast<size_t>(r), &on_frame, nullptr);
            } else if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                close_client(fd);  // closed, or broken
            }
        }
        if (g_kick.exchange(false) && g_client.load() >= 0) {
            ++g_c.send_fail;
            close_client(g_client.load());
            continue;
        }
        if (g_client.load() >= 0 && now_ms() - last_rx > kIdleMs) {
            ++g_c.idle_drops;
            close_client(g_client.load());
        }
    }
}

}  // namespace

bool start(const Hooks& h) {
    if (g_started) return true;
    g_hooks = h;
    g_q = xMessageBufferCreateStatic(sizeof(g_q_storage), g_q_storage, &g_q_ctl);
    if (g_q == nullptr) return false;
    if (xTaskCreateStaticPinnedToCore(task, "pot_nodeport", sizeof(g_stack) / sizeof(StackType_t), nullptr, 4,
                                      g_stack, &g_tcb, tskNO_AFFINITY) == nullptr) {
        return false;
    }
    g_started = true;
    return true;
}

void set_rx_notify(void* task) { g_notify = task; }

bool rx_pop(uint8_t* out, size_t cap, size_t& len, uint16_t& node) {
    if (g_q == nullptr) return false;
    EXT_RAM_BSS_ATTR static uint8_t msg[2 + kEspNowV2LinkMtu];  // the link task's only
    const size_t n = xMessageBufferReceive(g_q, msg, sizeof(msg), 0);
    if (n < 2 + kHeaderSize || n - 2 > cap) return false;
    node = static_cast<uint16_t>((msg[0] << 8) | msg[1]);
    len = n - 2;
    std::memcpy(out, msg + 2, len);
    return true;
}

bool serves(uint16_t node) { return node != 0 && g_client.load() >= 0 && g_node.load() == node; }
bool any() { return g_client.load() >= 0; }
uint32_t address() { return g_addr.load(); }
uint64_t published() {
    const uint32_t a = g_addr.load();
    return a == 0 ? 0 : (static_cast<uint64_t>(a) << 16) | kNodePortDefault;
}

int32_t send_to(uint16_t node, const uint8_t* frame, size_t len) {
    const int fd = g_client.load();
    if (fd < 0 || g_node.load() != node) return -1;
    const size_t n = write_serial_frame(frame, len, g_wire, sizeof(g_wire));
    if (n == 0) return -1;
    // Never block the link task: a socket that will not take a whole frame now loses it, as the radio would -- but a
    // frame half taken has torn the stream, and one refused for kSendStuckMs means the node no longer reads: either
    // way the connection is closed (by the server task), and the node redials.
    const ssize_t sent = ::send(fd, g_wire, n, MSG_DONTWAIT);
    if (sent == static_cast<ssize_t>(n)) {
        g_send_fail_since = 0;
    } else {
        const uint32_t t = now_ms();
        if (sent > 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            g_kick.store(true);
        } else if (g_send_fail_since == 0) {
            g_send_fail_since = t == 0 ? 1 : t;
        } else if (t - g_send_fail_since > kSendStuckMs) {
            g_kick.store(true);
            g_send_fail_since = 0;
        }
    }
    if (sent != static_cast<ssize_t>(n)) {
        ++g_c.send_fail;
        return -1;
    }
    ++g_c.frames_out;
    return 0;
}

void send_all(const uint8_t* frame, size_t len) {
    const uint16_t node = g_node.load();
    if (node != 0) send_to(node, frame, len);
}

Counters counters() { return g_c; }

}  // namespace pot::nodeport
