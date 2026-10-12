// M13: the presenting role, the board half. See pot/present.hpp.

#include "pot/present.hpp"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <new>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "pot/deploy.hpp"
#include "pot/node.hpp"
#include "pot/present_core.hpp"
#include "sdkconfig.h"

namespace pot::present {

namespace {

const char* kTag = "pot.present";
const char* kNvsNs = "pot_present";
constexpr size_t kBufLen = 1536;
constexpr size_t kCmdDepth = 8;
constexpr uint32_t kRediscoverMs = 300000;  // configs again every 5 min: a resource that was missing gets one
constexpr size_t kConfigsPerTick = 2;       // keep each pass of the link task short
constexpr size_t kOutLen = 16384;           // what the link task has handed the sender and it has not sent yet
constexpr size_t kRecLen = kBufLen + 128;   // one record: a kind byte, a topic, a NUL, a payload

struct Cmd {
    uint32_t hash;
    uint8_t len;
    char payload[24];
};

// What one holding of the role connects with. The link task builds it in start() and hands it over inside the 'S'
// record; the sender keeps its own copy (State::active), which the client and its event handler read -- so nothing
// the link task writes is ever read by another task (found by review, M0-LOG session 41: a role that left and came
// back quickly rewrote strings the old client's handler and the sender were still reading).
struct StartCfg {
    char uri[40];
    char client_id[40];
    char will_topic[64];
    char will_msg[64];
    char cmd_filter[48];
    char status_topic[48];
    char cell_id[9];
    char system[32];
    char user[65];
    char pass[65];
    uint16_t presenters[kMaxEligible];
    uint8_t n_presenters;
    uint8_t keepalive_s;
    bool login;
};

// One presenter per board, so its state is one block, in PSRAM where there is any: only tasks touch it.
struct State {
    // The sender task's alone: created, used and destroyed there (sender_task), never by the link task.
    esp_mqtt_client_handle_t client;
    esp_mqtt_client_config_t mc;
    StartCfg active;  // the sender's; also read by on_mqtt, on the current client's task
    StartCfg build;   // the link task's, posted with 'S' 
    MessageBufferHandle_t out;
    StaticMessageBuffer_t out_ctl;
    uint8_t out_store[kOutLen + 1];
    TaskHandle_t sender;
    char rec_in[kRecLen];   // the sender's
    char rec_out[kRecLen];  // the link task's
    std::atomic<bool> connected;
    std::atomic<bool> need_discovery;
    std::atomic<bool> holding;
    QueueHandle_t cmds;
    StaticQueue_t q_ctl;
    uint8_t q_store[kCmdDepth * sizeof(Cmd)];
    // The link task's: the actor's view of the cell (topics it builds in tick()).
    char cell_id[9];
    char system[32];
    uint16_t presenters[kMaxEligible];
    size_t n_presenters;
    char buf[kBufLen];
    char topic[112];
    std::atomic<uint32_t> connects, errors, cmds_dropped;
    uint32_t configs, states, writes_sent, writes_applied, writes_refused, enqueue_fail;
    std::atomic<uint32_t> published, publish_fail;
    bool sntp_started;
};
EXT_RAM_BSS_ATTR State g;

// The link task's view.
Cell cell() {
    Cell c;
    c.id = g.cell_id;
    c.system = g.system;
    c.prefix = CONFIG_POT_PRESENT_PREFIX;
    c.presenters = g.presenters;
    c.n_presenters = g.n_presenters;
    return c;
}

// The sender's (and the client's event handler's) view: the holding the current client was started for.
Cell cell_active() {
    Cell c;
    c.id = g.active.cell_id;
    c.system = g.active.system;
    c.prefix = CONFIG_POT_PRESENT_PREFIX;
    c.presenters = g.active.presenters;
    c.n_presenters = g.active.n_presenters;
    return c;
}

// Hand the sender task one record, never waiting: a full buffer drops it, counted (enqueue_fail). The link task
// builds records; only the sender touches the network.
bool post(char kind, const char* topic, const char* data, size_t len) {
    if (g.out == nullptr) return false;
    const size_t tl = topic != nullptr ? std::strlen(topic) : 0;
    if (2 + tl + len > kRecLen) {
        ++g.enqueue_fail;
        return false;
    }
    g.rec_out[0] = kind;
    if (tl) std::memcpy(g.rec_out + 1, topic, tl);
    g.rec_out[1 + tl] = '\0';
    if (len) std::memcpy(g.rec_out + 2 + tl, data, len);
    if (xMessageBufferSend(g.out, g.rec_out, 2 + tl + len, 0) == 0) {
        ++g.enqueue_fail;
        return false;
    }
    return true;
}

// QoS 0, never retained (section 13: no retained state).
void send(const char* topic, const char* data, size_t len) { post('P', topic, data, len); }

void on_mqtt(void*, esp_event_base_t, int32_t id, void* data) {
    auto* ev = static_cast<esp_mqtt_event_handle_t>(data);
    switch (static_cast<esp_mqtt_event_id_t>(id)) {
        case MQTT_EVENT_CONNECTED:
            g.connected.store(true);
            g.need_discovery.store(true);
            g.connects.fetch_add(1);
            esp_mqtt_client_subscribe_single(ev->client, g.active.cmd_filter, 0);
            esp_mqtt_client_subscribe_single(ev->client, g.active.status_topic, 0);
            ESP_LOGI(kTag, "connected to the broker");
            break;
        case MQTT_EVENT_DISCONNECTED:
            g.connected.store(false);
            break;
        case MQTT_EVENT_DATA: {
            if (ev->current_data_offset != 0 || ev->data_len != ev->total_data_len) break;  // fragments: not ours
            const size_t tl = static_cast<size_t>(ev->topic_len);
            if (tl == std::strlen(g.active.status_topic) && std::strncmp(ev->topic, g.active.status_topic, tl) == 0) {
                // Home Assistant's birth message: it (re)started and forgot every entity -- send the configs again.
                if (ev->data_len == 6 && std::strncmp(ev->data, "online", 6) == 0) g.need_discovery.store(true);
                break;
            }
            Cmd c{};
            const Cell cl = cell_active();
            if (!parse_command_topic(cl, ev->topic, tl, c.hash)) break;
            if (ev->data_len <= 0 || static_cast<size_t>(ev->data_len) >= sizeof(c.payload)) {
                c.len = 0;  // refused when handled: too long to be a value
            } else {
                c.len = static_cast<uint8_t>(ev->data_len);
                std::memcpy(c.payload, ev->data, c.len);
            }
            if (g.cmds == nullptr || xQueueSend(g.cmds, &c, 0) != pdTRUE) g.cmds_dropped.fetch_add(1);
            break;
        }
        case MQTT_EVENT_ERROR:
            g.errors.fetch_add(1);
            break;
        default:
            break;
    }
}

// The sender: the one task that talks to the broker. It takes records in order -- 'S' start a client from g.mc, 'X'
// say goodbye on our availability topic and destroy it, 'P' publish -- so a broker that does not answer blocks
// this task, never the link task, and esp-mqtt's own outbox (one queued message per pass of its loop, a pass
// lasting up to its 1 s read poll: found on the bench, outbox at 9.7 KB and states 40 s old) is not used.
void stop_client() {
    if (g.client == nullptr) return;
    if (g.connected.load()) esp_mqtt_client_publish(g.client, g.active.will_topic, g.active.will_msg, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_mqtt_client_disconnect(g.client);
    esp_mqtt_client_destroy(g.client);
    g.client = nullptr;
    g.connected.store(false);
}

void on_mqtt(void*, esp_event_base_t, int32_t id, void* data);

void sender_task(void*) {
    for (;;) {
        const size_t n = xMessageBufferReceive(g.out, g.rec_in, sizeof(g.rec_in) - 1, portMAX_DELAY);
        if (n < 2) continue;
        g.rec_in[n] = '\0';
        const char kind = g.rec_in[0];
        if (kind == 'S') {
            stop_client();  // the old client's task is gone before its handler's strings change
            if (n - 2 != sizeof(StartCfg)) continue;
            std::memcpy(&g.active, g.rec_in + 2, sizeof(StartCfg));
            esp_mqtt_client_config_t& mc = g.mc;
            mc = esp_mqtt_client_config_t{};
            mc.broker.address.uri = g.active.uri;
            mc.credentials.client_id = g.active.client_id;
            if (g.active.login) {
                mc.credentials.username = g.active.user;
                mc.credentials.authentication.password = g.active.pass;
            }
            mc.session.keepalive = g.active.keepalive_s;
            mc.session.last_will.topic = g.active.will_topic;
            mc.session.last_will.msg = g.active.will_msg;
            mc.session.last_will.msg_len = static_cast<int>(std::strlen(g.active.will_msg));
            mc.session.last_will.qos = 1;
            mc.session.last_will.retain = 0;
            mc.network.reconnect_timeout_ms = 3000;
            mc.buffer.size = 2048;
            g.client = esp_mqtt_client_init(&mc);
            if (g.client == nullptr) {
                ESP_LOGE(kTag, "MQTT client not created (out of memory?)");
                g.errors.fetch_add(1);
                continue;
            }
            esp_mqtt_client_register_event(g.client, MQTT_EVENT_ANY, &on_mqtt, nullptr);
            if (esp_mqtt_client_start(g.client) != ESP_OK) {
                esp_mqtt_client_destroy(g.client);
                g.client = nullptr;
                g.errors.fetch_add(1);
            }
        } else if (kind == 'X') {
            stop_client();
        } else if (kind == 'P' && g.client != nullptr && g.connected.load()) {
            const char* topic = g.rec_in + 1;
            const size_t tl = std::strlen(topic);
            const char* payload = topic + tl + 1;
            const int pl = static_cast<int>(n - 2 - tl);
            if (esp_mqtt_client_publish(g.client, topic, payload, pl, 0, 0) < 0) {
                g.publish_fail.fetch_add(1);
            } else {
                g.published.fetch_add(1);
            }
        }
    }
}

bool load_login(StartCfg& c) {
    nvs_handle_t h;
    c.user[0] = c.pass[0] = '\0';
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) return false;
    size_t ul = sizeof(c.user), pl = sizeof(c.pass);
    const bool ok = nvs_get_str(h, "user", c.user, &ul) == ESP_OK && nvs_get_str(h, "pass", c.pass, &pl) == ESP_OK;
    nvs_close(h);
    if (!ok) c.user[0] = c.pass[0] = '\0';
    return ok;
}

class PresentActor : public Actor {
  public:
    PresentActor(Node& node, const PresentConfig& cfg, const BoardServices* board)
        : node_(node), cfg_(cfg), board_(board) {}

