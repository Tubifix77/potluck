// M14: the presenting role on the PC's placement node. See host_present.hpp.
//
// Where it builds what is published it follows the board's PresentActor (firmware/components/pot_present/src/
// present.cpp) step for step -- discovery a few configs a pass, a READ of everything first, states then the presenter's
// availability with its live and down lists, the outside write checked by the package's rule and then by the owner --
// so Home Assistant sees the same configs, ids and topics whichever node presents. A change to one is a change to both.

#include "host_present.hpp"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <deque>
#include <new>
#include <string>

#include "pot/node.hpp"
#include "pot/present_core.hpp"

using namespace pot;
using namespace pot::present;

namespace hostpresent {

namespace {

constexpr size_t kBufLen = 1536;
constexpr uint32_t kRediscoverMs = 300000;
constexpr size_t kConfigsPerTick = 2;

struct Cmd {
    uint32_t hash;
    std::string payload;
};

struct State {
    Services svc;
    bool connected = false;
    bool need_discovery = false;
    bool holding = false;
    std::deque<Cmd> cmds;
    uint16_t presenters[kMaxEligible] = {};
    size_t n_presenters = 0;
    char buf[kBufLen];
    char topic[112];
};
State g;

std::string b64(const uint8_t* p, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(p[i]) << 16) | (i + 1 < n ? static_cast<uint32_t>(p[i + 1]) << 8 : 0) |
                           (i + 2 < n ? p[i + 2] : 0);
        s += t[(v >> 18) & 63];
        s += t[(v >> 12) & 63];
        s += i + 1 < n ? t[(v >> 6) & 63] : '=';
        s += i + 2 < n ? t[v & 63] : '=';
    }
    return s;
}

// A JSON string's characters: the topics and names here are printable ASCII (the package says so), so only the quote
// and the backslash need escaping.
std::string js(const char* s) {
    std::string o;
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\') o += '\\';
        o += *s;
    }
    return o;
}

void emit_publish(const char* topic, const char* data, size_t len) {
    if (!g.connected || g.svc.emit == nullptr) return;
    const std::string line = std::string("{\"t\":\"mqtt\",\"k\":\"P\",\"topic\":\"") + js(topic) + "\",\"b64\":\"" +
                             b64(reinterpret_cast<const uint8_t*>(data), len) + "\"}";
    g.svc.emit(line.c_str());
}

Cell cell() {
    Cell c;
    c.id = g.svc.cell_id;
    c.system = g.svc.system;
    c.prefix = g.svc.prefix;
    c.presenters = g.presenters;
    c.n_presenters = g.n_presenters;
    return c;
}

uint64_t unix_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

bool rule(size_t i, uint32_t& hash, uint8_t& flags, const char*& name, size_t& len) {
    if (g.svc.img == nullptr || i >= g.svc.img->outside_count) return false;
    outside_rule(*g.svc.img, i, hash, flags, &name, &len);
    return true;
}

class HostPresentActor : public Actor {
  public:
    HostPresentActor(Node& node, const PresentConfig& cfg) : node_(node), cfg_(cfg) {}

    bool start(uint32_t now_ms) override {
        if (g.svc.img == nullptr || g.svc.emit == nullptr) return false;
        const uint16_t self = node_.config().node_id;
        g.n_presenters = cfg_.place.count;
        for (size_t i = 0; i < g.n_presenters; ++i) g.presenters[i] = cfg_.place.node[i];
        const Cell cl = cell();
        char filter[48], status[48], will_topic[64], will_msg[64];
        command_filter(cl, filter, sizeof(filter));
        std::snprintf(status, sizeof(status), "%s/status", g.svc.prefix);
        presenter_topic(cl, self, will_topic, sizeof(will_topic));
        will_payload(self, will_msg, sizeof(will_msg));
        const bool tls = (cfg_.flags & kPresentTls) != 0;
        if (tls && (g.svc.img->broker_ca == nullptr || g.svc.img->broker_ca_len == 0)) {
            std::fprintf(stderr, "{\"t\":\"present_error\",\"why\":\"TLS asked for and no broker CA in the package\"}\n");
            return false;
        }
        char host[16], client_id[40];
        std::snprintf(host, sizeof(host), "%u.%u.%u.%u", cfg_.ip[0], cfg_.ip[1], cfg_.ip[2], cfg_.ip[3]);
        std::snprintf(client_id, sizeof(client_id), "potluck-%s-%04x", g.svc.cell_id, static_cast<unsigned>(self));
        char head[512];
        std::snprintf(head, sizeof(head),
                      "{\"t\":\"mqtt\",\"k\":\"S\",\"host\":\"%s\",\"port\":%u,\"tls\":%d,\"client_id\":\"%s\","
                      "\"keepalive_s\":%u,\"will_topic\":\"%s\",\"will_b64\":\"",
                      host, static_cast<unsigned>(cfg_.port), tls ? 1 : 0, client_id,
                      static_cast<unsigned>(cfg_.keepalive_s), js(will_topic).c_str());
        std::string line = head + b64(reinterpret_cast<const uint8_t*>(will_msg), std::strlen(will_msg)) +
                           "\",\"cmd_filter\":\"" + js(filter) + "\",\"status_topic\":\"" + js(status) +
                           "\",\"ca_b64\":\"" + (tls ? b64(g.svc.img->broker_ca, g.svc.img->broker_ca_len) : "") + "\"}";
        g.connected = false;
        g.need_discovery = false;
        g.cmds.clear();
        g.holding = true;
        g.svc.emit(line.c_str());
        next_ms_ = now_ms + 500;
        primed_ = false;
        rediscover_ms_ = now_ms + kRediscoverMs;
        return true;
    }