    bool start(uint32_t now_ms) override {
        if (board_ == nullptr || board_->outside_at == nullptr || board_->cell_id == nullptr) return false;
        const uint16_t self = node_.config().node_id;
        std::snprintf(g.cell_id, sizeof(g.cell_id), "%s", board_->cell_id(board_->ctx));
        std::snprintf(g.system, sizeof(g.system), "%s",
                      board_->system_name != nullptr ? board_->system_name(board_->ctx) : "lab");
        g.n_presenters = cfg_.place.count;
        for (size_t i = 0; i < g.n_presenters; ++i) g.presenters[i] = cfg_.place.node[i];
        const Cell cl = cell();
        StartCfg& b = g.build;
        b = StartCfg{};
        command_filter(cl, b.cmd_filter, sizeof(b.cmd_filter));
        std::snprintf(b.status_topic, sizeof(b.status_topic), "%s/status", CONFIG_POT_PRESENT_PREFIX);
        presenter_topic(cl, self, b.will_topic, sizeof(b.will_topic));
        will_payload(self, b.will_msg, sizeof(b.will_msg));
        std::snprintf(b.uri, sizeof(b.uri), "mqtt://%u.%u.%u.%u:%u", cfg_.ip[0], cfg_.ip[1], cfg_.ip[2], cfg_.ip[3],
                      static_cast<unsigned>(cfg_.port));
        std::snprintf(b.client_id, sizeof(b.client_id), "potluck-%s-%04x", g.cell_id, static_cast<unsigned>(self));
        std::memcpy(b.cell_id, g.cell_id, sizeof(b.cell_id));
        std::memcpy(b.system, g.system, sizeof(b.system));
        b.n_presenters = static_cast<uint8_t>(g.n_presenters);
        for (size_t i = 0; i < g.n_presenters; ++i) b.presenters[i] = g.presenters[i];
        b.keepalive_s = cfg_.keepalive_s;
        b.login = load_login(b);
        const bool login = b.login;
        if (g.cmds == nullptr) {
            g.cmds = xQueueCreateStatic(kCmdDepth, sizeof(Cmd), g.q_store, &g.q_ctl);
        } else {
            xQueueReset(g.cmds);
        }
        if (!g.sntp_started) {
            // The network's time, for the states' sampled_ms (section 13: SNTP on the presenting board). Display only:
            // ages come from the cell, never from this clock (section 9.4's SNTP row).
            esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            g.sntp_started = esp_netif_sntp_init(&sc) == ESP_OK;
        }
        if (g.out == nullptr) g.out = xMessageBufferCreateStatic(sizeof(g.out_store), g.out_store, &g.out_ctl);
        if (g.sender == nullptr &&
            xTaskCreatePinnedToCore(&sender_task, "pot_present", 4096, nullptr, 4, &g.sender, tskNO_AFFINITY) != pdPASS) {
            g.sender = nullptr;  // tried again at the next holding
            ESP_LOGE(kTag, "no memory for the sender task");
            return false;
        }
        g.connected.store(false);
        g.need_discovery.store(false);
        // Records left from an earlier holding go first and are harmless: the sender drops a publish with no client,
        // and this 'S' replaces any client still there. Only the sender reads the buffer, so only the link task writes.
        if (!post('S', nullptr, reinterpret_cast<const char*>(&b), sizeof(b))) return false;
        std::memset(b.pass, 0, sizeof(b.pass));
        g.holding.store(true);
        next_ms_ = now_ms;
        rediscover_ms_ = now_ms + kRediscoverMs;
        ESP_LOGI(kTag, "presenting the cell to %s%s", b.uri, login ? " (with a login)" : "");
        return true;
    }

    ~PresentActor() override {
        g.holding.store(false);
        // The sender says goodbye and destroys the client; states still queued go first, which is harmless. A full
        // buffer (a sender stuck on a broker that does not answer) drops the 'X': the next 'S' replaces the client.
        if (g.out != nullptr && !post('X', nullptr, nullptr, 0)) ESP_LOGW(kTag, "goodbye not queued (buffer full)");
        ESP_LOGI(kTag, "role handed on");
    }

    void tick(uint32_t now) override {
        Cmd c;
        while (g.cmds != nullptr && xQueueReceive(g.cmds, &c, 0) == pdTRUE) handle(c, now);
        if (!g.connected.load()) return;
        if (g.need_discovery.exchange(false)) {
            disc_cursor_ = 0;
            discovering_ = true;
        }
        if (static_cast<int32_t>(now - rediscover_ms_) >= 0) {
            rediscover_ms_ = now + kRediscoverMs;
            disc_cursor_ = 0;
            discovering_ = true;
        }
        if (discovering_) discover_some(now);
        if (static_cast<int32_t>(now - next_ms_) < 0) return;
        next_ms_ = now + cfg_.place.period_ms;
        publish_all();
    }

    size_t stats_json(char* buf, size_t cap, uint32_t) override {
        const int n = std::snprintf(buf, cap,
                                    "{\"t\":\"present_actor\",\"node\":%u,\"connected\":%d,\"entities\":%u,"
                                    "\"configs\":%lu,\"states\":%lu}",
                                    static_cast<unsigned>(node_.config().node_id), g.connected.load() ? 1 : 0,
                                    static_cast<unsigned>(entities_), static_cast<unsigned long>(g.configs),
                                    static_cast<unsigned long>(g.states));
        return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
    }