    ~HostPresentActor() override {
        g.holding = false;
        if (g.svc.emit != nullptr) g.svc.emit("{\"t\":\"mqtt\",\"k\":\"X\"}");  // the transport says goodbye
        g.connected = false;
    }

    void tick(uint32_t now) override {
        while (!g.cmds.empty()) {
            const Cmd c = g.cmds.front();
            g.cmds.pop_front();
            handle(c, now);
        }
        if (!g.connected) return;
        if (g.need_discovery) {
            g.need_discovery = false;
            disc_cursor_ = 0;
            discovering_ = true;
        }
        if (static_cast<int32_t>(now - rediscover_ms_) >= 0) {
            rediscover_ms_ = now + kRediscoverMs;
            disc_cursor_ = 0;
            discovering_ = true;
        }
        if (discovering_) discover_some(now);
        if (!primed_) {
            primed_ = true;
            prime();
            if (static_cast<int32_t>(next_ms_ - (now + 500)) < 0) next_ms_ = now + 500;
        }
        if (static_cast<int32_t>(now - next_ms_) < 0) return;
        next_ms_ = now + cfg_.place.period_ms;
        publish_all();
    }

    size_t stats_json(char* buf, size_t cap, uint32_t) override {
        const int n = std::snprintf(buf, cap,
                                    "{\"t\":\"present_actor\",\"node\":%u,\"connected\":%d,\"entities\":%u,"
                                    "\"configs\":%lu,\"states\":%lu}",
                                    static_cast<unsigned>(node_.config().node_id), g.connected ? 1 : 0,
                                    static_cast<unsigned>(entities_), static_cast<unsigned long>(configs_),
                                    static_cast<unsigned long>(states_));
        return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
    }