  private:
    bool rule(size_t i, uint32_t& hash, uint8_t& flags, const char*& name, size_t& len) const {
        return board_->outside_at(board_->ctx, i, &hash, &flags, &name, &len);
    }

    bool find_rule(uint32_t hash, uint8_t& flags, const char*& name, size_t& len) const {
        uint32_t h = 0;
        for (size_t i = 0; rule(i, h, flags, name, len); ++i) {
            if (h == hash) return true;
        }
        name = "";  // not named by the package: say nothing about another resource
        len = 0;
        flags = 0;
        return false;
    }

    void discover_some(uint32_t now) {
        const Cell cl = cell();
        for (size_t k = 0; k < kConfigsPerTick; ++k) {
            uint32_t hash = 0;
            uint8_t flags = 0;
            const char* name = nullptr;
            size_t len = 0;
            if (!rule(disc_cursor_, hash, flags, name, len)) {
                discovering_ = false;
                // A resource not yet known here, or whose type is not known until it first publishes (a portable
                // actor's output): try again in 10 s rather than at the next 5-minute pass.
                if (missed_) rediscover_ms_ = now + 10000;
                missed_ = false;
                return;
            }
            ++disc_cursor_;
            if ((flags & kOutsideExposed) == 0) continue;  // a safety mark alone: nothing to show
            Reading r;
            node_.read(hash, r);  // adopts the replica if this board has none yet
            const NsEntry* e = node_.ns().find(hash);
            if (e == nullptr) {
                missed_ = true;
                continue;
            }
            const Entity en = entity_of(*e, name, len);
            if (component_of(en) == Component::None) {
                if (e->value.type == ValueType::None) missed_ = true;  // unknown yet; Bytes and 64-bit never are
                continue;
            }
            const size_t n = config_payload(cl, en, cfg_.place.period_ms, g.buf, kBufLen);
            if (n == 0 || config_topic(cl, en, g.topic, sizeof(g.topic)) == 0) continue;
            send(g.topic, g.buf, n);
            ++g.configs;
        }
    }

    void publish_all() {
        const Cell cl = cell();
        const uint16_t self = node_.config().node_id;
        uint32_t down[kMaxOutside];
        size_t n_down = 0;
        size_t entities = 0;
        const uint64_t unix_ms = board_->unix_ms != nullptr ? board_->unix_ms(board_->ctx) : 0;
        uint32_t hash = 0;
        uint8_t flags = 0;
        const char* name = nullptr;
        size_t len = 0;
        for (size_t i = 0; rule(i, hash, flags, name, len); ++i) {
            if ((flags & kOutsideExposed) == 0) continue;
            Reading r;
            bool local = true;
            if (node_.read(hash, r, &local) != NsError::Ok) continue;
            const NsEntry* e = node_.ns().find(hash);
            if (e == nullptr) continue;
            ++entities;
            // Polling: refresh the replica from its owner for the next period (SUBSCRIBE is not built).
            if (!local && e->owner_node != 0 && e->owner_node != self) node_.request_read(e->owner_node, hash);
            const StalenessPolicy pol = static_cast<StalenessPolicy>(e->staleness_policy);
            if (!shows_value(r, pol) && n_down < kMaxOutside) down[n_down++] = hash;
            const size_t n = state_payload(r, pol, unix_ms, g.buf, kBufLen);
            if (n != 0 && state_topic(cl, hash, g.topic, sizeof(g.topic)) != 0) {
                send(g.topic, g.buf, n);
                ++g.states;
            }
        }
        // The availability last, so its "down" list says what the states just sent say.
        uint16_t live[kMaxPeers + 1];
        size_t n_live = 0;
        live[n_live++] = self;
        for (size_t i = 0; i < PeerTable::capacity() && n_live < kMaxPeers + 1; ++i) {
            const PeerLink& p = node_.peers().slot(i);
            if (p.state == PeerState::Alive && p.node_id != 0) live[n_live++] = p.node_id;
        }
        const size_t n = presenter_payload(self, live, n_live, down, n_down, g.buf, kBufLen);
        if (n != 0 && presenter_topic(cl, self, g.topic, sizeof(g.topic)) != 0) send(g.topic, g.buf, n);
        entities_ = entities;
        node_.publish(cfg_.place.out_hash, Value::of_i32(static_cast<int32_t>(entities)));
    }