  private:
    bool find_rule(uint32_t hash, uint8_t& flags, const char*& name, size_t& len) const {
        uint32_t h = 0;
        for (size_t i = 0; rule(i, h, flags, name, len); ++i) {
            if (h == hash) return true;
        }
        name = "";
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
                if (missed_) rediscover_ms_ = now + 10000;
                missed_ = false;
                return;
            }
            ++disc_cursor_;
            if ((flags & kOutsideExposed) == 0) continue;
            Reading r;
            node_.read(hash, r);
            const NsEntry* e = node_.ns().find(hash);
            if (e == nullptr) {
                missed_ = true;
                continue;
            }
            const Entity en = entity_of(*e, name, len);
            if (component_of(en) == Component::None) {
                if (e->value.type == ValueType::None) missed_ = true;
                continue;
            }
            const size_t n = config_payload(cl, en, cfg_.place.period_ms, g.buf, kBufLen);
            if (n == 0 || config_topic(cl, en, g.topic, sizeof(g.topic)) == 0) continue;
            emit_publish(g.topic, g.buf, n);
            ++configs_;
        }
    }

    void publish_all() {
        const Cell cl = cell();
        const uint16_t self = node_.config().node_id;
        uint32_t down[kMaxOutside];
        size_t n_down = 0;
        size_t entities = 0;
        const uint64_t now_unix = unix_ms();
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
            if (!local && e->owner_node != 0 && e->owner_node != self) node_.request_read(e->owner_node, hash);
            const StalenessPolicy pol = static_cast<StalenessPolicy>(e->staleness_policy);
            if (!shows_value(r, pol) && n_down < kMaxOutside) down[n_down++] = hash;
            const size_t n = state_payload(r, pol, now_unix, g.buf, kBufLen);
            if (n != 0 && state_topic(cl, hash, g.topic, sizeof(g.topic)) != 0) {
                emit_publish(g.topic, g.buf, n);
                ++states_;
            }
        }
        uint16_t live[kMaxPeers + 1];
        size_t n_live = 0;
        live[n_live++] = self;
        for (size_t i = 0; i < PeerTable::capacity() && n_live < kMaxPeers + 1; ++i) {
            const PeerLink& p = node_.peers().slot(i);
            if (p.state == PeerState::Alive && p.node_id != 0) live[n_live++] = p.node_id;
        }
        const size_t n = presenter_payload(self, live, n_live, down, n_down, g.buf, kBufLen);
        if (n != 0 && presenter_topic(cl, self, g.topic, sizeof(g.topic)) != 0) emit_publish(g.topic, g.buf, n);
        entities_ = entities;
        node_.publish(cfg_.place.out_hash, Value::of_i32(static_cast<int32_t>(entities)));
    }

    void handle(const Cmd& c, uint32_t now) {
        uint8_t flags = 0;
        const char* name = "";
        size_t len = 0;
        const char* result = "sent to its owner";
        Value v;
        const NsEntry* e = node_.ns().find(c.hash);
        const size_t pl = c.payload.size();
        if (!find_rule(c.hash, flags, name, len) || (flags & kOutsideWritable) == 0 || (flags & kOutsideSafety) != 0) {
            result = "refused: not writable from outside";
        } else if (e == nullptr) {
            result = "refused: not known on the presenting board";
        } else if (pl == 0 || pl >= 24 || !parse_command_value(e->value.type, c.payload.data(), pl, v)) {
            result = "refused: not a value of its type";
        } else if (e->owner_node == node_.config().node_id) {
            const NsError st = node_.ns().write_outside(c.hash, v, now);
            result = st == NsError::Ok ? "applied" : "refused by the owner";
        } else if (e->owner_node == 0 || node_.request_write(e->owner_node, c.hash, v, true) == 0) {
            result = "refused: its owner is not reachable";
        }
        const Cell cl = cell();
        const size_t n = write_event(name, len, c.hash, c.payload.data(), pl < 24 ? pl : 0, result, g.buf, kBufLen);
        if (n != 0 && event_topic(cl, g.topic, sizeof(g.topic)) != 0) emit_publish(g.topic, g.buf, n);
    }

    void prime() {
        const uint16_t self = node_.config().node_id;
        uint32_t hash = 0;
        uint8_t flags = 0;
        const char* name = nullptr;
        size_t len = 0;
        for (size_t i = 0; rule(i, hash, flags, name, len); ++i) {
            if ((flags & kOutsideExposed) == 0) continue;
            Reading r;
            node_.read(hash, r);
            const NsEntry* e = node_.ns().find(hash);
            if (e != nullptr && e->owner_node != 0 && e->owner_node != self) node_.request_read(e->owner_node, hash);
        }
    }

    Node& node_;
    PresentConfig cfg_;
    uint32_t next_ms_ = 0;
    uint32_t rediscover_ms_ = 0;
    size_t disc_cursor_ = 0;
    bool discovering_ = false;
    bool missed_ = false;
    bool primed_ = false;
    size_t entities_ = 0;
    unsigned long configs_ = 0, states_ = 0;
};

bool check(const ActorDecl& d, const ActorEnv&, const char** why) {
    PresentConfig c{};
    if (d.node_id != kPortableNode || !present_config(d, c)) {
        *why = "present must be portable, with a broker address, port and a keep-alive of 5..120 s";
        return false;
    }
    return true;
}

Actor* create(void* mem, const ActorDecl& d, const ActorEnv& env) {
    PresentConfig c{};
    if (env.node == nullptr || !present_config(d, c)) return nullptr;
    return new (mem) HostPresentActor(*env.node, c);
}

// Exactly the board's present_placement: every node must rank the role alike, or two hold it (M0-LOG session 42's
// portable_header lesson).
bool placement(const ActorDecl& d, TickerConfig& out) {
    PresentConfig c{};
    if (!present_config(d, c)) return false;
    out = c.place;
    out.need_caps = kHelloCapConnected;
    out.avoid_caps = kHelloCapBusy;
    return true;
}

size_t outputs(const ActorDecl& d, uint16_t, uint16_t peer, NsDecl* out, size_t cap) {
    PresentConfig c{};
    if (peer != 0 || cap == 0 || !present_config(d, c)) return 0;
    NsDecl x;
    x.path_hash = c.place.out_hash;
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

void set_services(const Services& s) { g.svc = s; }
void on_connected(bool c) {
    g.connected = c && g.holding;
    if (g.connected) g.need_discovery = true;
}
void on_discovery() { g.need_discovery = true; }
void on_command(uint32_t hash, const char* payload, size_t len) {
    if (!g.holding || g.cmds.size() >= 8) return;
    g.cmds.push_back(Cmd{hash, std::string(payload, len)});
}

const ActorKind kHostPresentKind = {ActorType::Present, "present", &check, &create, &placement, &outputs};

}  // namespace hostpresent