    // An outside write: the presenter's check first (the package's word), then the owner's, which checks again.
    void handle(const Cmd& c, uint32_t now) {
        uint8_t flags = 0;
        const char* name = "";
        size_t len = 0;
        const char* result = "sent to its owner";
        Value v;
        const NsEntry* e = node_.ns().find(c.hash);
        if (!find_rule(c.hash, flags, name, len) || (flags & kOutsideWritable) == 0 || (flags & kOutsideSafety) != 0) {
            result = "refused: not writable from outside";
        } else if (e == nullptr) {
            result = "refused: not known on the presenting board";
        } else if (c.len == 0 || !parse_command_value(e->value.type, c.payload, c.len, v)) {
            result = "refused: not a value of its type";
        } else if (e->owner_node == node_.config().node_id) {
            // The node's clock, the one every age in this namespace is measured on (found on the bench: the log's
            // clock ran a few ms ahead and the next state carried an age of 2^32 - 18 ms).
            const NsError st = node_.ns().write_outside(c.hash, v, now);
            result = st == NsError::Ok ? "applied" : "refused by the owner";
        } else if (e->owner_node == 0 || node_.request_write(e->owner_node, c.hash, v, true) == 0) {
            result = "refused: its owner is not reachable";
        }
        if (std::strncmp(result, "refused", 7) == 0) {
            ++g.writes_refused;
        } else if (result[0] == 'a') {
            ++g.writes_applied;
        } else {
            ++g.writes_sent;
        }
        ESP_LOGI(kTag, "outside write to %.*s: %s", static_cast<int>(len), name, result);
        const Cell cl = cell();
        const size_t n = write_event(name, len, c.hash, c.payload, c.len, result, g.buf, kBufLen);
        if (n != 0 && g.connected.load() && event_topic(cl, g.topic, sizeof(g.topic)) != 0) send(g.topic, g.buf, n);
    }

    Node& node_;
    PresentConfig cfg_;
    const BoardServices* board_;
    uint32_t next_ms_ = 0;
    uint32_t rediscover_ms_ = 0;
    size_t disc_cursor_ = 0;
    bool discovering_ = false;
    bool missed_ = false;
    size_t entities_ = 0;
};

bool present_check(const ActorDecl& d, const ActorEnv&, const char** why) {
    PresentConfig c{};
    if (d.node_id != kPortableNode || !present_config(d, c)) {
        *why = "present must be portable, with a broker address, port and a keep-alive of 5..120 s";
        return false;
    }
    return true;
}

Actor* present_create(void* mem, const ActorDecl& d, const ActorEnv& env) {
    static_assert(sizeof(PresentActor) <= kActorSlotBytes, "PresentActor outgrew its actor slot");
    PresentConfig c{};
    if (env.node == nullptr || !present_config(d, c)) return nullptr;
    return new (mem) PresentActor(*env.node, c, env.board);
}

bool present_placement(const ActorDecl& d, TickerConfig& out) {
    PresentConfig c{};
    if (!present_config(d, c)) return false;
    out = c.place;
    out.need_caps = kHelloCapConnected;  // only a board on a network can hold the role (the review's item 3)
    return true;
}

size_t present_outputs(const ActorDecl& d, uint16_t, uint16_t peer, NsDecl* out, size_t cap) {
    PresentConfig c{};
    if (peer != 0 || cap == 0 || !present_config(d, c)) return 0;
    NsDecl x;
    x.path_hash = c.place.out_hash;  // act/<name>/entities: how many resources the cell presents now
    x.type = ValueType::I32;
    x.unit = Unit::Count;
    x.kind = ResourceKind::Sampled;
    x.access = Access::Read;
    x.latency_class = kClassL4;
    x.staleness_bound_ms = 5u * c.place.period_ms;
    x.staleness_policy = StalenessPolicy::Informative;
    out[0] = x;
    return 1;
}

}  // namespace

const ActorKind kPresentKind = {ActorType::Present, "present", &present_check, &present_create, &present_placement,
                                &present_outputs};

void print_status() {
    std::printf("{\"t\":\"present\",\"holding\":%d,\"connected\":%d,\"connects\":%lu,\"configs\":%lu,\"states\":%lu,"
                "\"writes_sent\":%lu,\"writes_applied\":%lu,\"writes_refused\":%lu,\"cmds_dropped\":%lu,"
                "\"enqueue_fail\":%lu,\"errors\":%lu,\"published\":%lu,\"publish_fail\":%lu,\"pending\":%u,"
                "\"int_free\":%u,\"int_largest\":%u,"
                "\"int_min_free\":%u}\n",
                g.holding.load() ? 1 : 0, g.connected.load() ? 1 : 0, static_cast<unsigned long>(g.connects.load()),
                static_cast<unsigned long>(g.configs), static_cast<unsigned long>(g.states),
                static_cast<unsigned long>(g.writes_sent), static_cast<unsigned long>(g.writes_applied),
                static_cast<unsigned long>(g.writes_refused), static_cast<unsigned long>(g.cmds_dropped.load()),
                static_cast<unsigned long>(g.enqueue_fail), static_cast<unsigned long>(g.errors.load()),
                static_cast<unsigned long>(g.published.load()), static_cast<unsigned long>(g.publish_fail.load()),
                g.out != nullptr ? static_cast<unsigned>(kOutLen - xMessageBufferSpacesAvailable(g.out)) : 0u,
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

bool handle_console(const char* line, size_t len) {
    static const char kPrefix[] = "POT! present ";
    constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (len < kPrefixLen || std::memcmp(line, kPrefix, kPrefixLen) != 0) return false;
    char buf[200];
    if (len - kPrefixLen >= sizeof(buf)) {
        std::printf("{\"t\":\"present_cfg\",\"result\":\"too_long\"}\n");
        return true;
    }
    std::memcpy(buf, line + kPrefixLen, len - kPrefixLen);
    buf[len - kPrefixLen] = '\0';
    for (size_t n = std::strlen(buf); n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n'); --n) buf[n - 1] = '\0';
    const char* result = "usage";
    nvs_handle_t h;
    char* sp = std::strchr(buf, ' ');
    if (std::strcmp(buf, "forget") == 0) {
        result = "erase_failed";
        if (nvs_open(kNvsNs, NVS_READWRITE, &h) == ESP_OK) {
            if (nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK) result = "forgotten";
            nvs_close(h);
        }
    } else if (sp != nullptr && std::strncmp(buf, "login ", 6) == 0) {
        char* user = buf + 6;
        char* sp2 = std::strchr(user, ' ');
        if (sp2 != nullptr) {
            *sp2 = '\0';
            const char* pass = sp2 + 1;
            if (std::strlen(user) < 1 || std::strlen(user) > 64 || std::strlen(pass) < 1 || std::strlen(pass) > 64) {
                result = "refused_length";
            } else {
                result = "save_failed";
                if (nvs_open(kNvsNs, NVS_READWRITE, &h) == ESP_OK) {
                    if (nvs_set_str(h, "user", user) == ESP_OK && nvs_set_str(h, "pass", pass) == ESP_OK &&
                        nvs_commit(h) == ESP_OK) {
                        result = "login_saved";
                    }
                    nvs_close(h);
                }
            }
        }
    }
    std::memset(buf, 0, sizeof(buf));
    std::printf("{\"t\":\"present_cfg\",\"result\":\"%s\"}\n", result);  // the result only, never the arguments
    return true;
}

}  // namespace pot::present
